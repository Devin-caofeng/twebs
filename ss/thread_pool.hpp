#ifndef THREAD_POOL_HPP_
#define THREAD_POOL_HPP_

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>

#include "locker.hpp"


/*
 * 线程池类
 *   定义为模板类可以使得代码复用，模板参数 T 是任务类
 */
template <typename T>
class ThreadPool {
public:
    // 参数 thread_number 是线程池中线程的数量，
    // max_requests 是请求队列中最多允许的，等待处理的请求的数量
    ThreadPool(int thread_number = 0, int max_requests = 10000);
    ~ThreadPool();

    bool Append(T *request);

private:
    // 工作线程运行的函数，不断从工作队列中取出任务并执行
    static void *Worker(void *arg);
    void Run();

private:
    int             thread_number_;  // 线程中的线程数
    int             max_requests_;   // 请求队列中允许的最大请求数
    pthread_t      *threads_;        // 描述线程池的数组，其大小为 thread_number
    std::list<T *>  work_queue_;     // 请求队列
    MutexLocker     queue_locker_;   // 保护请求队列的互斥锁
    SemLocker       queue_stat_;     // 是否有任务需要处理
    bool            stop_;           // 是否结束线程
};

template <typename T>
ThreadPool<T>::ThreadPool(int thread_number, int max_requests)
    : thread_number_(thread_number),
      max_requests_(max_requests),
      stop_(false),
      threads_(nullptr) {
    if ((thread_number <= 0) || (max_requests <= 0)) {
        throw std::exception();
    }

    threads_ = new pthread_t[thread_number];
    if (!threads_) {
        throw std::exception();
    }

    for (int i = 0; i < thread_number; ++i) {
        printf("create the %dth thread\n", i);
        if (pthread_create(threads_ + i, NULL, Worker, this) != 0) {
            delete[] threads_;
            throw std::exception();
        }
        if (pthread_detach(threads_[i])) {
            delete[] threads_;
            throw std::exception();
        }
    }
}

template <typename T>
ThreadPool<T>::~ThreadPool() {
    delete[] threads_;
    stop_ = true;
}

template <typename T>
bool ThreadPool<T>::Append(T *request) {
    // 操作工作队列是一定要加锁
    queue_locker_.MutexLock();
    if (work_queue_.size() > max_requests_) {
        queue_locker_.MutexUnlock();
        return false;
    }
    work_queue_.push_back(request);
    queue_locker_.MutexUnlock();
    queue_stat_.Add();

    return true;
}

template <typename T>
void *ThreadPool<T>::Worker(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;
    pool->Run();
    return pool;
}

template <typename T>
void ThreadPool<T>::Run() {
    while (!stop_) {
        queue_stat_.Wait();
        queue_locker_.MutexLock();
        if (work_queue_.empty()) {
            queue_locker_.MutexUnlock();
            continue;
        }

        T *request = work_queue_.front();
        work_queue_.pop_front();
        queue_locker_.MutexUnlock();
        if (!request) {
            continue;
        }
        request->Process();
    }
}


#endif  // THREAD_POOL_HPP_
