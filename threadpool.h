#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<cstdio>
#include<pthread.h>
#include <list>
#include<exception>
#include "locker.h"

//代码复用使用模板类
template<typename T>
class threadpool{
public:
    threadpool(int thread_num = 8, int max_queuerequest = 10000);
    ~threadpool();
    bool append(T* request);
private:
    static void* worker(void* arg);
    void run();
private:
    //数量
    int m_thread_num;
    
    //数组静态资源
    pthread_t* m_threads;

    //请求队列
    int m_max_request;

    std::list< T*>m_work_queue;
    
    locker m_queuelocker;

    //信号量
    sem m_queuestat;

    //是否结束
    bool m_stop; 
};

template<typename T>
threadpool<T> ::threadpool(int thread_num , int max_queuerequest ):
m_thread_num (thread_num), m_max_request (max_queuerequest), m_stop(false), m_threads(NULL)
{
    if(thread_num <= 0 || max_queuerequest <= 0)    throw std::exception();
    
    m_threads = new pthread_t[m_thread_num]; 
    if(!m_threads)  throw std::exception();

    //初始化线程
    for(int i = 0; i < m_thread_num; i++){
        printf("create the %dth thread\n", i);  
        if(pthread_create(&m_threads[i], NULL, worker, this) != 0){
            delete [] m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i]) != 0){
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T> ::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T> ::append(T* request)
{
    m_queuelocker.lock();
    if(m_work_queue.size() > m_max_request){
        m_queuelocker.unlock();
        return false;
    }

    m_work_queue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template<typename T>
void* threadpool<T> ::worker(void* arg){
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return  pool;
}

template<typename T>
void threadpool<T> ::run(){
    while(!m_stop){//程序没有停止
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_work_queue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_work_queue.front();
        m_work_queue.pop_front();
        m_queuelocker.unlock();
        
        if(!request)    continue;

        request->process();//让任务进行
    }
}

#endif