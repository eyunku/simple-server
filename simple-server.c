#include "simple-server.h"

#define PORT "8080"
// number of backlog connections before failure
const int BACKLOG = 10;
// STRSIZE for string buffers, BUFSIZE for file buffers
const int STRSIZE = 128, BUFSIZE = 1048576;
// number of bytes to read from files at a time
const int READSIZE = 64;
int sock_fd;

int setup_server() {
    int addrinfo_err;
    struct addrinfo hints, *res;

    // config hints for getaddrinfo
    memset(&hints, 0, sizeof hints);
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // config socket
    if ((addrinfo_err = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(addrinfo_err));
        return -1;
    }

    // bind first available socket
    struct addrinfo *p = res;
    for (p = res; p != NULL; p = p->ai_next) {
        if ((sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) {
            fprintf(stderr, "socket creation failed: %s, trying next...\n", strerror(errno));
            continue;
        }

        if (bind(sock_fd, p->ai_addr, p->ai_addrlen) < 0) {
            if (p->ai_family == AF_INET) {
                char ip[STRSIZE];
                if (inet_ntop(AF_INET,
                              &((struct sockaddr_in *)p->ai_addr)->sin_addr,
                              ip, STRSIZE) == NULL) {
                    fprintf(stderr, "invalid address type detected\n");
                    continue;
                }
                fprintf(
                    stderr,
                    "bind on %s:%hu failed: %s, trying next...\n",
                    ip,
                    ((struct sockaddr_in *)p->ai_addr)->sin_port,
                    strerror(errno)
                );
                continue;
            } else if (p->ai_family == AF_INET6) {
                char ip[STRSIZE];
                if (inet_ntop(AF_INET6,
                              &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr,
                              ip, STRSIZE) == NULL) {
                    fprintf(stderr, "invalid address type detected\n");
                    continue;
                }
                fprintf(
                    stderr,
                    "bind on %s:%hu failed: %s, trying next...\n",
                    ip,
                    ((struct sockaddr_in6 *)p->ai_addr)->sin6_port,
                    strerror(errno)
                );
                continue;
            } else {
                fprintf(stderr, "unexpected sa_family value: exiting\n");
                freeaddrinfo(res);
                close(sock_fd);
                return -1;
            }
        }

        // successful bind, cleanup and exit loop
        freeaddrinfo(res);
        break;
    }

    if (p == NULL) { // failed to bind
        fprintf(stderr, "all binds failed\n");
        freeaddrinfo(res);
        close(sock_fd);
        return -1;
    }

    return 0;
}

int get_content_type(const char *buf, char *content_type) {
    return 0;
}

void* handle_client(void* argp) {
    int client_fd = *((int *)argp);
    char *buf = malloc(BUFSIZE * sizeof(char));
    if (buf == NULL) {
        fprintf(stderr, "insufficient thread space\n");
        return NULL;
    }

    // receive message from client socket
    ssize_t bytesrecv;
    if ((bytesrecv = recv(client_fd, buf, BUFSIZE, 0)) < 0) {
        perror("receive failed");
    } else if (bytesrecv > 0) { // ignore empty requests
        // allocate response buffer
        size_t response_len;
        char *response = malloc(BUFSIZE * sizeof(char));
        if (response == NULL) {
            fprintf(stderr, "insufficient thread space\n");
            free(buf); buf = NULL;
            return NULL;
        }
        
        // setup buffers for regexec
        size_t nmatch = 2;
        regmatch_t startline_match[nmatch];
        
        // compile regex
        int errcode;
        char regerr[STRSIZE];
        regex_t getreg;
        regex_t postreg;
        if ((errcode = regcomp(&getreg, "^GET /([^ ]*) HTTP/1", REG_EXTENDED)) != 0) {
            memset(regerr, 0, STRSIZE);
            regerror(errcode, &getreg, regerr, STRSIZE);
            fprintf(stderr, "regex compilation failed: %s\n", regerr);
            snprintf(response, BUFSIZE,
                     "HTTP/1.1 500 Server Error\r\n"
                     "\r\n"
                    );
            response_len = strlen(response);
            send(client_fd, response, response_len, 0);
            // cleanup before exiting thread
            free(buf); buf = NULL;
            free(response); response = NULL;
            return NULL;
        }
        if ((errcode = regcomp(&postreg, "^POST /([^ ]*) HTTP/", REG_EXTENDED)) != 0) {
            memset(regerr, 0, STRSIZE);
            regerror(errcode, &postreg, regerr, STRSIZE);
            fprintf(stderr, "regex compilation failed: %s\n", regerr);
            snprintf(response, BUFSIZE,
                     "HTTP/1.1 500 Server Error\r\n"
                     "\r\n"
                    );
            response_len = strlen(response);
            send(client_fd, response, response_len, 0);
            // cleanup before exiting thread
            regfree(&getreg);
            free(buf); buf = NULL;
            free(response); response = NULL;
            return NULL;
        }

        // generate response
        if (regexec(&getreg, buf, nmatch, startline_match, 0) == 0) {
            // regex match starts at filename
            char filename[STRSIZE];
            int i = 0;
            char *p = buf + startline_match[1].rm_so;
            while (*p != ' ' && i < STRSIZE) {
                filename[i++] = *p++;
            }
            filename[i] = '\0';

            // open file and add data to response
            int file_fd = open(filename, O_RDONLY);
            if (file_fd < 0) {
                snprintf(response, BUFSIZE,
                         "HTTP/1.1 404 Not Found\r\n"
                         "\r\n"
                        );
                response_len = strlen(response);
                send(client_fd, response, response_len, 0);
            } else {
                // add status line to response buffer
                snprintf(response, BUFSIZE,
                         "HTTP/1.1 200 OK\r\n"
                         "\r\n"
                        );
                response_len = strlen(response);

                // read file contents into response buffer
                ssize_t bytesread;
                while ((bytesread = read(file_fd, response + response_len, READSIZE)) > 0) {
                    response_len += bytesread;
                }

                if (bytesread < 0) { // read error
                    perror("failed to read file");
                    snprintf(response, BUFSIZE,
                             "HTTP/1.1 500 Server Error\r\n"
                             "\r\n"
                            );
                    response_len = strlen(response);
                    send(client_fd, response, response_len, 0);
                } else { // send full response
                    send(client_fd, response, response_len, 0);
                }

                close(file_fd);
            }
        } else if (regexec(&postreg, buf, nmatch, startline_match, 0) == 0) {
            // TODO
        } else { // bad request
            snprintf(response, BUFSIZE,
                     "HTTP/1.1 400 Bad Request\r\n"
                     "\r\n"
                    );
            response_len = strlen(response);
            send(client_fd, response, response_len, 0);
        }

        // free regex buffers
        regfree(&getreg);
        regfree(&postreg);

        // free response buffer
        free(response); response = NULL;
    }

    free(buf); buf = NULL;
    return NULL;
}

int main(void) {
    if (setup_server() < 0) {
        exit(EXIT_FAILURE);
    }

    // listen for connections
    if (listen(sock_fd, BACKLOG) < 0) {
        perror("listen failed");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // setup client socket
    struct sockaddr client_addr;
    socklen_t client_addr_len = sizeof client_addr;
    int *client_fd = malloc(sizeof(int));
    while(1) {
        if ((*client_fd = accept(sock_fd, &client_addr, &client_addr_len)) < 0) {
            perror("accept failed");
            continue;
        }

        // handle client in separate thread
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_fd) != 0) {
            perror("thread creation failed");
            continue;
        }
        pthread_detach(thread_id);
    }

    free(client_fd); client_fd = NULL;
    close(sock_fd);
    return 0;
}
