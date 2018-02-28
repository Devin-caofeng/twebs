#include "wrap.h"
#include "parse.h"

#define PID_FILE "./pid.file"

static void doit(int fd);
static void write_pid(int option);
static void get_requesthdrs(rio_t *rp);
static void post_requesthdrs(rio_t *rp,int *length);
static int  parse_uri(char *uri, char *filename, char *cgiargs);
static void serve_static(int fd, char *filename, int filesize);
static void serve_dir(int fd,char *filename);
static void get_filetype(const char *filename, char *filetype);
static void get_dynamic(int fd, char *filename, char *cgiargs);
static void post_dynamic(int fd, char *filename, int contentLength,rio_t *rp);
static void client_error(int fd, const char *cause, const char *errnum,
                         const char *shortmsg, const char *longmsg);
static void sig_chld_handler(int signo);


static int is_show_dir = 1;
char *cwd = NULL;

int main(int argc, char *argv[]) {

    openlog(argv[0], LOG_NDELAY | LOG_PID, LOG_DAEMON);
    cwd = get_current_dir_name();

    char temp_cwd[MAXLINE];
    strcpy(temp_cwd, cwd);
    strcat(temp_cwd, "/");

    char is_daemon;
    char *port_ptr = NULL;
    char *log_ptr = NULL;
    parse_option(argc, argv, &is_daemon, &port_ptr, &log_ptr);

    int port = (port_ptr == NULL) ? atoi(Getconfig("http")) : atoi(port_ptr);

    Signal(SIGCHLD, sig_chld_handler);

    if (log_ptr == NULL) {
        log_ptr = Getconfig("log");
    }
    initlog(strcat(temp_cwd, log_ptr));

    // whether show dir
    if (strcmp(Getconfig("dir"), "no") == 0) {
        is_show_dir = 0;
    }

    struct sockaddr_in cli_addr;
    socklen_t cli_addr_len = sizeof(cli_addr);

    if (is_daemon == 1 || strcmp(Getconfig("daemon"), "yes") == 0) {
        Daemon(1, 1);
    }
    write_pid(1);

    int listenfd = Open_listenfd(port);
    while (1) {
        int connfd = Accept(listenfd, (SA *)&cli_addr, &cli_addr_len);
        if (access_ornot(inet_ntoa(cli_addr.sin_addr)) == 0) {
            client_error(connfd, "maybe this web server not open to you !",
                         "403", "Forbidden", "Tiny couldn`t read the file`");
            continue;
        }

        int pid;
        if ((pid = Fork()) > 0) {
            Close(connfd);
            continue;
        }
        else if (pid == 0) {
            doit(connfd);
            exit(1);
        }
    }
}

static void sig_chld_handler(int signo) {
    Waitpid(-1, NULL, WNOHANG);
}

/*
 *  handle one HTTP request/response transaction
 */
static void doit(int fd) {
    char buf[MAXLINE];
    memset(buf, '\0', MAXLINE);

    // read request line and headers
    rio_t rio;
    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXBUF);

    char method[MAXLINE];
    char uri[MAXLINE];
    char version[MAXLINE];
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET") != 0 && strcasecmp(method, "POST") != 0) {
        client_error(fd, method, "501", "Not Inplemented",
                     "Tiny does not inplement this method");
        return;
    }

    // parse uri from GET request
    int is_static;
    char filename[MAXLINE];
    char cgi_args[MAXLINE];
    is_static = parse_uri(uri, filename, cgi_args);

    struct stat status;
    if (lstat(filename, &status) < 0) {
        client_error(fd, filename, "404", "Not found",
                     "Tiny couldn`t find this file");
        return;
    }
    if (S_ISDIR(status.st_mode) && is_show_dir) {
        serve_dir(fd, filename);
    }

    int is_get = 1;
    if (strcasecmp(method, "POST") == 0) {
        is_get = 0;
    }

    if (is_static) {
        get_requesthdrs(&rio);

        if (!(S_ISREG(status.st_mode)) || !(S_IRUSR & status.st_mode)) {
            client_error(fd, filename, "403", "Forbidden",
                         "Tiny couldn`t read the file");
            return;
        }
        serve_static(fd, filename, status.st_size);
    }
    else {  // serve dynamic content
        if (!(S_ISREG(status.st_mode)) || !(S_IXUSR & status.st_mode)) {
            client_error(fd, filename, "403", "Forbidden",
                         "Tiny couldn`t run the CGI program");
            return;
        }

        if (is_get) {
            get_requesthdrs(&rio);
            get_dynamic(fd, filename, cgi_args);
        }
        else {
            int content_length = 0;
            post_requesthdrs(&rio, &content_length);
            post_dynamic(fd, filename, content_length, &rio);
        }
    }
}

