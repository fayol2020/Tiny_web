#include"http_conn.h"

//定义http响应的一些状态信息
//200 OK
//400 Bad Request--解析请求报文出现错误，语法错误
//403 Forbidden--请求的内容没有访问/读权限或者是目录
//404 Not Found--没有在服务器相关目录找到请求的文件
//500 Internal Error--解析请求行时出现了一些其他的未知错误
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "You request has had syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
//网站的根目录，所有请求的文件均存放在当前目录下
const char* doc_root = "/var/www/html";

//将文件描述符设置成非阻塞的
int setnonblocking(int fd){//将文件描述符设置为非阻塞(边缘触发搭配非阻塞)
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option| O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

//向内核事件表中添加文件描述符，以及注册可读事件/对端异常关闭事件、ET模式以及决定是否采用EPOLLONESHOT
//可以避免在当前线程正在处理某个套接字的数据，此时又有新数据到来，那么EPOLLIN可读(即使是ET模式依然如此)
//这时又会唤醒一个线程，读数据处理数据等，这样并不合理，两个线程同时处理一个socket的数据
//应该设置成EPOLLONESHOT，当前fd上的事件(读/写/异常)只会被触发一次，就不会出现上面那种情况
//但是一定要注意处理完事件后重置EPOLLONESHOT，使得事件还可以被触发，不然就不会被触发了
void addfd(int epollfd,int fd,bool one_shot){//向epoll例程中注册监视对象文件描述符
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;//注册三种事件类型
    if(one_shot){
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

//从内核事件表中关闭该fd，同时close(fd)，关闭TCP连接
void removefd(int epollfd,int fd){//移除并关闭文件描述符
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//修改事件，重置事件，处理完后，重置当前事件
void modfd(int epollfd,int fd,int ev){//重置事件，更改文件描述符，可以接受ev对应的读/写/异常事件
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn :: m_user_count = 0;//用户数量
int http_conn :: m_epollfd = -1;

//关闭连接，移除fd，closefd，user_count--，客户数量一定要-1
//重置当前的m_sockfd-套接字描述符
void http_conn :: close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;//关闭一个连接时，将客户总量减1
    }
}

//http_conn的初始化工作sockfd address，对端的ip地址
void http_conn :: init(int sockfd,const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    //以下两行为了避免TIME_WAIT状态--设置端口重用
    int reuse = 1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    //添加文件描述符
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    
    init();
}
//初始化读/写缓冲区、主从状态机初始状态--这一定是新来客户连接，或者是处理完一次客户请求，长连接，不关闭，重新初始化操作
void http_conn::init(){
    //主状态机初始化状态
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;    //默认是短连接

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    //接收缓冲区起始行位置
    m_start_line = 0;
    //当前正在分析的字节位置
    m_checked_idx = 0;
    //buffer中客户数据的尾部的下一字节
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}
//从状态机=>得到行的读取状态，分别表示1.读取一个完整的行LINE_OK，2.行出错LINE_BAD，3.行的数据尚且不完整LINE_OPEN
//http报文每一行都是以 '\r\n'结尾
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    /*m_checked_idx指向buffer中当前正在分析的字节，m_read_idx指向buffer中客户数据的尾部的下一字节。
    buffer中[0~m_checked_idx - 1]都已经分析完毕，下面的循环分析[m_checked_idx~m_read_idx- 1]的数据*/
    //每次分析buffer中一个字节
    for(;m_checked_idx < m_read_idx;++m_checked_idx){
        //获取当前要分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果当前字符是‘\t’则有可能读取一行
        if(temp == '\r'){
            //如果\t是buffer中最后一个已经被读取的数据，那么当前没有读取一个完整的行，还需要继续读
            if(m_checked_idx + 1 == m_read_idx){
                return LINE_OPEN;   //未读取到一个完整的行，还需要继续读取数据
            }
            //如果下一个字节是\n，那么已经读取到完整的行
            else if(m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';    //将'/r/n'均置为空，表示读取到了一行，那么去处理当前读取到的这行数据
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //否则的话，说明客户发送的HTTP请求存在语法问题---BAD_REQUEST
            return LINE_BAD;
        }
        //如果当前的字节是\n，也说明可能读取到一个完整的行
        else if(temp == '\n'){
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            else {
                return LINE_BAD;
            }
        }
    }
    //如果所有内容分析完毕也没遇到\r字符，则还需要继续读取客户数据
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read(){
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read=0;
    while(true)
    {
        //非阻塞读ET
        bytes_read = recv(m_sockfd,m_read_buf + m_read_idx,READ_BUFFER_SIZE - m_read_idx,0);
        if(bytes_read == -1)
        {
            //缓冲区满，等待再读 / 最后一次读，已经读取完
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        //返回为0，表示读到了文件尾，或者是FIN，关闭连接
        else if(bytes_read == 0){
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;    
}

//解析HTTP请求行，获得请求方法,目标URL，以及HTTP版本号   
// GET http://www.google.com:80/index.html HTTP/1.1
//主状态机 CHECK_STATE_REQUESTLINE
http_conn :: HTTP_CODE http_conn::parse_request_line(char* text){
    //在text中找到第一个该字符的位置，前面就是方法
    m_url = strpbrk(text," ");//A找到第一个等于空格或者制表符的下标位置GET
    if(!m_url){        
        return BAD_REQUEST;    //未找到语法错误
    }
    *m_url++ = '\0';//GET\0

    //通过找第一个空格位置，找到了方法GET
    char* method = text;
    if(strcasecmp(method,"GET") == 0){//忽略大小写比较大小
        m_method = GET;
    }
    else{
        return BAD_REQUEST;
    }
    //现在的m_url是get 后面的内容，strspn是检索字符串1中第一个不在字符串2中出现的字符的下标
    //返回的位置就是url的位置，去掉多余的空格影响
    m_url += strspn(m_url," ");//去掉GET后面多余空格的影响，找到其中最后一个空格位置
    //url 版本，找到空格，下一个位置就是版本
    m_version = strpbrk(m_url," ");//找到url的结束位置html和HTTP中间的空格位置
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';//html\0HTTP
    //要去掉多余的空格
    //这时的m_version一定是指向'H'的
    m_version += strspn(m_version," ");//去掉中间空格，此时m_version指向H,从状态机中已经将每行的结尾设置为\0\0
    if(strcasecmp(m_version,"HTTP/1.1") != 0){//仅支持HTTP/1.1
        return BAD_REQUEST;
    }
    //检查url的合法性
    if(strncmp(m_url,"HTTP://",7) == 0){//检查url是否合法
        m_url += 7;
        m_url = strchr(m_url,'/');//找/index中的/        
    }
    //现在的m_url指向的是'/'，也就是index.html前面的'/'
    if(!m_url || m_url[0] != '/'){//记住URL后缀是/
        return NO_REQUEST;
    }
    //主状态机的状态转移，也就是分析完了请求行字段，下面去分析首部行
    //HTTP请求行处理完毕，状态转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//分析头部字段
//HTTP请求的组成是1.一个请求行，2.后面跟随0个或者多个请求头，3.最后跟随一个空的文本行来终止报头列表
//每次是parse_line读到一行，然后去分析
//主状态机状态：CHECK_STATE_HEADER
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    //遇到空行，说明头部字段解析完毕
    if(text[0] == '\0')
    {
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;   //content-length字段不为0，说明有主体部分，那么状态转移
            return NO_REQUEST;           //返回的是NO_REQUEST，表示当前还未到写响应的时候
        }
        //否则说明我们得到一个完整的HTTP请求
        return GET_REQUEST;  //GET_REQUEST表示得到一个完整的HTTP请求
    }
    //下面处理多种请求报头
    //处理Connection头部字段--如果keep-alive，那么m_linger为true，表示为长连接
    else if(strncasecmp(text,"Connection:",11) == 0){
        text += 11;
        text += strspn(text," ");
        if(strcasecmp(text,"keep-alive") == 0){
            m_linger = true;
        }
    }
    //处理content-length头部字段
    else if(strcasecmp(text,"Content-Length:") == 0){
        text += 15;
        text += strspn(text," ");
        m_content_length = atol(text);
    }
    //处理Host头部信息--主机
    else if(strncasecmp(text,"Host:",5) == 0){
        text += 5;
        text += strspn(text," ");
        m_host = text;
    }
    else{
        printf("oop!unkonwn header %s\n",text);
    }

    return NO_REQUEST;
}

//我们没有真正的解析HTTP请求的消息体，只是判断它是否被完整的读入了
//主状态机状态：CHECK_STATE_CONTENT
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if(m_read_idx >= (m_content_length + m_checked_idx)){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//主状态机，用于从buffer中取出所有完整的行
//process_read，解析读取数据操作操作就是由该函数完成的
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;//记录当前行的读取状态
    HTTP_CODE ret = NO_REQUEST;//记录HTTP请求的处理结果--初始为NO_REQUEST，表示没有处理完，要继续处理
    char* text = 0;
    
    //当解析到完整的一行时，去根据主状态机状态解析该行内容，如果状态不是LINE_OK,说明还需要去读，或者有问题
    //如果当前是实体主体内容，那么从状态机的状态就不发生变化了，因为主体并不存在http报文的每一行最后的 '\r\n'格式
    //实体主体内容，只需要根据Content-Length的长度读就可以了
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OPEN))   //??????????
        || (line_status = parse_line()) == LINE_OK) {//m_check_state记录主状态机当前的状态
         text = get_line(); //获取刚读到的一行数据
         m_start_line = m_checked_idx;
         printf("got 1 http line:%s\n",text);
        
        //根据当前主状态机的状态，决定是应该分析什么字段
         switch(m_check_state){
             case CHECK_STATE_REQUESTLINE:{//分析请求行
                 ret = parse_request_line(text);
                 if(ret == BAD_REQUEST){   //返回的是BAD_REQUEST，说明请求报文的语法错误，返回400
                     return BAD_REQUEST;
                 }
                 break;
             }
             case CHECK_STATE_HEADER:{//分析头部字段
                ret = parse_headers(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                else if(ret == GET_REQUEST){     //GET_REQUEST分析完成，这时去写请求，这里意味着首部无content-length字段
                    return do_request();
                }
                break;
             }
             case CHECK_STATE_CONTENT:{//分析消息体
                 ret = parse_content(text);
                 if(ret == GET_REQUEST)
                 {
                     return do_request();        //写请求
                 }
                 line_status = LINE_OPEN;        //前面还未到写请求，说明，一定是还没有读取完，那么置从状态机为LINE_OPEN,
                 break;
             }
             default:{
                 return INTERNAL_ERROR;
             }

         }

    }
    //解析行状态为LINE_BAD，则BAD_REQUEST
    //if(line_status==LINE_BAD)
    //    return BAD_REQUEST;

    return NO_REQUEST;     //不满足while循环的条件，则返回NO_REQUEST表示还不能写，这样去事件集监听写事件
}

/*当得到一个完整正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，
对所有用户可读，且不是目录，则使用mmap将其映射内存地址m_file_address处，并告诉调用者获取文件成功*/
//分析完用户请求后，do_request响应之--去判断用户请求内容(文件类型、权限内容等)
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);
    //m_real_file客户请求的目标文件的完整路径，其内容等于doc_root + m_url,doc_root是网站根目录
    strncpy( m_real_file + len,m_url,FILENAME_LEN - len - 1);
    //m_read_file是用户请求的完整路径和文件名

    if(stat(m_real_file,&m_file_stat)){//获取文件的状态并保存在m_file_stat中
        return NO_RESOURCE;   //404，未找到请求的资源信息
    }
    //不可读，403
    if(!(m_file_stat.st_mode & S_IROTH)){
        return FORBIDDEN_REQUEST;
    }
    //是目录则400，表示语法错误，请求了一个目录
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }

    //打开文件，并映射到一块虚拟内存区域
    int fd = open(m_real_file,O_RDONLY);
    //创建虚拟内存区域，并将对象映射到这些区域
    m_file_address = (char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    //关闭文件描述符，从内存区域读取文件即可
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address,m_file_stat.st_size);//删除虚拟内存的区域
        m_file_address = 0;
    }
}

