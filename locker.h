#ifndef LOCKER_H
#define LOCKER_H

#include<exception>
#include<pthread.h>
#include<semaphore.h>

//信号量封装
class sem{
public:
    sem()
    {
        if(sem_init(&m_sem,0,0)!=0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem)==0;
    }
    bool post()
    {
        return sem_post(&m_sem);
    }
private:
    sem_t m_sem;
};

//互斥锁封装
class locker{
public:
    locker()
    {
        if(pthread_mutex_init(&m_mutex,NULL)!=0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex)==0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex)==0;
    }
private:
    pthread_mutex_t m_mutex;
};

//条件变量的封装
class cond{
public:
    cond()
    {
        //条件变量属于共享资源，要有互斥锁来保护之
        if(pthread_mutex_init(&m_mutex,NULL)!=0)
        {
            throw std::exception();
        }
        if(pthread_cond_init(&m_cond,NULL)!=0)
        {
            pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_mutex_destroy(&m_mutex);
        pthread_cond_destroy(&m_cond);
    }
    //在调用条件变量的wait时，需要先判断是否满足条件，如果不满足，则将该线程加到条件变量的阻塞队列中去，避免在这个过程中条件变量发生变化，所以先加锁
    //要使得每次条件变量的变化都能捕捉到
    bool wait()
    {
        int ret=0;
        pthread_mutex_lock(&m_mutex);
        ret=pthread_cond_wait(&m_cond,&m_mutex);
        pthread_mutex_unlock(&m_mutex);
        return ret==0;
    }
    //唤醒一个线程，具体唤醒哪个，要根据内核的调度策略和优先级来决定
    bool signal()
    {
        return pthread_cond_signal(&m_cond)==0;
    }
private:
    pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};

#endif