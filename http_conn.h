#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<sys/epoll.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<sys/stat.h>
#include<string.h>
#include<pthread.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/mman.h>
#include<stdarg.h>//可变参数需要的头文件
#include<errno.h>
#include"locker.h"
//http_conn对象的头文件
//http_conn是http表示http连接的对象，以及相关的处理
class http_conn
{
public:    
    static const int FILENAME_LEN = 200;//文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;//读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;//写缓冲区的大小
    /*HTTP请求方法，但我们仅支持GET*/
    enum METHOD{GET = 0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT,PATCH};
    /*解析客户请求时，主状态机所处的状态*/
    //主状态机，是在解析http请求时，处理的状态分别是1.解析请求行 2.头部行 3.主体行
    enum CHECK_STATE{CHECK_STATE_REQUESTLINE = 0,CHECK_STATE_HEADER,CHECK_STATE_CONTENT};
    /*服务器处理HTTP请求的可能结果*/
    //可能的处理结果，处理HTTP请求可能返回的结果
    enum HTTP_CODE{NO_REQUEST,GET_REQUEST,BAD_REQUEST,NO_RESOURCE,
                   FORBIDDEN_REQUEST,FILE_REQUEST,INTERNAL_ERROR,CLOSED_CONNECTION};

     /*行的读取状态*/   
     //从状态机，在主状态机内实现，用来在解析行时判断当前读取/解析的行的状态
     //分别是1.读取一整行 2.行错误，这时返回BAD_REQUEST(语法错误) 3.未读取完一整行，可能缓冲区满，没有读到所有数据，这时，监听EPOLLIN事件，等待可读，再继续读
     enum LINE_STATUS{LINE_OK = 0,LINE_BAD,LINE_OPEN};

public:
    http_conn(){}
    ~http_conn(){}

public:
    //初始化，包括清空缓冲区、一些值置0等操作
    void init(int sockfd,const sockaddr_in& addr);//初始化新接受的连接
    void close_conn(bool real_close = true);//关闭连接
    //实际工作线程运行的处理客户请求的操作
    void process();//处理客户请求    

    //读写操作，ET模式，均是非阻塞读写--文件描述法设置成非阻塞的
    bool read();//非阻塞读操作
    bool write();//非阻塞写操作

private:
    void init();//初始化连接
    //解析HTTP请求--
    HTTP_CODE process_read();//解析HTTP请求
    //根据解析请求的结果，填充HTTP应答，返回的结果是bool型变量，
    //true--keep_alive长连接，仅仅去初始化init即可，无需关闭TCP连接
    //false--colsed短连接，需要关闭TCP连接
    bool process_write(HTTP_CODE ret);//填充HTTP应答

    //下面这一组函数被process_read调用以分析HTTP请求--主状态机实现，用于状态转换
    HTTP_CODE parse_request_line(char* text);  //解析请求行
    HTTP_CODE parse_headers(char* text);       //解析首部行
    HTTP_CODE parse_content(char* text);       //解析主体行
    //处理相应，返回的均是http响应码
    HTTP_CODE do_request();         
    //从接收缓冲区中取数据，返回后面的未解析的数据
    char* get_line(){return m_read_buf + m_start_line;}
    //解析行，从状态机
    LINE_STATUS parse_line();

    //下面这一组函数被process_write调用以填充HTTP应答
    void unmap();    //将开辟的空间释放掉(已经写到发送缓冲区后)
    //往响应报文中添加响应
    bool add_response(const char* format,...);//可以允许参数个数的不确定
    bool add_content(const char* content);    //添加主体部分
    bool add_status_line(int status,const char* title);  //添加状态行，要有状态码
    bool add_headers(int content_length);     //添加首部
    bool add_content_length(int content_len);
    //首部信息只有Connection、Content-Length、
    bool add_linger();     //表示是否是长连接--Connection首部字段
    bool add_blank_line(); //添加空行-表示的是首部后会有一个空行，然后后面才是实体主体部分

public:
    /*所有socket上的事件都被注册到同一个epoll内核事件表中，所以将epoll文件描述符设置为静态的*/
    static int m_epollfd;
    static int m_user_count;//统计用户数量

private:
    //该HTTP连接的socket和对方的socket地址
    int m_sockfd;
    sockaddr_in m_address;

    char m_read_buf[READ_BUFFER_SIZE];//读缓冲区
    int m_read_idx;//标识读缓冲区已经读入的客户数据的最后一个字节的下一个位置
    //标识正在分析的字符在读缓冲区的位置
    int m_checked_idx;//当前正在分析的字符在读缓冲区中的位置
    //正在解析的当前行的初始位置
    int m_start_line;//当前正在解析的行的初始位置
    //写缓冲区的位置-写缓冲区待发送的字节数
    char m_write_buf[WRITE_BUFFER_SIZE];//写缓冲区
    int m_write_idx;//写缓冲区中待发送的字节数

    //记录主状态机的当前状态
    CHECK_STATE m_check_state;//主状态机当前所处的状态
    METHOD m_method;//请求方法 方法 url 版本--get www.baidu.com/index.html http1.1
    //客户请求的目标文件的完整路径，其内容等于doc_root+m_url,doc_root是网站根目录
    char m_real_file[FILENAME_LEN]; //标识所请求文件的完整路径，均在某一根目录下
    char* m_url;//客户请求的目标文件的文件名
    char* m_version;//HTTP协议版本号，我们仅支持HTTP/1.1
    char* m_host;//主机名--请求报文首部
    //用于解析/读取实体主体内容，我觉得可以避免TCP粘包
    int m_content_length;//HTTP请求的消息体的长度--这个字段很重要
    bool m_linger;//HTTP请求是否要求保持连接--最终写完成后，根据返回的状态，决定是否是长连接

    //mmap申请一段内存空间，客户所请求的文件被映射到该内存空间，写到写缓冲区--snprintf
    //写完后，用umap删除这段内存空间
    char* m_file_address;//客户请求的目标文件被mmap到内存中的起始位置
    //获取目标文件的状态，决定返回状态码，如果是目录--400/文件不可读--403/文件不存在--404
    struct stat m_file_stat;//目标文件的状态。通过它我们可以判断文件是否存在/是否为目录/是否可读，并获得文件大小等信息
    //我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量
    
    /* 应答：1.1状态行 2.多个首部字段 3.1空行 4.主体(请求文档内容)
     * do_request中，可以将状态行、首部、空行，写到一块内存中，然后将主体写到另外一块内存中(采用mmap)
     * 并不需要将这两块内容拼接成一块之后，再一起写到fd，套接字描述符，写给客户端，使用writev可以集中写
    */
    struct iovec m_iv[2];
    int m_iv_count;   //表示被写的内存块的数量
};
#endif