//写HTTP响应--本系统中是由主线程完成读写
bool http_conn::write(){
    int temp = 0;
    int bytes_have_send = 0;     //已经发送的字节数
    int bytes_to_send = m_write_idx;  //待发送的字节数
    if(bytes_to_send == 0){
        modfd(m_epollfd,m_sockfd,EPOLLIN);   //写完了，就重置等待读
        init();
        return true;
    }

    //集中写，就是将状态行、首部行放在一起，主体部分为另一块缓冲区，无需将其拷贝到同一块缓冲区，就可以直接写
    while(1){
        temp = writev(m_sockfd,m_iv,m_iv_count);   //m_iv_count，表示集中写的缓冲区的数量
        if(temp <= -1){
        //如果TCP写缓存没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但是可以保证连接的完整性
        //这里是当前写缓冲区无法写(满)，那么继续监听写事件，设置了EPOLLONESHOT，无法接收该客户的下一个请求
            if(errno == EAGAIN){//当前不可写
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();   //出现问题，释放掉区间，return false，关闭TCP连接
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        //将要发送的数量<=已经发送的字节数，也就是说
        if(bytes_to_send <= bytes_have_send){
            //发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger){   //保持长连接
                init();     //直接重新初始化当前对象
                modfd(m_epollfd,m_sockfd,EPOLLIN);   //继续监听可读事件
                return true;   //return true表示长连接
            }
            else{
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return false;
            }
        }
    }
}

//往写缓冲区写入待发送的数据，可变参数
bool http_conn::add_response(const char*format,...){
    if(m_write_idx >= WRITE_BUFFER_SIZE){   //写缓冲区满，则return false
        return false;
    }
    //以下是将format格式的字符串，以及后面的参数，组成新的字符串，替代%s这种
    va_list arg_list;
    va_start(arg_list,format);
    //将可变参数格式化输出到一个字符数组
    int len = vsnprintf(m_write_buf + m_write_idx,WRITE_BUFFER_SIZE - 1 - m_write_idx,format,arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)){   //超过了当前写缓冲区的剩余量
        return false;
    }
    m_write_idx += len;     //m_write_idx，是以及向写缓冲区写的字节数
    va_end(arg_list);
    
    return true;
}

