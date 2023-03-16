#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>

class http_conn{
public:
    http_conn(){
    }
    ~http_conn(){
    }
    void process();
    void init(int sockfd, const struct sockaddr_in addr);
    static int m_epoll_fd;//epoll
    static int m_user_count;//用户数量
    static const int FILENAME_LEN = 200;        // 文件名的最大长度
    static const int READ_BUF_SIZE = 2048;
    static const int WRITE_BUF_SIZE = 1024;

    //解析的时候使用状态机进行处理
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};

    /*
    CHECK_STATE_REQUESTLINE:当前正在分析请求行
    CHECK_STATE_HEADER:当前正在分析头部字段
    CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};

    /*
    服务器解析请求的可能结果,报文解析的结果
        NO_QUEST        :   请求不完整,需要继续读取客户端数据
        GET_QUEST       :   表示获得了一个完整的客户请求
        BAD_QUEST       :   表示请求语法问题
        NO_RESOURSE     :   表示服务器没有资源
        FORBIDDEN_QUEST :   表示客户对资源没有足够的访问权限 
    */
    enum HTTP_CODE {NO_QUEST = 0, GET_QUEST, BAD_QUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };

    /*
    解析一行的状态
    1.完整 2.出错   3.不完整
    */
    enum LINE_STATUS  {LINE_OK = 0, LINE_BAD, LINE_OPEN};

    void close_conn();
    bool read();
    bool write();

    

private:
    int m_sockfd;//这个对象对应的套接字和连接地址
    sockaddr_in m_address;
    char m_read_buf[READ_BUF_SIZE];
    int m_read_index;
    
    char m_real_file[ FILENAME_LEN ];       // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    int m_check_index;      //现在检测到的位置
    int m_start_line;       //下一次检测的开始的行
    char* m_url;            //目标文件名
    char* m_version;        //协议版本
    METHOD m_method;        //需要请求的方法
    char* m_host;           //连接的IP
    int m_content_length;
    bool m_linker;          //判断是否保持连接

    char m_write_buf[ WRITE_BUF_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;
    CHECK_STATE m_check_state;//主状态机

    HTTP_CODE process_read();//解析http
    HTTP_CODE parse_request_line(char* text);//解析头
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    LINE_STATUS parse_line();    

    // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();
    bool process_write( HTTP_CODE ret );    // 填充HTTP应答

    void init();
    char* get_line(){ return m_read_buf + m_start_line;}
    HTTP_CODE do_quest();
};

#endif