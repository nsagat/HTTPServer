#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <regex.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>

#include "iowrapper.h"
#include "listener_socket.h"
#include "protocol.h"
#include "rwlock.h"
#include "queue.h"

#define BUFSIZE 4096
#define REQEX  "^([A-Z]{1,8}) /([A-Za-z0-9.\\-_]{1,63}) (HTTP/[0-9]\\.[0-9])\r\n"
#define HEADEX "([A-Za-z0-9-]+): ([ -~]+)\r\n"
#define OPTIONS "t:"


rwlock_t *rwlock_new(PRIORITY p, uint32_t n);
void reader_lock(rwlock_t *rw);
void reader_unlock(rwlock_t *rw);
void writer_lock(rwlock_t *rw);
void writer_unlock(rwlock_t *rw);
typedef struct {
    char *command, *target_path, *version, *message_body;
    int input_file_descriptor;
    int content_length, remaining_bytes;
    char *request_id;
} Request;
void  *worker(void *arg);
void   handle_connection(int connfd);
void   handle_get(Request *R);
void   handle_put(Request *R);
int    parse_request(Request *R, char *buf, ssize_t n);
ssize_t read_until(int fd, char buf[], size_t n, const char *str);

//lib for statussesss
static const char *get_status(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "Version Not Supported";
    default:  return "";
    }
}

static void send_header(int sockfd, int code, size_t len) {
    const char *phrase = get_status(code);
    dprintf(sockfd,
            "HTTP/1.1 %d %s\r\n"
            "Content-Length: %zu\r\n"
            "\r\n",
            code, phrase, len);
}

static void send_response(int sockfd, int code, const char *body) {
    size_t len = strlen(body);
    send_header(sockfd, code, len);
    if (len) write(sockfd, body, len);
}



//queue
static queue_t *q;

//linked list to keep track of path to lock nodes
//conflict/nonconflict testfcases
typedef struct pathlock {
    char *path; //uri
    rwlock_t *lock; //reader/writer lock
    struct pathlock *next;
} pathlock_t;

static pathlock_t *headptrList = NULL;
static pthread_mutex_t lockList_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER; //audit log


//rwlock for a single path
static rwlock_t *lock_forPath(const char *path) {
    pthread_mutex_lock(&lockList_mutex);
    pathlock_t *cur = headptrList;
    while(cur) {
        if (strcmp(cur->path, path) == 0) {
            rwlock_t *ret = cur->lock;
            pthread_mutex_unlock(&lockList_mutex);
            return ret;
        }
        cur = cur->next;
    }
    //make a new nodeif not found
    pathlock_t *node = malloc(sizeof(pathlock_t));
    if (!node) { 
        perror("malloc"); 
        exit(EXIT_FAILURE); 
    }
    node->path = strdup(path);
    if(!node->path){ 
        perror("strdup"); 
        free(node);
        exit(EXIT_FAILURE); 
    }
    node->lock = rwlock_new(WRITERS, 0);
    node->next = headptrList;
    headptrList = node;
    pthread_mutex_unlock(&lockList_mutex);
    return node->lock;
}

