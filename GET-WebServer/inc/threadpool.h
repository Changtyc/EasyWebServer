#ifndef THREADPOOL_H
#define THREADPOOL_H

// 线程同步的包装类
#include "locker.h"
#include <cstdio>
#include <exception>
#include <list>
#include <pthread.h>

// 线程池类，模板参数是任务类
template <typename T> class threadpool {
  public:
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request);

  private:
    // 必须是静态成员函数，因为普通成员函数隐含地包含了一个 this
    // 指针，而pthread_create 的执行函数签名要求是 void*
    // (fun)(void*)，它不包含类实例指针。
    static void *worker(void *arg);
    void run();

  private:
    int m_thread_number;        // 线程池中的线程数
    int m_max_requests;         // 请求队列中允许的最大请求数
    pthread_t *m_threads;       // 线程池数组
    std::list<T *> m_workqueue; // 请求队列
    locker m_queuelocker;       // 保护请求队列的互斥锁
    sem m_queuestat; // 包装的信号类，判断是否有任务需要处理
    bool m_stop;     // 是否结束线程
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests)
    : m_thread_number(thread_number), m_max_requests(max_requests),
      m_stop(false), m_threads(NULL) {
    if ((thread_number <= 0) || (max_requests <= 0)) {
        throw std::exception();
    }

    // 创建线程数组
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }

    // 创建子线程，并设置为脱离线程
    for (int i = 0; i < thread_number; ++i) {
        printf("create the %dth thread\n", i);
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T> threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

// 添加任务队列
template <typename T> bool threadpool<T>::append(T *request) {
    // 操作工作队列时一定要加锁，因为其被所有线程共享。
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    // 添加一个信号量
    m_queuestat.post();
    return true;
}

// 对应线程的处理函数，不断从工作队列取出任务并执行，一个死循环
template <typename T> void *threadpool<T>::worker(void *arg) {
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T> void threadpool<T>::run() {
    while (!m_stop) {
        // 等待需要处理的信号量
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request) {
            continue;
        }
        // 执行任务
        request->process();
    }
}

#endif
