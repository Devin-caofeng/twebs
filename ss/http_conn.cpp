#include "http_conn.hpp"

// HTTP 响应状态信息
const char *ok_200_title  = "OK";

const char *err_400_title = "Bad Request";
const char *err_400_form  =
    "Your request has bad syntax or is inherently impossible to satisfy.\n";

const char *err_403_title = "Forbidden";
const char *err_403_form  =
    "You do not have permission to get file from this server.\n";

const char *err_404_title = "Not Found";
const char *err_404_form  =
    "The requested file was not found on this server.\n";

const char *err_500_title = "Internal Error";
const char *err_500_form  =
    "There was an unusual problem serving the requested file.\n";

const char *doc_root = "/www/html";

int SetNonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);

    return old_option;
}

void AddFD(int epollfd, int fd, bool one_shot) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    SetNonblocking(fd);
}

void RemoveFD(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

void ModFD(int epollfd, int fd, int ev) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int HttpConn::epollfd_ = -1;
int HttpConn::user_count_ = 0;

void HttpConn::CloseConn(bool read_close) {
    if (read_close && (sockfd_ != -1)) {
        RemoveFD(epollfd_, sockfd_);
        sockfd_ = -1;
        --user_count_;
    }
}

void HttpConn::Init(int sockfd, const struct sockaddr_in &addr) {
    sockfd_ = sockfd;
    addr_   = addr;
    // 避免 TIME_WAIT 状态，仅用于调试，实际使用时应去掉
    int reuse = 1;
    setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    AddFD(epollfd_, sockfd_, true);
    ++user_count_;

    Init();
}

void HttpConn::Init() {
    check_state_    = CHECK_STATE_REQUESTLINE;
    linger_         = false;
    method_         = GET;
    url_            = nullptr;
    version_        = nullptr;
    content_length_ = 0;
    host_           = nullptr;
    start_line_     = 0;
    checked_idx_    = 0;
    read_idx_       = 0;
    write_idx_      = 0;
    memset(read_buf_, '\0', READ_BUF_SIZE);
    memset(write_buf_, '\0', WRITE_BUF_SIZE);
    memset(real_file_, '\0', FILENAME_LEN);
}

/*
 * 从状态机
 */
HttpConn::LineStatus HttpConn::ParseLine() {
    for ( ; checked_idx_ < read_idx_; ++checked_idx_) {
        char temp = read_buf_[checked_idx_];
        if (temp == '\r') {
            if ((checked_idx_ + 1) == read_idx_) {
                return LINE_OPEN;
            }
            else if (read_buf_[checked_idx_ + 1]  == '\n') {
                read_buf_[checked_idx_++] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LINE_OK;
            }

            return LINE_BAD;
        }
        else if (temp == '\n') {
            if ((checked_idx_ > 1) && (read_buf_[checked_idx_ - 1] == '\r')) {
                read_buf_[checked_idx_++] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LINE_OK;
            }

            return LINE_BAD;
        }
    }

    return LINE_OPEN;
}

/*
 * 读取客户端数据，直到无数据刻度或客户端关闭连接
 */
bool HttpConn::Read() {
    if (read_idx_ >= READ_BUF_SIZE) {
        return false;
    }

    while (true) {
        int bytes_read = recv(sockfd_, read_buf_ + read_idx_,
                              READ_BUF_SIZE - read_idx_, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        }
        else if (bytes_read == 0) {
            return false;
        }

        read_idx_ += bytes_read;
    }

    return true;
}

/*
 * 解析 HTTP 请求行，获得请求方法，目标 URL，以及 HTTP 版本号
 */
HttpConn::HttpCode HttpConn::ParseRequestLine(char *text) {
    url_ = strpbrk(text, " \t");
    if (!url_) {
        return BAD_REQEUST;
    }
    *url_++ = '\0';

    char *method = text;
    if (strcasecmp(method, "GET") == 0) {
        method_ = GET;
    }
    else {
        return BAD_REQEUST;
    }

    url_ += strspn(url_, " \t");
    version_ = strpbrk(url_, " \t");
    if (strcasecmp(version_, "HTTP/1.1") != 0) {
        return BAD_REQEUST;
    }
    if (strncasecmp(url_, "http://", 7) == 0) {
        url_ += 7;
        url_ = strchr(url_, '/');
    }

    if (!url_ || url_[0] != '/') {
        return BAD_REQEUST;
    }

    check_state_ = CHECK_STATE_REQUESTLINE;
    return NO_REQUEST;
}

/*
 * 解析 HTTP 请求的一个头部信息
 */
HttpConn::HttpCode HttpConn::ParseHeaders(char *text) {
    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0') {
        // 如果 HTTP 请求有消息体，则还需要读取 content_length_ 字节的消息体，
        // 状态转移到 CHECK_STATE_CONTENT 状态
        if (content_length_ != 0) {
            check_state_ = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明已经得到一个完整的 HTTP 请求
        return GET_REQUEST;
    }
    // 处理 Connetion 头部字段
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            linger_ = true;
        }
    }
    // 处理 Content-Length 头部字段
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        content_length_ = atol(text);
    }
    // 处理 Host 头部字段
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        host_ = text;
    }
    else {
        printf("oop ! unknow header %s\n", text);
    }

    return NO_REQUEST;
}