int main(int argc, char **argv) {
    int opt, nt = 4;
    while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
        if (opt == 't') {
            nt = atoi(optarg);
            if (nt < 1) {
                fprintf(stderr, "Invalid thread count: %s\n", optarg);
                fflush(stderr);
                exit(EXIT_FAILURE);
            }
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [-t <threads>] <port>\n", argv[0]);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    char *endptr = NULL;
    size_t port = strtoull(argv[optind], &endptr, 10);
    if (endptr == NULL || *endptr != '\0') {
        fprintf(stderr, "Invalid port: %s\n", argv[optind]);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    if (optind + 1 < argc) {
        fprintf(stderr, "Usage: %s [-t <threads>] <port>\n", argv[0]);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket_t *ls = ls_new(port);
    q = queue_new(nt);
    pthread_mutex_init(&log_lock, NULL);

    // create the  worker threads
    pthread_t threads[nt];
    for (int i = 0; i < nt; i++) {
        int err = pthread_create(&threads[i], NULL, worker, NULL);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            fflush(stderr);
            exit(EXIT_FAILURE);
        }
    }

    //accept loop
    while(true) {
        uintptr_t connfd = ls_accept(ls);
        if ((int)connfd < 0) continue;
        queue_push(q, (void *)connfd);
    }
    return 0;
}


//worker thread
void *worker(void *arg) {
    (void)arg;
    while (true) {
        uintptr_t rawfd;
        queue_pop(q, (void **)&rawfd);
        int connfd = (int)rawfd;
        handle_connection(connfd);
        close(connfd);
    }
    return NULL;
}


//read parse & dispatch
void handle_connection(int connfd) {
    char buf[BUFSIZE + 1];
    ssize_t n = read_until(connfd, buf, BUFSIZE, "\r\n\r\n");
    if (n < 0) {
        send_response(connfd, 400, "Bad Request\n");
        return;
    }
    Request R;
    memset(&R, 0, sizeof(R));
    R.input_file_descriptor = connfd;

    if (parse_request(&R, buf, n) == EXIT_SUCCESS) {
        if (strncmp(R.command, "GET", 3) == 0) {
            handle_get(&R);
        }
        else if (strncmp(R.command, "PUT", 3) == 0) {
            handle_put(&R);
        }
    }
}


int parse_request(Request *R, char *buf, ssize_t n) {
    int offset = 0;
    regex_t re_line, re_head;
    regmatch_t matches[4];

    // request‐line
    if (regcomp(&re_line, REQEX, REG_EXTENDED) != 0
        || regexec(&re_line, buf, 4, matches, 0) != 0) {
        send_response(R->input_file_descriptor, 400, "Bad Request\n");
        regfree(&re_line);
        return EXIT_FAILURE;
    }
    R->command = buf;
    buf[matches[1].rm_eo] = '\0';
    R->target_path = buf + matches[2].rm_so;
    buf[matches[2].rm_eo] = '\0';
    R->version = buf + matches[3].rm_so;
    buf[matches[3].rm_eo] = '\0';
    buf += matches[3].rm_eo + 2;
    offset += matches[3].rm_eo + 2;
    regfree(&re_line);

    R->content_length = -1;
    R->request_id = NULL;

    if (regcomp(&re_head, HEADEX, REG_EXTENDED) != 0) {
        send_response(R->input_file_descriptor, 400, "Bad Request\n");
        return EXIT_FAILURE;
    }
    while (regexec(&re_head, buf, 3, matches, 0) == 0) {
        buf[matches[1].rm_eo] = '\0';
        buf[matches[2].rm_eo] = '\0';
        char *field = buf + matches[1].rm_so;
        char *value = buf + matches[2].rm_so;
        if (strcmp(field, "Content-Length") == 0) {
            R->content_length = (int)strtol(value, NULL, 10);
        }
        else if (strcmp(field, "Request-Id") == 0) {
            R->request_id = value;
        }
        buf += matches[2].rm_eo + 2;
        offset += matches[2].rm_eo + 2;
    }
    regfree(&re_head);

    if (buf[0] == '\r' && buf[1] == '\n') {
        R->message_body = buf + 2;
        offset += 2;
        R->remaining_bytes = (int)(n - offset);
        if (!R->request_id) R->request_id = "0";
        return EXIT_SUCCESS;
    } else {
        send_response(R->input_file_descriptor, 400, "Bad Request\n");
        return EXIT_FAILURE;
    }
}
//asgn2
ssize_t read_until(int fd, char buf[], size_t n, const char *str) {
    size_t total = 0, target_len = (str ? strlen(str) : 0);
    while (total < n) {
        ssize_t r = read(fd, buf + total, 1);
        if (r == 0) break;
        if (r < 0) return -1;
        total += (size_t)r;
        if (str && total >= target_len) {
            for (size_t i = 0; i + target_len <= total; i++) {
                if (memcmp(buf + i, str, target_len) == 0) {
                    buf[total] = '\0';
                    return (ssize_t)total;
                }
            }
        }
    }
    buf[total] = '\0';
    return (ssize_t)total;
}


void handle_get(Request *R) {
    char *uri = R->target_path, *req_id = R->request_id;

    if (R->content_length != -1 || R->remaining_bytes > 0) {
        send_response(R->input_file_descriptor, 400, "Bad Request\n");
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "GET,/%s,400,%s\n", uri, req_id);
        fflush(stderr);
        pthread_mutex_unlock(&log_lock);
        return;
    }

    rwlock_t *plock = lock_forPath(uri);
    reader_lock(plock);

    int dirfd = open(uri, O_RDONLY | O_DIRECTORY);
    if (dirfd != -1) {
        close(dirfd);
        send_response(R->input_file_descriptor, 403, "Forbidden\n");
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "GET,/%s,403,%s\n", uri, req_id);
        fflush(stderr);
        pthread_mutex_unlock(&log_lock);
        reader_unlock(plock);
        return;
    }

    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
        int e = errno, status;
        if (e == ENOENT) { status = 404; send_response(R->input_file_descriptor, 404, "Not Found\n"); }
        else if (e == EACCES) { status = 403; send_response(R->input_file_descriptor, 403, "Forbidden\n"); }
        else { status = 500; send_response(R->input_file_descriptor, 500, "Internal Server Error\n"); }
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "GET,/%s,%d,%s\n", uri, status, req_id);
        fflush(stderr);
        pthread_mutex_unlock(&log_lock);
        reader_unlock(plock);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "GET,/%s,500,%s\n", uri, req_id);
        fflush(stderr);
        pthread_mutex_unlock(&log_lock);
        close(fd);
        reader_unlock(plock);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        send_response(R->input_file_descriptor, 403, "Forbidden\n");
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "GET,/%s,403,%s\n", uri, req_id);
        fflush(stderr);
        pthread_mutex_unlock(&log_lock);
        close(fd);
        reader_unlock(plock);
        return;
    }

    off_t size = st.st_size;
    send_header(R->input_file_descriptor, 200, (size_t)size);
    pass_n_bytes(fd, R->input_file_descriptor, (size_t)size);

    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "GET,/%s,200,%s\n", uri, req_id);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);

    close(fd);
    reader_unlock(plock);
}