//构建状态行、首部行、实体主体行 format  ...(可变参数--对应format中的%s %d这种)
bool http_conn::add_status_line(int status,const char* title){
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}
//只处理三种首部信息
bool http_conn::add_headers(int content_len){//头部就三种信息
    add_content_length(content_len);//内容长度 ---实体首部字段
    add_linger();//客户连接信息       //通用首部
    add_blank_line();//空行          //加空行，首部结束后
}
//content-len是当前的要发送的文件的大小--字节数
bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n",content_len);
}
//Connection字段
bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n",(m_linger == true) ? "keep-alive" : "close");
}
//首部后，主体前，添加一个空行
bool http_conn::add_blank_line(){
    return add_response("%s","\r\n");
}
//添加内容，将所有的content主体部分，添加到响应中去
bool http_conn::add_content(const char* content){
    return add_response("%s",content);
}

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
//ret就是状态码，也就是处理的响应结果
bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
        case INTERNAL_ERROR:{   //500内部错误，未知的其他问题
            add_status_line(500,error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)){
                return false;
            }
            break;
        }
        case BAD_REQUEST:{     //400语法错误，请求的是目录
            add_status_line(400,error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)){
                return false;
            }
            break;
        }
        case NO_RESOURCE:{    //404没有找到资源，stat错误
            add_status_line(404,error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)){
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:{   //403禁止访问，不可读
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)){
                return false;
            }
            break;
        }
        case FILE_REQUEST:{      //返回请求的实体主体部分
            add_status_line(200,ok_200_title);
            if(m_file_stat.st_size != 0){//st_size表示文件的大小
                add_headers(m_file_stat.st_size);  //添加首部信息
                //写缓冲区的内容，此前状态行和首部行已经被添加到了写缓冲区  add_status_line/add_headers
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                //文件内容和大小--字节数
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;    //写的缓冲区的数量为2
                return true;
            }
            else{   //请求的文件为空，那么根据html信息返回空的结构体就ok--1.状态行 2.首部行 3.主体行
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string)){    
                    return false;
                }
            }
            //没有break，是因为一定就返回了
        }
        default:{
            return false;
        }
        //除了FILE_REQUEST外的其他几种情况，均是没有文件内容，所以，只需要将状态和首部发送即可
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_idx;
        m_iv_count = 1;
        return true;
    }
}

