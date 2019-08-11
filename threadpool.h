#ifndef THREADPOOL_H
#define THREADPOOL_H

//生产者消费者模式实现线程池
#include<list>
#include<cstdio>
#include<exception>
#include<pthread.h>
#include"locker.h"

//线程池类，把它定义为模板类是为了代码复用，模板参数T是任务类　
//Ｔ表示的是任务，也就是http_conn对象
template<typename T>
class threadpool{
public:
  /*参数thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的，等待处理的请求的数量*/
    threadpool(int thread_number = 8,int max_requests = 1000);
    ~threadpool();
    //往请求队列中添加任务
    bool append(T* request);

private:
    //工作线程运行的函数，它不断从工作队列中取出任务并执行之
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number;//线程池中的线程数
    int m_max_requests; //请求队列中允许的最大请求数
    //线程池数组大小
    pthread_t* m_threads;//描述线程池的数组，其大小为m_thread_number
    //请求队列，任务队列
    std::list<T*> m_workqueue;//请求队列
    //互斥锁，用于保护请求队列，请求队列属于共享资源
    locker m_queuelocker;//保护请求队列的互斥锁
    //信号量，用于线程阻塞和唤醒，创建线程池，运行worker，阻塞，然后主线程往队列中放任务后，唤醒之
    sem m_queuestat;//是否有任务需要处理
    bool m_stop;//是否结束线程
};
//线程池的构造函数，用于参数初始化等
template<typename T>
threadpool<T>::threadpool(int thread_number,int max_requests):
    m_thread_number(thread_number),m_max_requests(max_requests),
    m_stop(false),m_threads(NULL)

{    
    if(thread_number <= 0 || max_requests <= 0){
        throw std::exception();
    }
    //新建线程数组，存的是线程tid，每个线程一个
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw std::exception();
    }
    //创建thread_number个线程，并设置为分离线程
    for(int i = 0;i < thread_number;++i){
        printf("create the %dth thread\n",i + 1);
        //worker线程函数参数传递的是this，也就是当前线程池对象
        if(pthread_create(&m_threads[i],NULL,worker,this) != 0){
            delete [] m_threads;
            throw std::exception();
        }
        //创建线程，并将每个线程均设置为分离线程，所谓分离线程就是在运行结束了系统回收资源，不用其他线程回收该线程资源
        if(pthread_detach(m_threads[i]) != 0){
            delete [] m_threads;
            throw std::exception();
        }
    }
    
}

//线程池的析构函数，避免内存泄露
template<typename T>
threadpool<T> :: ~threadpool(){
    delete [] m_threads;
    throw std:: exception();
}

//将任务添加到工作队列中去，操作任务队列前，无论是添加元素还是删除元素，均要先加锁－－属于共享资源
template<typename T>
bool threadpool<T>::append(T* request){
    /*操作工作队列前一定要加锁，因为它被所有工作队列共享*/
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    //解锁后，因为确保队列非空，此时可以执行Ｖ操作，信号量值+1,可以唤醒一个线程
    m_queuestat.post();//保证队列非空
    return true;
}

template<typename T>
void* threadpool<T> :: worker(void* arg){//传入的是this
    threadpool* pool = (threadpool*)arg;//将void×指针转化为threadpool*指针
    //转换为threadpool指针后，运行run函数
    pool -> run();//就是下面实现的run函数
    return pool;
}

//线程实际运行的函数
template<typename T>
void threadpool<T>::run(){//消费者
    //该线程未终止，执行P操作，刚创建线程一定是阻塞之，使得线程睡眠在工作队列中
    while(!m_stop){
        m_queuestat.wait();//执行p操作
        //唤醒后，从工作队列取元素，要先加锁
        //操作等待队列(取元素，或添加元素)均一定要先加锁
        m_queuelocker.lock();//对工作队列操作钱加锁
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        //取出来元素，再解锁
        //T是任务对象，在本项目中就是http_conn对象
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){
            continue;
        }
        //执行任务的函数，也就是process
        request -> process();//任务中要有process处理函数
    }    
}

#endif