void handle_put(Request *R) {
    char *uri    = R->target_path;
    char *req_id = R->request_id ? R->request_id : "0";

    if (R->content_length == -1) {
        send_response(R->input_file_descriptor, 400, "Bad Request\n");
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "PUT,/%s,400,%s\n", uri, req_id);
        pthread_mutex_unlock(&log_lock);
        return;
    }
    size_t content_len = (size_t)R->content_length;

    char template[] = "/tmp/httpserver-buffer-XXXXXX";
    int temp_fd = mkstemp(template);
    if(temp_fd < 0) {
        send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
        pthread_mutex_unlock(&log_lock);
        return;
    }
    unlink(template);
    ssize_t already = R->remaining_bytes;
    if (already > 0) {
        if(write(temp_fd, R->message_body, (size_t)already) < 0) {
            send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
            close(temp_fd);
            pthread_mutex_lock(&log_lock);
            fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
            pthread_mutex_unlock(&log_lock);
            return;
        }
    }
    size_t written = (already > 0 ? (size_t)already : 0);
    size_t to_read = content_len - written;
    if(to_read > 0) {
        if (pass_n_bytes(R->input_file_descriptor, temp_fd, to_read) < 0) {
            send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
            close(temp_fd);
            pthread_mutex_lock(&log_lock);
            fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
            pthread_mutex_unlock(&log_lock);
            return;
        }
    }

    if(lseek(temp_fd, 0, SEEK_SET) < 0) {
        send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
        close(temp_fd);
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
        pthread_mutex_unlock(&log_lock);
        return;
    }
    rwlock_t *plock = lock_forPath(uri);
    writer_lock(plock);
    int dirfd = open(uri, O_WRONLY | O_DIRECTORY);
    if(dirfd != -1) {
        close(dirfd);
        send_response(R->input_file_descriptor, 403, "Forbidden\n");
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "PUT,/%s,403,%s\n", uri, req_id);
        pthread_mutex_unlock(&log_lock);
        writer_unlock(plock);
        close(temp_fd);
        return;
    }

    bool existed = (access(uri, F_OK) == 0);
    int realfd;
    int status_code;
    if(!existed) {
        realfd = open(uri, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if(realfd < 0) {
            int e = errno;
            if(e == EACCES) {
                send_response(R->input_file_descriptor, 403, "Forbidden\n");
                pthread_mutex_lock(&log_lock);
                fprintf(stderr, "PUT,/%s,403,%s\n", uri, req_id);
                pthread_mutex_unlock(&log_lock);
                writer_unlock(plock);
                close(temp_fd);
                return;
            }
            else{
                send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
                pthread_mutex_lock(&log_lock);
                fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
                pthread_mutex_unlock(&log_lock);
                writer_unlock(plock);
                close(temp_fd);
                return;
            }
        }
        status_code = 201;
    }
    else{
        realfd = open(uri, O_WRONLY | O_TRUNC, 0600);
        if(realfd < 0) {
            int e = errno;
            if(e == EACCES){
                send_response(R->input_file_descriptor, 403, "Forbidden\n");
                pthread_mutex_lock(&log_lock);
                fprintf(stderr, "PUT,/%s,403,%s\n", uri, req_id);
                pthread_mutex_unlock(&log_lock);
                writer_unlock(plock);
                close(temp_fd);
                return;
            }
            else{
                send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
                pthread_mutex_lock(&log_lock);
                fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
                pthread_mutex_unlock(&log_lock);
                writer_unlock(plock);
                close(temp_fd);
                return;
            }
        }
        status_code = 200;
    }
    {
        char buffer[BUFSIZE];
        size_t remain = content_len;
        while (remain > 0) {
            ssize_t chunk = read(temp_fd, buffer, (remain < BUFSIZE ? remain : BUFSIZE));
            if (chunk < 0) {
                send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
                close(temp_fd);
                close(realfd);
                pthread_mutex_lock(&log_lock);
                fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
                pthread_mutex_unlock(&log_lock);
                writer_unlock(plock);
                return;
            }
            if (chunk == 0){
                break;
            }
            ssize_t w = write(realfd, buffer, (size_t)chunk);
            if (w < 0){
                send_response(R->input_file_descriptor, 500, "Internal Server Error\n");
                close(temp_fd);
                close(realfd);
                pthread_mutex_lock(&log_lock);
                fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
                pthread_mutex_unlock(&log_lock);
                writer_unlock(plock);
                return;
            }
            remain -= (size_t)w;
        }
    }
    close(realfd);
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "PUT,/%s,%d,%s\n", uri, status_code, req_id);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
    writer_unlock(plock);

    if (status_code == 201) {
        send_response(R->input_file_descriptor, 201, "Created\n");
    } else {
        send_response(R->input_file_descriptor, 200, "OK\n");
    }
    close(temp_fd);
}




