//有线程池中的工作线程调用，这是处理HTTP请求的入口函数
/*
 * 写完成后，已经将实体主体内容放到了合适的缓冲区，并且返回HTTP_CODE，知道怎么填充状态行，首部字段已经设置好了private的m_linger字段
 * 并获取到了目标文件的状态，根据m_file_stat.st_size填充content-length，写只需要将这些内容写到响应的缓冲区，此外首部还需要填充空行即可
 * 而状态行已经给出了根据状态码填充的信息
*/
void http_conn::process(){
    //处理读事件
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){    //读事件返回的是NO_REQUEST，表示还应该继续读，继续监听
        modfd(m_epollfd,m_sockfd,EPOLLIN);  //重新监听可读事件，return，还没到写的时候，这也是重置了EPOLLONESHOT
        return;
    }
    //处理写事件---我觉得这里写的有问题，待会验证一下
    bool write_ret = process_write(read_ret);
    //处理写事件就是将待写的状态行、首部行、主体行写到缓冲区，一旦缓冲区空间大小小于待写的数据字节数，那么就返回false
    //表示需要继续监听EPOLLONESHOT事件，等待缓冲区不满，触发可写(ET)，LT是缓冲区有剩余空间就会触发写事件
    if(!write_ret){     //false应该是因为写的数据大于当前发送缓冲区大小，导致没有写完
         close_conn();  //直接关闭连接，不发送数据
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);  //因为，process_write仅仅是将该待写数据写到了写缓冲区位置，然后监听可写事件，等待触发，由主线程完成写操作
}

