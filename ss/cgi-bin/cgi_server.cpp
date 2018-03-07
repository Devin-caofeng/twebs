#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "process_pool.hpp"

using SA = struct sockaddr;

/*
 * 处理客户 CGI 请求，可作为 ProcessPool 的模板参数
 */
class CgiConn {
public:
    CgiConn() = default;
    ~CgiConn() = default;

    void Init(int epollfd, int sockfd, const struct sockaddr_in &cli_addr);
    void Process();

private:
    int CoreProcess(int ret, int idx);

private:
    static const int   BUF_SIZE = 1024;
    static int         epollfd_;
    int                sockfd_;
    struct sockaddr_in addr_;
    char               buf_[BUF_SIZE];
    // 标记读缓冲中已经读入的客户数据的最后一个字节的下一个位置
    int                read_idx_;
};

int CgiConn::epollfd_ = -1;

void CgiConn::Init(int epollfd, int sockfd, const sockaddr_in &cli_addr) {
    epollfd_  = epollfd;
    sockfd_   = sockfd;
    addr_     = cli_addr;
    read_idx_ = 0;
    memset(buf_, '\0', BUF_SIZE);
}

int CgiConn::CoreProcess(int ret, int idx) {
    read_idx_ += ret;
    printf("user content is :%s\n", buf_);
    // 如果遇到字符"\r\n"，则开始处理客户请求
    for ( ; idx < read_idx_; ++idx) {
        if ((idx >= 1) && (buf_[idx - 1] == '\r') && (buf_[idx] == '\n')) {
            break;
        }
    }
    // 如果没有遇到字符"\r\n"，则需要读取更多的用户数据
    if (idx == read_idx_) {
        return 0;
    }
    buf_[idx - 1] = '\0';

    char *file_name = buf_;
    if (access(file_name, F_OK) == -1) {
        RemoveFD(epollfd_, sockfd_);
        return -1;
    }
    // 创建子进程来执行 CGI 程序
    int pid = fork();
    if (pid == -1) {
        RemoveFD(epollfd_, sockfd_);
        return -1;
    }
    else if (pid > 0) {
        // 父进程只需关闭连接
        RemoveFD(epollfd_, sockfd_);
        return -1;
    }
    else {
        close(STDOUT_FILENO);
        dup(sockfd_);
        execl(buf_, buf_, NULL);
        exit(0);
    }
}

void CgiConn::Process() {
    while (true) {
        int idx = read_idx_;
        int ret = recv(sockfd_, buf_ + idx, BUF_SIZE - 1 - idx, 0);
        // 如果读操作发生错误，则关闭客户连接，如果是暂时无数据可读，则退出循环
        if (ret < 0) {
            if (errno != EAGAIN) {
                RemoveFD(epollfd_, sockfd_);
            }
            break;
        }
        // 如果对方关闭连接，则服务器也关闭连接
        else if (ret == 0) {
            RemoveFD(epollfd_, sockfd_);
            break;
        }
        else {
            if (CoreProcess(ret, idx) != 0) {
                break;
            }
        }
    }
}

int main(int argc, char *argv[]) {

    if (argc <= 2) {
        printf("usage: %s ip address port number\n", basename(argv[0]));
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &addr.sin_addr);
    addr.sin_port = htons(port);

    int ret = bind(listenfd, (SA *)&addr, sizeof(addr));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    ProcessPool<CgiConn> *pool = ProcessPool<CgiConn>::Create(listenfd);
    if (pool) {
        pool->Run();
        delete pool;
    }

    close(listenfd);

    return 0;
}