/*
 * 在此没有真正解析 HTTP 请求的消息体，只是判断它是否被完成的读入了
 */
HttpConn::HttpCode HttpConn::ParseContent(char *text) {
    if (read_idx_ >= (content_length_ + checked_idx_)) {
        text[content_length_] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/*
 * 主状态机
 */
HttpConn::HttpCode HttpConn::ProcessRead() {
    LineStatus line_status = LINE_OK;
    HttpCode ret = NO_REQUEST;

    while (((check_state_ == CHECK_STATE_CONTENT) &&
            (line_status == LINE_OK)) ||
           ((line_status = ParseLine()) == LINE_OK)) {
        char *text = GetLine();
        start_line_ = checked_idx_;
        printf("got 1 http line: %s\n", text);

        switch (check_state_) {
        case CHECK_STATE_REQUESTLINE:
            ret = ParseRequestLine(text);
            if (ret == BAD_REQEUST) {
                return BAD_REQEUST;
            }
            break;
        case CHECK_STATE_HEADER:
            ret = ParseHeaders(text);
            if (ret == BAD_REQEUST) {
                return BAD_REQEUST;
            }
            else if (ret == GET_REQUEST) {
                return DoRequest();
            }
            break;
        case CHECK_STATE_CONTENT:
            ret = ParseContent(text);
            if (ret == GET_REQUEST) {
                return DoRequest();
            }
            line_status = LINE_OPEN;
            break;
        default:
            return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;
}

/*
 * 得到一个完整，正确的 HTTP 请求时，分析目标文件的属性，如果目标文件存在，
 * 对所有用户可读，且不是目录，则使用 mmap 将其映射到内存地址 file_address_ 处，
 * 并通知调用者获取文件成功
 */
HttpConn::HttpCode HttpConn::DoRequest() {
    strcpy(real_file_, doc_root);
    int len = strlen(doc_root);
    strncpy(real_file_ + len, url_, FILENAME_LEN - len - 1);

    if (stat(real_file_, &file_stat_) < 0) {
        return NO_RESOURCE;
    }

    if (!(file_stat_.st_mode & S_IROTH)) {
        return BAD_REQEUST;
    }

    if (S_ISDIR(file_stat_.st_mode)) {
        return BAD_REQEUST;
    }

    int fd = open(real_file_, O_RDONLY);
    file_addr_ = (char *)mmap(0, file_stat_.st_size, PROT_READ,
                              MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}

/*
 * 对内存映射区执行 munmap 操作
 */
void HttpConn::Unmap() {
    if (file_addr_) {
        munmap(file_addr_, file_stat_.st_size);
        file_addr_ = nullptr;
    }
}

/*
 * 写 HTTP 响应
 */
bool HttpConn::Write() {
    int bytes_have_send = 0;
    int bytes_to_send = write_idx_;
    if (bytes_to_send == 0) {
        ModFD(epollfd_, sockfd_, EPOLLIN);
        Init();
        return true;
    }

    while (1) {
        int temp = writev(sockfd_, iv_, iv_count_);
        if (temp <= -1) {
            // 如果 tcp 写缓冲没有空间，则等待下次的 EPOLLOUT 事件，虽然在此期间
            // 服务器无法立即接收到同一客户的下一请求，但可以保证连接的完整性
            if (errno == EAGAIN) {
                ModFD(epollfd_, sockfd_, EPOLLOUT);
                return true;
            }
            Unmap();

            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_to_send <= bytes_have_send) {
            // 发送 HTTP 响应成功，
            // 根据 HTTP 请求中的 Connection 字段决定是否立即关闭连接
            Unmap();
            if (linger_) {
                Init();
                ModFD(epollfd_, sockfd_, EPOLLIN);
                return true;
            }
            else {
                ModFD(epollfd_, sockfd_, EPOLLIN);
                return false;
            }
        }
    }
}

/*
 * 向写缓冲中写入待发送的数据
 */
bool HttpConn::AddResponse(const char *format, ...) {
    if (write_idx_ >= WRITE_BUF_SIZE) {
        return false;
    }

    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(write_buf_ + write_idx_,
                        WRITE_BUF_SIZE - 1 - write_idx_, format, arg_list);
    if (len >= (WRITE_BUF_SIZE - 1 - write_idx_)) {
        return false;
    }
    write_idx_ += len;
    va_end(arg_list);

    return true;
}

bool HttpConn::AddStatusLine(int status, const char *title) {
    return AddResponse("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool HttpConn::AddHeaders(int content_len) {
    return AddContentLength(content_len) && AddLinger() && AddBlankLine();
}

bool HttpConn::AddContentLength(int content_len) {
    return AddResponse("Content-Length: %d\r\n", content_len);
}

bool HttpConn::AddLinger() {
    return AddResponse("Connection: %s\r\n",
                        (linger_ == true) ? "keep-alive" : "close");
}

bool HttpConn::AddBlankLine() {
    return AddResponse("%s", "\r\n");
}

bool HttpConn::AddContent(const char *content) {
    return AddResponse("%s", content);
}

bool HttpConn::ProcessWriteCommon(int num, const char *title,
                                  const char *form) {
    AddStatusLine(num, title);
    AddHeaders(strlen(form));
    return AddContent(form);
}

/*
 * 根据服务器处理 HTTP 请求的结果，决定返回给客户端的内容
 */
bool HttpConn::ProcessWrite(HttpCode ret) {
    switch (static_cast<int>(ret)) {
    case INTERNAL_ERROR:
        if (!ProcessWriteCommon(500, err_500_title, err_500_form)) return false;
        break;
    case BAD_REQEUST:
        if (!ProcessWriteCommon(400, err_400_title, err_400_form)) return false;
        break;
    case NO_RESOURCE:
        if (!ProcessWriteCommon(404, err_404_title, err_404_form)) return false;
        break;
    case FORBIDDEN_REQUEST:
        if (!ProcessWriteCommon(403, err_403_title, err_403_form)) return false;
        break;
    case FILE_REQUEST:
        AddStatusLine(200, ok_200_title);
        if (file_stat_.st_size != 0) {
            AddHeaders(file_stat_.st_size);
            iv_[0].iov_base = write_buf_;
            iv_[0].iov_len  = write_idx_;
            iv_[1].iov_base = file_addr_;
            iv_[1].iov_len  = file_stat_.st_size;
            iv_count_ = 2;
            return true;
        }
        else {
            const char *ok_string = "<html><body></body></html>";
            AddHeaders(strlen(ok_string));
            if (!AddContent(ok_string)) return false;
        }
        break;
    default:
        return false;
        break;
    }

    iv_[0].iov_base = write_buf_;
    iv_[0].iov_len  = write_idx_;
    iv_count_ = 1;

    return true;
}

/*
 * 处理 HTTP 请求的入口函数，由线程池中的工作线程调用，
 */
void HttpConn::Process() {
    HttpCode read_ret = ProcessRead();
    if (read_ret == NO_REQUEST) {
        ModFD(epollfd_, sockfd_, EPOLLIN);
        return;
    }

    bool write_ret = ProcessWrite(read_ret);
    if (!write_ret) {
        CloseConn();
    }
    ModFD(epollfd_, sockfd_, EPOLLOUT);
}
