#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<cassert>
#include<sys/epoll.h>

#include"./locker.h"
#include"./threadpool.h"
#include"./http_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

//添加文件描述符到内核事件集
extern void addfd(int epollfd,int fd,bool one_shot);
//移除--在TCP连接关闭时，移除后，关闭TCP连接
extern void removefd(int epollfd,int fd);

//添加信号处理函数，主要是处理SIGPIPE信号
//1.往关闭管道读端的写端fd[1]写就会触发SIGPIPE
//2.对一个已经关闭或者不存在的TCP连接，写数据，第一次写会返回RST，第二次写就会触发SIGPIPE信号，默认操作是结束当前进程
/* 以上情况可能会导致客户端意外关闭，服务端也会跟着关闭
 * pthread线程库对信号的捕捉是逐线程的，(难道不应该是线程有自己的信号屏蔽字，共享信号处理函数？？)
 * 可以专门设置一个线程，用来处理信号，决定将该信号发送给哪个线程
*/
void addsig(int sig,void(handler)(int),bool restart = true){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;    //设置当前信号集的信号处理函数
    if(restart){
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);   //初始化信号集，将信号集设置为所有信号的集合
    assert(sigaction(sig,&sa,NULL) != -1);    //设置该信号的信号粗粝函数
}

//向客户端发送错误信息
void show_error(int connfd,const char* info){
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int main(int argc,char* argv[]){
    if(argc <= 2){//argv[0]可执行文件名/main,argv[1]IP地址，argv[2]是端口号
        printf("usage: [%s ip port]\n",basename(argv[0]));//最后一个/的字符串内容
        return 1;
    }
    const char* ip = argv[1];
    char* port = argv[2];

    //忽略SIGPIPE信号
    addsig(SIGPIPE,SIG_IGN);//SIG_IGN表示忽略SIGPIPE那个注册的信号。

    //创建线程池，线程池内的对象，也就是往工作队列中添加的对象是http_conn
    threadpool<http_conn>* pool = NULL;
    try{//这里的语句有任何异常就执行下面的return  并发实现模式--生产者/消费者
        pool = new threadpool<http_conn>;         //新建线程池，包括-一组线程/工作队列/互斥锁/信号量
    }
    catch(...){
        return 1;
    }

    //预先为每个可能的客户连接分配一个http_conn对象，这样下标就可以当作是文件描述符
    http_conn* users = new http_conn[MAX_FD];
    assert(users);
    //记录当前的用户数量
    int user_count = 0;  

    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd >= 0);

    /* 默认关闭close时，是close调用立即返回，TCP模块负责将该socket对应的TCP发送缓冲区中残留的数据发送给对方 
     * 1,0--表示的是close调用在关闭TCP连接时，TCP模块将该socket对应的发送缓冲区数据直接丢弃，同时发送给对方一个复位报文段
     * 给服务器提供了一个异常终止连接的方法(对端会收到复位报文段)
     * 1,非0--阻塞--等待一段时间再关闭，如果超过时间未收到确认，则返回-1，且errno设置为EWOULDBLOCK
     *      非阻塞--直接返回，根据errno和返回值判断状态
    */
    struct linger tmp = {1,0};//1表示还有数据没发送完毕的时候容许逗留，0表示逗留时间
    setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));//即让没发完的数据发送出去后在关闭socket

    //绑定端口号，创建监听套接字--队列--已完成连接队列，未完成连接队列2次握手
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family = AF_INET;
    //address.sin_addr.s_addr = htonl(ip);
    inet_aton(ip,&address.sin_addr);
    address.sin_port = htons(atoi(port));

    ret = bind(listenfd,(struct sockaddr*)& address,sizeof(address));
    assert(ret >= 0);

    ret = listen(listenfd,5);
    
    //创建内核事件集
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    //添加listenfd到内核事件集中，监听连接事件
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd = epollfd;  //设置

    while(true){
        int number = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if((number < 0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }
        
        for(int i = 0;i < number;++i){
            int sockfd = events[i].data.fd;
            //如果是监听套接字，则accept取出一个已连接socket
            if(sockfd == listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);                
                int connfd = accept(listenfd,(struct sockaddr*)& client_address,&client_addrlength);
                if(connfd < 0){
                    printf("error is: %d\n",errno);
                    continue;
                }
                //判断当前的总用户数量，如果用户数量大于MAX_FD，也是内核允许当前进程最大打开文件描述符的数量，那么就不再
                if(http_conn::m_user_count >= MAX_FD){
                    show_error(connfd,"Internal server busy");
                    continue;
                }
                //初始化客户连接，user[connfd]表示当前客户连接，connfd就是已连接套接字，就直接是下标
                users[connfd].init(connfd,client_address);  //已连接套接字、客户端IP设置端口重用，再去初始化其他一些状态
                printf("sock_close\n");
            }
            //异常状态，或者对端关闭连接
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP |EPOLLERR)){
                //如果有异常，直接关闭客户连接
                printf("sock_exception_close\n");
                users[sockfd].close_conn();   //直接关闭连接close
            }
            //可读
            else if(events[i].events & EPOLLIN){
                //根据读的结果，决定将任务添加到线程池，还是关闭连接
                //先读，根据读操作返回的结果，true则将该http_conn对象添加到工作队列中去，由工作线程去处理事件
                //所以这里是由主线程完成读写，而将http_conn这一对象添加到工作队列中去，工作线程只负责解析接收缓冲区的数据
                //半同步/半反应堆模式
                //我认为这里更像是  同步模拟的Proactor模式，因为Reactor模式是主线程仅负责监听事件，读写、处理业务逻辑均是由工作线程完成
                if(users[sockfd].read()){
                    pool->append(users + sockfd);
                }
                else{
                    printf("sock_read_close\n");
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events & EPOLLOUT){
                //根据写的结果，决定是否关闭连接
                //写事件触发
                printf("write_main\n");
                //由主线程完成写，这个时候逻辑是工作线程处理完读取到的数据，并根据读取的数据情况，决定要写的响应
                //包括状态行、首部、空行、主体部分
                //返回结果的false-表示短连接
                //返回结果的true--表示长连接，这是根据请求和响应报文中首部字段的 Connection决定是长连接或者是短连接
                if(!users[sockfd].write()){
                    printf("sock_write_close\n");
                    users[sockfd].close_conn();
                }
                //这里如果是长连接，那么在写完后，就已经重新初始化完了
            }
            else
            {
                printf("close\n");
            }
        }

    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    return 0;
}