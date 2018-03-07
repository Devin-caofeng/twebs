#ifndef LOCKER_HPP_
#define LOCKER_HPP_

#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

class SemLocker {
public:
    SemLocker() {
        if (sem_init(&sem_, 0, 0) != 0) printf("sem init error");
    }

    ~SemLocker() { sem_destroy(&sem_); }

    bool Wait() { return sem_wait(&sem_) == 0; }

    bool Add() { return sem_post(&sem_) == 0; }

private:
    sem_t sem_;
};

class MutexLocker {
public:
    MutexLocker() {
        if (pthread_mutex_init(&mutex_, NULL) != 0) {
            printf("mutex init error");
        }
    }

    ~MutexLocker() { pthread_mutex_destroy(&mutex_); }

    bool MutexLock() { return pthread_mutex_lock(&mutex_) == 0; }

    bool MutexUnlock() { return pthread_mutex_unlock(&mutex_) == 0; }

private:
    pthread_mutex_t mutex_;
};

class CondLocker {
public:
    CondLocker() {
        if (pthread_mutex_init(&mutex_, NULL) != 0) {
            printf("mutex init error");
        }
        if (pthread_cond_init(&cond_, NULL) != 0) {
            pthread_mutex_destroy(&mutex_);
            printf("cond init error");
        }
    }

    ~CondLocker() {
        pthread_mutex_destroy(&mutex_);
        pthread_cond_destroy(&cond_);
    }

    bool Wait() {
        pthread_mutex_lock(&mutex_);
        int ret = pthread_cond_wait(&cond_, &mutex_);
        pthread_mutex_unlock(&mutex_);

        return ret == 0;
    }

    bool Signal() { return pthread_cond_signal(&cond_) == 0; }

    bool Broadcast() { return pthread_cond_broadcast(&cond_) == 0; }

private:
    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
};

#endif  // LOCKER_HPP_
