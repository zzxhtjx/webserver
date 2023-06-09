#include"http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/nowcoder/webserver/resources";

int http_conn::m_epoll_fd = -1;
int http_conn::m_user_count = 0;
//添加需要进行监听的文件描述符
void addfd(int epollfd, int fd, int one_shot){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN |EPOLLRDHUP;
    if(one_shot){
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符非阻塞
    int flag = fcntl(fd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flag);//取消文件描述符阻塞
}

void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::process(){
    //解析http请求

    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_QUEST ) {
        modfd( m_epoll_fd, m_sockfd, EPOLLIN );
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epoll_fd, m_sockfd, EPOLLOUT);//ONESHOT需要进行重置
}

void  http_conn::init(int sockfd, const struct sockaddr_in addr){
    m_sockfd = sockfd;
    m_address = addr;
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    addfd(m_epoll_fd, m_sockfd, true);
    m_user_count++;

    init();
}

void  http_conn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;//解析首行
    m_linker = false;

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_check_index = 0;
    m_read_index = 0;
    m_write_idx = 0;
    bzero(m_read_buf, READ_BUF_SIZE);
    bzero(m_write_buf, READ_BUF_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

void  http_conn::close_conn(){
    if(m_sockfd != -1){
        m_user_count--;
        removefd(m_epoll_fd, m_sockfd); 
        m_sockfd = -1;
    }
}

bool  http_conn::read(){
    if(m_read_index >= READ_BUF_SIZE)   return false;
    int bytes = 0;
    while(true){
        bytes = recv(m_sockfd, m_read_buf + m_read_index, READ_BUF_SIZE - m_read_index, 0 );
        if(bytes == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break;//没有数据
            }
            return false;
        }else if(bytes == 0){
            //关闭连接
            return false;
        }
        m_read_index += bytes;
    }
    printf("读取到了数据 : %s\n", m_read_buf);
    return true;
}

void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

bool  http_conn::write(){
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epoll_fd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epoll_fd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linker) {
                init();
                modfd( m_epoll_fd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epoll_fd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

//主状态机
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_QUEST;
    char* text = 0;
    while( ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
    || (line_status = parse_line()) == LINE_OK){
        //解析到了一行数据,或者是解析到了请求体 

        text = get_line();

        m_start_line = m_check_index; 
        printf("got 1 http line : %s\n", text);
        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {    
                ret = parse_request_line(text);
                if(ret == BAD_QUEST){
                    return BAD_QUEST;
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret == BAD_QUEST){
                    return BAD_QUEST;
                }else if(ret == GET_QUEST){
                    return do_quest();
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == GET_QUEST){
                    return do_quest();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_QUEST; 
}

//解析http请求行,获得请求方法,目标URL,HTTP
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    //GET / HTTP/1.1
    m_url = strpbrk(text, " \t");
    *m_url++ = '\0';
    
    char* method = text;
    if(strcasecmp(method, "GET") == 0){
        m_method = GET;
    }else {
        return BAD_QUEST;
    }

    m_version = strpbrk(m_url, " \t");
    if(!m_version)  return BAD_QUEST;

    *m_version++ = '\0';
    if(strcasecmp(m_version, "HTTP/1.1") != 0){
        return BAD_QUEST;
    }

    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if(!m_url || m_url[0] != '/'){
        return BAD_QUEST;
    }

    m_check_state = CHECK_STATE_HEADER;//   检测状态变成检查
    return NO_QUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char* text){//本身传入的就是一行了
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_QUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_QUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linker = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_QUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char* text){
    //分析请求体
    if ( m_read_index >= ( m_content_length + m_check_index ) )
    {
        text[ m_content_length ] = '\0';
        return GET_QUEST;
    }
    return NO_QUEST;
}

//解析一行 \r\n
http_conn::LINE_STATUS http_conn::parse_line(){
    char tmp;
    
    for(; m_check_index < m_read_index; ++m_check_index){
        tmp = m_read_buf[m_check_index];
        if(tmp == '\r'){
            if((m_check_index + 1) == m_read_index){
                return LINE_OPEN;
            }else if(m_read_buf[m_check_index + 1] == '\n'){
                m_read_buf[m_check_index++] = '\0';
                m_read_buf[m_check_index++] = '\0';  
                return LINE_OK;
            }
        }else if(tmp == '\n'){
            if((m_check_index > 1) && (m_read_buf[m_check_index] == '\r')){
                m_read_buf[m_check_index - 1] = 0;
                m_read_buf[m_check_index++] = 0;
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

http_conn::HTTP_CODE http_conn::do_quest(){
    //获取资源,然后返回进行写
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_QUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUF_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUF_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUF_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linker == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_QUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}
