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

    // config address
    if ((addrinfo_err = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(addrinfo_err));
        return -1;
    }

    // bind first available socket
    struct addrinfo *p = res;
    for (p = res; p != NULL; p = p->ai_next) {
        if ((sock_fd = socket(p->ai_family,
                              p->ai_socktype,
                              p->ai_protocol)) < 0) {
            fprintf(stderr, "socket creation failed: %s, trying next...\n",
                    strerror(errno));
            continue;
        }

        if (bind(sock_fd, p->ai_addr, p->ai_addrlen) < 0) {
            if (p->ai_family == AF_INET) {
                char address[STRSIZE];
                if (inet_ntop(AF_INET,
                              &((struct sockaddr_in *)p->ai_addr)->sin_addr,
                              address, STRSIZE) == NULL) {
                    perror("invalid address");
                    continue;
                }
                fprintf(
                    stderr,
                    "bind on %s:%hu failed: %s, trying next...\n",
                    address,
                    ((struct sockaddr_in *)p->ai_addr)->sin_port,
                    strerror(errno)
                );
                continue;
            } else if (p->ai_family == AF_INET6) {
                char address[STRSIZE];
                if (inet_ntop(AF_INET6,
                              &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr,
                              address, STRSIZE) == NULL) {
                    perror("invalid address");
                    continue;
                }
                fprintf(
                    stderr,
                    "bind on %s:%hu failed: %s, trying next...\n",
                    address,
                    ((struct sockaddr_in6 *)p->ai_addr)->sin6_port,
                    strerror(errno)
                );
                continue;
            } else {
                fprintf(stderr,
                        "address must be in IPv4 or IPv6 format...exiting\n"
                       );
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

void get_content_type(const char *filename, char *buffer, size_t size) {
    const char *dot = strchr(filename, '.');
    char ext[STRSIZE];
    if (dot && dot != filename) {
        int i = 0;
        while(*(dot + i + 1) != '\0') {
            ext[i] = *(dot + i + 1);
            i++;
        }
        ext[i] = '\0';
    } else {
        *ext = '\0';
    }

    printf("%s\n", filename);
    printf("%s\n", ext);
    if (strcmp(ext, "txt") == 0) {
        snprintf(buffer, size, "Content-Type: text/plain");
    } else if (strcmp(ext, "csv") == 0) {
        snprintf(buffer, size, "Content-Type: text/csv");
    } else if (strcmp(ext, "html") == 0) {
        snprintf(buffer, size, "Content-Type: text/html");
    } else if (strcmp(ext, "xml") == 0) {
        snprintf(buffer, size, "Content-Type: application/xml");
    } else if (strcmp(ext, "json") == 0) {
        snprintf(buffer, size, "Content-Type: application/json");
    } else {
        snprintf(buffer, size, "Content-Type: application/octet-stream");
    }
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
        
        // setup response headers
        char date[STRSIZE];
        time_t timer = time(NULL);
        struct tm *tm = gmtime(&timer);
        strftime(date, STRSIZE, "Date: %a, %d %b %Y %H:%M:%S %Z", tm);
        char server[STRSIZE];
        snprintf(server, STRSIZE, "Server: simple-server/0.01");
        char content_type[STRSIZE];

        // setup buffers for regexec
        size_t nmatch = 2;
        regmatch_t startline_match[nmatch];
        
        // compile regex
        int errcode;
        char regerr[STRSIZE];
        regex_t getreg;
        regex_t postreg;
        if ((errcode = regcomp(&getreg,
                               "^GET /([^ ]*) HTTP/1",
                               REG_EXTENDED)) != 0) {
            memset(regerr, 0, STRSIZE);
            regerror(errcode, &getreg, regerr, STRSIZE);
            fprintf(stderr, "regex compilation failed: %s\n", regerr);
            snprintf(response, BUFSIZE,
                     "HTTP/1.0 500 Server Error\r\n"
                     "%s\r\n"
                     "%s\r\n"
                     "Content-Length: 0\r\n"
                     "\r\n",
                     date,
                     server
                    );
            response_len = strlen(response);
            send(client_fd, response, response_len, 0);
            // cleanup before exiting thread
            free(buf); buf = NULL;
            free(response); response = NULL;
            return NULL;
        }
        if ((errcode = regcomp(&postreg, 
                               "^POST /([^ ]*) HTTP/",
                               REG_EXTENDED)) != 0) {
            memset(regerr, 0, STRSIZE);
            regerror(errcode, &postreg, regerr, STRSIZE);
            fprintf(stderr, "regex compilation failed: %s\n", regerr);
            snprintf(response, BUFSIZE,
                     "HTTP/1.0 500 Server Error\r\n"
                     "%s\r\n"
                     "%s\r\n"
                     "Content-Length: 0\r\n"
                     "\r\n",
                     date,
                     server
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
            // get filename from regex match
            char filename[STRSIZE];
            int i = 0;
            char *p = buf + startline_match[1].rm_so;
            while (*p != ' ' && i < STRSIZE) {
                filename[i++] = *p++;
            }
            filename[i] = '\0';

            // open file and add data to response
            FILE* fp = fopen(filename, "r");
            if (fp == NULL) {
                snprintf(response, BUFSIZE,
                         "HTTP/1.0 404 Not Found\r\n"
                         "%s\r\n"
                         "%s\r\n"
                         "Content-Length: 0\r\n"
                         "\r\n",
                         date,
                         server
                        );
                response_len = strlen(response);
                send(client_fd, response, response_len, 0);
            } else {
                // get content type
                get_content_type(filename, content_type, STRSIZE);

                // add status line to response buffer
                fseek(fp, 0L, SEEK_END);
                snprintf(response, BUFSIZE,
                         "HTTP/1.0 200 OK\r\n"
                         "%s\r\n"
                         "%s\r\n"
                         "%s\r\n"
                         "Content-Length: %ld\r\n"
                         "\r\n",
                         date,
                         server,
                         content_type,
                         ftell(fp)
                        );
                rewind(fp);
                response_len = strlen(response);

                // read file contents into response buffer
                ssize_t bytesread;
                while ((bytesread = fread(response + response_len,
                                          1, 
                                          READSIZE, 
                                          fp)) > 0) {
                    response_len += bytesread;
                }
                if (ferror(fp) != 0) { // read error
                    perror("failed to read file");
                    snprintf(response, BUFSIZE,
                             "HTTP/1.0 500 Server Error\r\n"
                             "%s\r\n"
                             "%s\r\n"
                             "Content-Length: 0\r\n"
                             "\r\n",
                             date,
                             server
                            );
                    response_len = strlen(response);
                    send(client_fd, response, response_len, 0);
                } else { // send full response
                    send(client_fd, response, response_len, 0);
                }

                fclose(fp); // also closes file_fd
            }
        } else if (regexec(&postreg, buf, nmatch, startline_match, 0) == 0) {
            // TODO
        } else { // unsupported operation
            snprintf(response, BUFSIZE,
                     "HTTP/1.0 501 Not Implemented\r\n"
                     "%s\r\n"
                     "%s\r\n"
                     "Content-Length: 0\r\n"
                     "\r\n",
                     date,
                     server
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
        if ((*client_fd = accept(sock_fd,
                                 &client_addr,
                                 &client_addr_len)) < 0) {
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