/*
 * read_requesthdrs - read and parse HTTP request headers
 */
static void get_requesthdrs(rio_t *rp) {
    char buf[MAXLINE];
    Rio_readlineb(rp, buf, MAXLINE);
    writetime();  // write access tiem in log file

    while (strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        writelog(buf);
    }
}

static void post_requesthdrs(rio_t *rp, int *length) {
    char buf[MAXLINE];
    Rio_readlineb(rp, buf, MAXLINE);
    writetime();  // write access time in log file

    while (strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        if (strncasecmp(buf, "Content-Length:", 15) == 0) {
            char *ptr = &buf[15];
            ptr += strspn(ptr, " \t");
            *length = atoi(ptr);
        }
        writelog(buf);
    }
}

static void serve_dir(int fd, char *filename) {
    char *ptr = strrchr(filename, '/');
    ++ptr;
    char dir_name[MAXLINE];
    strcpy(dir_name, ptr);
    strcat(dir_name, "/");

    DIR *dir = opendir(filename);
    if (dir == NULL) {
        syslog(LOG_ERR, "cannot open dir:%s", filename);
    }

    char files[MAXLINE];
    sprintf(files, "<html><title>Dir Browser</title>");
    sprintf(files, "%s<style type=""text/css""> a:link{text-decoration:none;} \
            </style>", files);
    sprintf(files, "%s<body bgcolor=""ffffff"" font-family=Arial color=#fff \
            font-size=14px>\r\n", files);

    struct dirent *dirent_ptr;
    while ((dirent_ptr = readdir(dir)) != NULL) {
        if (strcmp(dirent_ptr->d_name, ".") == 0 ||
            strcmp(dirent_ptr->d_name, "..") == 0) {
            continue;
        }

        char name[MAXLINE];
        sprintf(name, "%s/%s", filename, dirent_ptr->d_name);
        struct stat status;
        Stat(name, &status);
        struct passwd *file_passwd = getpwuid(status.st_uid);

        char img[MAXLINE];
        if (S_ISDIR(status.st_mode)) {
            sprintf(img,
                    "<img src=""dir.png"" width=""24px"" height=""24px"">");
        }
        else if (S_ISFIFO(status.st_mode)) {
            sprintf(img,
                    "<img src=""fifo.png"" width=""24px"" height=""24px"">");
        }
        else if (S_ISLNK(status.st_mode)) {
            sprintf(img,
                    "<img src=""link.png"" width=""24px"" height=""24px"">");
        }
        else if (S_ISSOCK(status.st_mode)) {
            sprintf(img,
                    "<img src=""sock.png"" width=""24px"" height=""24px"">");
        }
        else {
            sprintf(img,
                    "<img src=""file.png"" width=""24px"" height=""24px"">");
        }

        int num = 1;
        char modify_time[MAXLINE];
        sprintf(files, "%s<p><pre>%-2d %s ""<a href=%s%s"">%-15s</a>%-10s%10d %24s</pre></p>\r\n",
                files, ++num, img, dir_name, dirent_ptr->d_name,
                dirent_ptr->d_name, file_passwd->pw_name, (int)status.st_size,
                timeModify(status.st_mtime, modify_time));
    }
    closedir(dir);
    sprintf(files, "%s</body></html>", files);

    // send response headers to client
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sContent-Length: %lu\r\n", buf, strlen(files));
    sprintf(buf, "%sContent-Type: %s\r\n\r\n", buf, "text/html");

    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, files, strlen(files));

    exit(0);
}

