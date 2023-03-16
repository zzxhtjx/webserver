#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include"locker.h"
#include"threadpool.h"
#include"http_conn.h"
#include<signal.h>
#include<iostream>
using namespace std;

#define MAX_FD 65536
#define MAX_EVENT_NUM 10000 //监听最多
void addsig(int sig, void(*handler)(int)){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

extern void addfd(int epollfd, int fd, int one_shot);

extern void removefd(int epollfd, int fd);

extern void modfd(int epollfd, int fd, int ev);


int main(int argc, char* argv[]){
    if(argc <= 1){
        printf("按照如下命令运行 : %s post_number\n",basename(argv[0]));
        exit(-1);
    }

    int port = atoi(argv[1]);

    //对sigpie信号进行处理
    addsig(SIGPIPE, SIG_IGN);

    threadpool<http_conn>* pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    }catch(...){
        exit(-1);
    }

    //保存所有的客户端的信息
    http_conn* users = new http_conn[MAX_FD];
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    //绑定之前设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    listen(listenfd, 5);//监听
    //监听
    epoll_event events[MAX_EVENT_NUM];
    int epollfd = epoll_create(5);

    
    //监听文件添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epoll_fd = epollfd;
    int cnt = 0;
    while(true){
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUM, -1);
        if((num < 0) && (errno != EINTR)){
            printf("epoll filure\n");
            break;
        }
        for(int i = 0; i < num; i++){
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int confd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                if(confd < 0){
                    printf("%d : confd\n",confd);
                    continue;//因为这个的话是没有进行连接啊
                } 
                if(http_conn::m_user_count >= MAX_FD){
                    //连接满了
                    //系欸一个信息
                    close(sockfd);
                    continue;
                }
                users[confd].init(confd, client_address);
                continue;
            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                users[sockfd].close_conn();
            }else if(events[i].events & EPOLLIN){
                if(users[sockfd].read()){
                    pool->append(users + sockfd);
                }else {
                    users[sockfd].close_conn();
                }
            }else if(events[i].events & EPOLLOUT){
                if(!users[sockfd].write()){
                    users[sockfd].close_conn();
                }
            }
            //初始化新的客户数据
            //否则的话就是正常的客户端请求，需要加入到工作队列
        }
    }
    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;
    return 0;
}
