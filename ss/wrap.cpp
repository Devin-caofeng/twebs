#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#include "wrap.hpp"


void PerrAndExit(const char *remind) {
    perror(remind);
    exit(1);
}

int Socket(int family, int type, int protocol) {
    int sfd;

    if ((sfd = socket(family, type, protocol)) < 0) PerrAndExit("socket error");

    return sfd;
}

void Bind(int fd, const struct sockaddr *sa, socklen_t sa_len) {
    if (bind(fd, sa, sa_len) < 0) PerrAndExit("bind error");
}

void Listen(int fd, int backlog) {
    if (listen(fd, backlog) == -1) PerrAndExit("listen error");
}

int Accept(int fd, struct sockaddr *sa, socklen_t *sa_len) {
    int conn_fd;
again:
    if ((conn_fd = accept(fd, sa, sa_len)) < 0) {
        if ((errno == ECONNABORTED) || errno == EINTR) goto again;
        else PerrAndExit("accept error");
    }

    return conn_fd;
}

void Connect(int fd, const struct sockaddr *sa, socklen_t sa_len) {
    if (connect(fd, sa, sa_len) < 0) PerrAndExit("connect error");
}

ssize_t Recv(int fd, void *buf, ssize_t len, int flags) {
    ssize_t n;
again:
    if ((n = recv(fd, buf, len, flags)) == -1) {
        if (errno == EAGAIN || errno == EINTR) goto again;
        else return -1;
    }

    return n;
}

ssize_t Send(int fd, const void *buf, ssize_t len, int flags) {
    ssize_t n;
again:
    if ((n = send(fd, buf, len, flags)) == -1) {
        if (errno == EAGAIN || errno == EINTR) goto again;
        else return -1;
    }

    return n;
}

ssize_t Read(int fd, void *ptr, size_t nbytes) {
    ssize_t n;
again:
    if ((n = read(fd, ptr, nbytes)) == -1) {
        if (errno == EINTR) goto again;
        else return -1;
    }

    return n;
}

ssize_t Write(int fd, const void *ptr, size_t nbytes) {
    ssize_t n;
again:
    if ((n = write(fd, ptr, nbytes)) == -1) {
        if (errno == EINTR) goto again;
        else return -1;
    }

    return n;
}

void Close(int fd) {
    if (close(fd) == -1) PerrAndExit("close error");
}

ssize_t ReadN(int fd, const void *vptr, size_t n) {
    size_t  nleft = n;
    char   *ptr   = (char *)vptr;
    ssize_t nread;

    while (nleft > 0) {
        if ((nread = read(fd, ptr, nleft)) < 0) {
            if (errno == EINTR) nread = 0;
            else return -1;
        }
        else if (nread == 0) {
            break;
        }

        nleft -= nread;
        ptr += nread;
    }

    return n - nleft;
}

ssize_t WriteN(int fd, const void *vptr, size_t n) {
    size_t nleft = n;
    ssize_t nwritten;
    char *ptr  = (char *)vptr;

    while (nleft > 0) {
        if ((nwritten = write(fd, ptr, nleft)) <= 0) {
            if (nwritten < 0 && errno == EINTR) nwritten = 0;
            else return -1;
        }

        nleft -= nwritten;
        ptr += nwritten;
    }

    return n - nleft;
}

static ssize_t MyRead(int fd, char *ptr) {
    static int read_cnt;
    static char *read_ptr;
    static char read_buf[100];

    if (read_cnt <= 0) {
again:
        if ((read_cnt = read(fd, read_buf, sizeof(read_buf))) < 0) {
            if (errno == EINTR) goto again;
            else return -1;
        }
        else if (read_cnt == 0) {
            return 0;
        }

        read_ptr = read_buf;
    }
    --read_cnt;
    *ptr = *read_ptr++;

    return 1;
}

size_t ReadLine(int fd, void *vptr, ssize_t max_len) {
    ssize_t n, rc;
    char c, *ptr;

    ptr = (char *)vptr;
    for (n = 1; n < max_len; ++n) {
        if ((rc = MyRead(fd, &c)) == 1) {
            *ptr++ = c;
            if (c == '\n') break;
        }
        else if (rc == 0) {
            *ptr = 0;
            return n - 1;
        }
        else {
            return -1;
        }
    }

    *ptr = 0;
    return n;
}