static void post_dynamic(int fd, char *filename, int content_length, rio_t *rp) {
    char length[32];
    sprintf(length, "%d", content_length);

    char data[MAXLINE];
    memset(data, 0, MAXLINE);

    int pipefd[2];
    Pipe(pipefd);

    if (Fork() == 0) {
        Close(pipefd[0]);
        Rio_readnb(rp, data, content_length);
        Rio_writen(pipefd[1], data, content_length);
        exit(0);
    }

    // send response headers to client
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.9 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);

    Rio_writen(fd, buf, strlen(buf));

    Dup2(pipefd[0], STDIN_FILENO);
    Close(pipefd[0]);

    Close(pipefd[1]);
    setenv("CONTENT-LENGTH", length, 1);

    Dup2(fd, STDOUT_FILENO);
    char *empty_list[] = { NULL };
    Execve(filename, empty_list, environ);
}

/*
 * parse URI into filename and CGI args
 *     return 0 if dynamic content, 1 if static
 */
static int parse_uri(char *uri, char *filename, char *cgi_args) {
    if (!strstr(uri, "cig-bin")) {
        strcpy(cgi_args, "");
        char temp_cwd[MAXLINE];
        strcpy(filename, strcat(temp_cwd, Getconfig("root")));
        strcat(filename, uri);

        if (uri[strlen(uri) - 1] == '/') {
            strcat(filename, "home.html");
        }
        return 1;
    }
    else {
        char *ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgi_args, ptr + 1);
            *ptr = '\0';
        }
        else {
            strcpy(cgi_args, "");
        }

        strcpy(filename, cwd);
        strcat(filename, uri);

        return 0;
    }
}

/*
 * copy a file back to the cilent
 */
static void serve_static(int fd, char *filename, int filesize) {
    // sned response headers to client
    char filetype[MAXLINE];
    get_filetype(filename, filetype);
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sContent-Length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-Type: %s\r\n\r\n", buf, filetype);

    // send response body to client
    int src_fd = Open(filename, O_RDONLY, 0);
    char *src_ptr =
        (char *)Mmap(0, filesize, PROT_READ, MAP_PRIVATE, src_fd, 0);
    Close(src_fd);

    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, src_ptr, filesize);
    Munmap(src_ptr, filesize);
}

/*
 * dirive file type from file name
 */
static void get_filetype(const char *filename, char *filetype) {
    if (strstr(filename, ".html"))     strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".png")) strcpy(filetype, "image/png");
    else                               strcpy(filetype, "text/plain");
}

/*
 * run a CGI program on behalf of the client
 */
void get_dynamic(int fd, char *filename, char *cgi_args) {
    // return first part of HTTP response
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) {
        // real server would set all CGI vars here
        setenv("QUERY_STRING", cgi_args, 1);
        Dup2(fd, STDOUT_FILENO);
        char *empty_list[] = { NULL };
        Execve(filename, empty_list, environ);
    }
}

/*
 * return an error message to the client
 */
static void client_error(int fd, const char *cause, const char *err_num,
                         const char *short_msg, const char *long_msg) {
    // build the HTTP response body
    char body[MAXLINE];
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, err_num, short_msg);
    sprintf(body, "%s<p>%s: %s\r\n", body, long_msg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web Server</em>\r\n", body);

    // print the HTTP response
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.0 %s %s\r\n", err_num, short_msg);
    sprintf(buf, "%sContent-Type: text/html\r\n", buf);
    sprintf(buf, "%sContent-Length: %lu\r\n\r\n", buf, strlen(body));

    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(buf));
}

/*
 * if the process is running, the interger in the pid file is the pid, else if -1
 */
static void write_pid(int option) {
    int fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC);

    int pid;
    if (option) pid = (int)getpid();
    else        pid = -1;

    write(fd, (void *)&pid, sizeof(pid));
    Close(fd);
}
