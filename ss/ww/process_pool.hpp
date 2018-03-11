#ifndef PROCESS_POOL_HPP_
#define PROCESS_POOL_HPP_

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define OUT stdout

using SA = struct sockaddr;

/*
 * 表示一个子进程的类，
 * pid_ 是目标子进程的 pid，
 * pipefd_ 是父进程和子进程通信用的管道
 */
class Process {
public:
    Process() : pid_(-1) { }

public:
    pid_t pid_;
    int   pipefd_[2];
};

/*
 * 进程池类，定义为模板是为了代码复用，其模板参数是处理逻辑任务的类
 */
template <typename T>
class ProcessPool {
private:
    ProcessPool(int listenfd, int process_number = 8);
public:
    // 单例模式，保证程序最多创建一个 ProcessPool 对象，这是程序正确处理信号的必要条件
    static ProcessPool<T> *Create(int listenfd, int process_number = 8) {
        if (!instance_) {
            instance_ = new ProcessPool<T>(listenfd, process_number);
        }
        return instance_;
    }

    ~ProcessPool() { delete[] sub_process_; }

    void Run();

private:
    void SetupSigPipe();

    void ProcessNewConnInParent(int &sub_process_counter);
    void ProcessSigInParent();
    void RunParent();

    int  ProcessNewConnInChild(int sockfd, T *const users);
    int  ProcessSigInChild();
    void CoreRunChild(const struct epoll_event *events,
                      int i, int pipefd, T *const users);
    void RunChild();

private:
    // 进程池允许的最大子进程数
    static const int MAX_PROCESS_NUMBER = 16;
    // 每个子进程最多可以处理的客户数量
    static const int USER_PER_PROCESS = 65536;
    // epoll 最多可以处理的事件数
    static const int MAX_EVENT_NUMBER = 10000;
    // 进程池中的进程总数
    int process_number_;
    // 子进程在进程池中的序号，编号从0开始
    int idx_;
    // 每个进程都有一个 epoll 内核事件表，用 epollfd 标识
    int epollfd_;
    // 监听 socket
    int listenfd_;
    // 子进程通过 stop 来决定是否停止运行
    int stop_;
    // 保存所有子进程的描述信息
    Process *sub_process_;
    // 进程池静态对象
    static ProcessPool<T> *instance_;
};

template <typename T>
ProcessPool<T> *ProcessPool<T>::instance_ = nullptr;

// 用于处理信号的管道，以实现统一事件源，简称为信号管道
static int sig_pipefd[2];

static int SetNonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

static void AddFD(int epollfd, int fd) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    SetNonblocking(fd);
}

static void RemoveFD(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

static void SigHandler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(sig_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

static void AddSig(int sig, void(*Handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = Handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }

    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

/*
 * 进程池构造函数
 *   参数 listenfd 是监听 socket，它必须在创建进程池之前被创建，
 *   否则子进程无法直接引用它。
 *   参数 process_number 指定进程池中子进程的数量
 */
template <typename T>
ProcessPool<T>::ProcessPool(int listenfd, int process_number)
    : listenfd_(listenfd),
      process_number_(process_number),
      idx_(-1),
      stop_(false) {
    assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));

    sub_process_ = new Process[process_number];
    assert(sub_process_);

    // 创建 process_number 个子进程，并建立它们和父进程之间的管道
    for (int i = 0; i < process_number; ++i) {
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sub_process_[i].pipefd_);
        assert(ret == 0);

        sub_process_[i].pid_ = fork();
        assert(sub_process_[i].pid_ >= 0);
        if (sub_process_[i].pid_ > 0) {
            close(sub_process_[i].pipefd_[1]);
            continue;
        }
        else {
            close(sub_process_[i].pipefd_[0]);
            idx_ = i;
            break;
        }
    }
}

/*
 * 统一事件源
 */
template <typename T>
void ProcessPool<T>::SetupSigPipe() {
    // 创建 epoll 事件监听表和信号管道
    epollfd_ = epoll_create(5);
    assert(epollfd_ != -1);

    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert(ret != -1);

    SetNonblocking(sig_pipefd[1]);
    AddFD(epollfd_, sig_pipefd[0]);

    // 设置信号处理函数
    AddSig(SIGCHLD, SigHandler);
    AddSig(SIGTERM, SigHandler);
    AddSig(SIGINT,  SigHandler);
    AddSig(SIGPIPE, SIG_IGN);
}

/*
 * 父进程中 idx 值为-1，子进程中 idx 值大于等于0，
 * 根据此判断要运行的是父进程还是子进程
 */
template <typename T>
void ProcessPool<T>::Run() {
    if (idx_ != -1) {
        RunChild();
    }
    else {
        RunParent();
    }
}

template <typename T>
int ProcessPool<T>::ProcessNewConnInChild(int sockfd, T *const users) {
    int client = 0;
    // 从父，子进程之间的管道读取数据，并将结果保存在 client 中
    // 如果读取成功，则表示有新客户连接到来
    int ret = recv(sockfd, (char *)&client, sizeof(client), 0);
    if (((ret < 0) && (errno != EAGAIN)) || ret == 0) {
        return -1;
    }
    else {
        struct sockaddr_in cli_addr;
        socklen_t cli_addr_len = sizeof(cli_addr);
        int connfd = accept(listenfd_, (SA *)&cli_addr,
                            &cli_addr_len);
        if (connfd < 0) {
            perror("accept error");
            return -1;
        }
        AddFD(epollfd_, connfd);
        // 模板类 T 必须实现 init 方法，以初始化一个客户连接。
        // 我们直接使用 connfd 来索引逻辑处理对象（T 类型的对象），以提高程序效率
        users[connfd].Init(epollfd_, connfd, cli_addr);
    }

    return 0;
}

