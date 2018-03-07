#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <errno.h>
#include <sys/epoll.h>

#include "locker.hpp"
#include "thread_pool.hpp"
#include "http_conn.hpp"

using SA = struct sockaddr;

constexpr int max_fd = 65536;
constexpr int max_event_num = 10000;

extern int AddFD(int epollfd, int fd, bool one_shot);
extern int RemoveFD(int epollfd, int fd);

void AddSig(int sig, void(* Handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = Handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void ShowError(int connfd, const char *info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}


int main(int argc, char *argv[]) {
    if (argc <= 2) {
        printf("usage: %s ip address port number\n", basename(argv[0]));
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    AddSig(SIGPIPE, SIG_IGN);

    ThreadPool<HttpConn> *pool = nullptr;
    try {
        pool = new ThreadPool<HttpConn>;
    }
    catch(...) {
        return 1;
    }

    // 预先为每个可能的客户端连接分配一个 HttpConn 对象
    HttpConn *users = new HttpConn[max_fd];
    assert(users);
    int user_count = 0;

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    struct linger temp = { 1, 0 };
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &temp, sizeof(temp));

    int ret = 0;
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &addr.sin_addr);
    addr.sin_port = htons(port);

    ret = bind(listenfd, (SA *)&addr, sizeof(addr));
    assert(ret >= 0);

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    struct epoll_event events[max_event_num];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    AddFD(epollfd, listenfd, false);
    HttpConn::epollfd_ = epollfd;

    while (true) {
        int num = epoll_wait(epollfd, events, max_event_num, -1);
        if ((num < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < num; ++i) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {
                struct sockaddr_in cli_addr;
                socklen_t cli_addr_len = sizeof(cli_addr_len);
                int connfd = accept(listenfd, (SA *)&cli_addr, &cli_addr_len);

                if (connfd < 0) {
                    perror("accpet error");
                    continue;
                }
                if (HttpConn::user_count_ >= max_fd) {
                    ShowError(connfd, "Internal server busy");
                    continue;
                }
                users[connfd].Init(connfd, cli_addr);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                users[sockfd].CloseConn();
            }
            else if (events[i].events & EPOLLIN) {
                if (users[sockfd].Read()) {
                    pool->Append(users + sockfd);
                }
                else {
                    users[sockfd].CloseConn();
                }
            }
            else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].Write()) {
                    users[sockfd].CloseConn();
                }
            }
            else {
                ;
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;

    return 0;
}
