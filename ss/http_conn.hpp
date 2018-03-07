#ifndef HTTP_CONNECTION_HPP_
#define HTTP_CONNECTION_HPP_

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>

#include "locker.hpp"

class HttpConn {
public:
    static const int FILENAME_LEN = 256;
    static const int READ_BUF_SIZE = 4096;
    static const int WRITE_BUF_SIZE = 2048;

    enum Method {
        GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCK
    };
    // 解析客户请求时，朱状态机所处的状态
    enum CheckState {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    // 服务器处理 HTTP 请求可能的结果
    enum HttpCode {
        NO_REQUEST = 0,
        GET_REQUEST,
        BAD_REQEUST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    // 行读取状态
    enum LineStatus { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    HttpConn() = default;
    ~HttpConn() = default;

    void Init(int sockfd, const struct sockaddr_in &addr);
    void CloseConn(bool real_close = true);
    void Process();
    bool Read();
    bool Write();

private:
    void     Init();
    bool     ProcessWrite(HttpCode ret);
    HttpCode ProcessRead();

    // 供 ProcessWrite 调用，以解析 HTTP 请求
    HttpCode    ParseRequestLine(char *text);
    HttpCode    ParseHeaders(char *text);
    HttpCode    ParseContent(char *text);
    HttpCode    DoRequest();
    char       *GetLine() { return read_buf_ + start_line_; }
    LineStatus  ParseLine();

    // 供 ProcessWrite 调用，以完成 HTTP 应答
    bool ProcessWriteCommon(int num, const char *title, const char *form);
    void Unmap();
    bool AddResponse(const char *format, ...);
    bool AddContent(const char *content);
    bool AddStatusLine(int status, const char *title);
    bool AddHeaders(int content_length);
    bool AddContentLength(int content_len);
    bool AddLinger();
    bool AddBlankLine();

public:
    // 所有 socket 上的事件都被注册到同一个 epoll 内核事件表中
    static int epollfd_;
    static int user_count_;

private:
    // 该 HTTP 连接的 socket 和对方的 socket 地址
    int                sockfd_;
    struct sockaddr_in addr_;

    char               read_buf_[READ_BUF_SIZE];
    int                read_idx_;
    int                checked_idx_;
    int                start_line_;

    char               write_buf_[WRITE_BUF_SIZE];
    int                write_idx_;

    CheckState         check_state_;
    Method             method_;

    char               real_file_[FILENAME_LEN];
    char              *url_;
    char              *version_;
    char              *host_;
    int                content_length_;
    bool               linger_;

    // 客户请求的目标文件被 mmap 到内存中的其实位置
    char              *file_addr_;
    struct stat        file_stat_;
    // 用 writev 来执行写操作，iv_count_ 表示被写内存块的数量
    struct             iovec iv_[2];
    int                iv_count_;
};


#endif  // HTTP_CONNECTION_HPP_
