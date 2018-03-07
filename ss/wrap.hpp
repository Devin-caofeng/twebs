#ifndef WRAP_HPP_
#define WRAP_HPP_

#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

void PerrAndExit(const char *remind);

int Socket(int family, int type, int protocol);

void Bind(int fd, const struct sockaddr *sa, socklen_t sa_len);

void Listen(int fd, int backlog);

int Accept(int fd, struct sockaddr *sa, socklen_t *sa_len);

void Connect(int fd, const struct sockaddr *sa, socklen_t sa_len);

ssize_t Recv(int fd, void *buf, ssize_t len, int flags);

ssize_t Send(int fd, const void *buf, ssize_t len, int flags);

ssize_t Read(int fd, void *ptr, size_t nbytes);

ssize_t Write(int fd, const void *vptr, size_t nbytes);

void Close(int fd);

ssize_t ReadN(int fd, const void *vptr, size_t n);

ssize_t WriteN(int fd, const void *vptr, size_t n);

size_t ReadLine(int fd, void *vptr, ssize_t max_len);


#endif /* WRAP_HPP_ */