template <typename T>
int ProcessPool<T>::ProcessSigInChild() {
    char signals[1024];
    int ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
    if (ret <= 0) {
        return -1;
    }
    else {
        for (int i = 0; i < ret; ++i) {
            switch (signals[i]) {
            case SIGCHLD:
                pid_t pid;
                int stat;
                while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
                    continue;
                }
                break;
            case SIGTERM:
            case SIGINT:
                stop_ = true;
                break;
            default:
                break;
            }
        }
    }

    return 0;
}

template <typename T>
void ProcessPool<T>::CoreRunChild(const struct epoll_event *events,
                                  int i, int pipefd, T *const users) {
    int sockfd = events[i].data.fd;
    if ((sockfd == pipefd) && (events[i].events & EPOLLIN)) {
        int ret = ProcessNewConnInChild(sockfd, users);
        if (ret == -1) {
            fprintf(OUT, "ProcessNewConnInChild error\n");
        }
    }
    else if ((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN)) {
        int ret = ProcessSigInChild();
        if (ret == -1) {
            fprintf(OUT, "ProcessSigInChild error\n");
        }
    }
    // 如果是其他可读数据，那么一定是客户端请求，调用逻辑处理对象的 Process 处理
    else if (events[i].events & EPOLLIN) {
        users[sockfd].Process();
    }
    else {
        ;
    }
}

template <typename T>
void ProcessPool<T>::RunChild() {
    SetupSigPipe();

    // 每个子进程都通过其在进程池中的序号值 idx 找到与父进程通信的管道
    int pipefd = sub_process_[idx_].pipefd_[1];
    // 子进程需要监听管道文件描述符 pipefd，因为父进程将通过它来通知子进程 accept 新连接
    AddFD(epollfd_, pipefd);

    struct epoll_event events[MAX_EVENT_NUMBER];
    T *users = new T[USER_PER_PROCESS];
    assert(users);

    while (stop_) {
        int number = epoll_wait(epollfd_, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {
            perror("epoll failure");
            break;
        }
        for (int i = 0; i < number; ++i) {
            CoreRunChild(events, i, pipefd, users);
        }
    }

    delete[] users;
    users = nullptr;
    close(pipefd);
    close(epollfd_);
}

template <typename T>
void ProcessPool<T>::ProcessNewConnInParent(int &sub_process_counter) {
    int i = sub_process_counter;
    do {
        if (sub_process_[i].pid_ != -1) break;
        i = (i + 1) % process_number_;
    } while (i != sub_process_counter);

    if (sub_process_[i].pid_ == -1) {
        stop_ = true;
        return;
    }

    sub_process_counter = (i + 1) % process_number_;

    int new_conn = 1;
    send(sub_process_[i].pipefd_[0], (char *)&new_conn, sizeof(new_conn), 0);
    printf("send request to child %d\n", i);
}

template <typename T>
void ProcessPool<T>::ProcessSigInParent() {
    char signals[1024];
    int ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
    if (ret <= 0) return;

    for (int i = 0; i < ret; ++i) {
        switch (signals[i]) {
        case SIGCHLD:
            pid_t pid;
            int stat;
            while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
                for (int j = 0; j < process_number_; ++j) {
                    // 如果进程池中第 i 个子进程退出了，则主进程关闭相应的通信管道，
                    // 并设置相应的 pid 为-1，以标记该子进程已经退出
                    if (sub_process_[j].pid_ == pid) {
                        printf("child %d join\n", j);
                        close(sub_process_[j].pipefd_[0]);
                        sub_process_[j].pid_ = -1;
                    }
                }
            }
            // 如果所有的子进程都退出了，则父进程也退出
            stop_ = true;
            for (int j = 0; j < process_number_; ++j) {
                if (sub_process_[j].pid_ != -1) {
                    stop_ = false;
                }
            }
            break;
        case SIGTERM:
        case SIGINT:
            // 如果是父进程收到终止信号，则杀死所有子进程，并等待它们全部结束。
            printf("kill all the child now\n");
            for (int j = 0; j < process_number_; ++j) {
                int pid = sub_process_[j].pid_;
                if (pid != -1) kill(pid, SIGTERM);
            }
            break;
        default:
            break;
        }
    }
}


template <typename T>
void ProcessPool<T>::RunParent() {
    SetupSigPipe();

    AddFD(epollfd_, listenfd_);

    epoll_event events[MAX_EVENT_NUMBER];
    int sub_process_counter = 0;

    while (!stop_) {
        int number = epoll_wait(epollfd_, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < number; ++i) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd_) {
                // 如果有新连接到来，采用 round robin 方式将其分配给一个子进程来处理
                ProcessNewConnInParent(sub_process_counter);
            }
            else if ((sockfd = sig_pipefd[0]) && (events[i].events & EPOLLIN)) {
                ProcessSigInParent();
            }
            else {
                ;
            }
        }
    }

    close(epollfd_);
}

#endif  // PROCESS_POOL_HPP_
