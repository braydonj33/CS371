/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: Braydon Scott

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct {
    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
    int socket_fd;       /* File descriptor for the client socket connected to the server. */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
} client_thread_data_t;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == -1 && errno == EINTR) continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return -1;
    }
    return 0;
}

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP";
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
        perror("client epoll_ctl(ADD)");
        return NULL;
    }

    data->total_rtt = 0;
    data->total_messages = 0;

    struct timeval loop_start, loop_end;
    gettimeofday(&loop_start, NULL);

    for (int i = 0; i < num_requests; i++) {
        gettimeofday(&start, NULL);

        if (send_all(data->socket_fd, send_buf, MESSAGE_SIZE) != 0) {
            perror("client send");
            break;
        }

        for (;;) {
            int nready = epoll_wait(data->epoll_fd, events, MAX_EVENTS, -1);
            if (nready == -1) {
                if (errno == EINTR) continue;
                perror("client epoll_wait");
                goto done;
            }

            int got_reply = 0;
            for (int e = 0; e < nready; e++) {
                if (events[e].data.fd != data->socket_fd) continue;

                if (events[e].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    goto done;
                }

                if (events[e].events & EPOLLIN) {
                    ssize_t r = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                    if (r <= 0) {
                        goto done;
                    }
                    size_t recvd = (size_t)r;
                    while (recvd < MESSAGE_SIZE) {
                        r = recv(data->socket_fd, recv_buf + recvd, MESSAGE_SIZE - recvd, 0);
                        if (r > 0) {
                            recvd += (size_t)r;
                            continue;
                        }
                        if (r == -1 && errno == EINTR) continue;
                        if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                        goto done;
                    }
                    got_reply = 1;
                    break;
                }
            }

            if (got_reply) break;
        }

        gettimeofday(&end, NULL);
        long long rtt_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
        data->total_rtt += rtt_us;
        data->total_messages++;
    }

done:
    gettimeofday(&loop_end, NULL);
    double elapsed_s = (double)(loop_end.tv_sec - loop_start.tv_sec) +
                       (double)(loop_end.tv_usec - loop_start.tv_usec) / 1000000.0;
    data->request_rate = (elapsed_s > 0.0) ? (float)((double)data->total_messages / elapsed_s) : 0.0f;

    close(data->socket_fd);
    close(data->epoll_fd);

    return NULL;
}

/*
 * This function orchestrates multiple client threads to send requests to a server,
 * collect performance data of each threads, and compute aggregated metrics of all threads.
 */
void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0f;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < num_client_threads; i++) {
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd == -1) {
            perror("client socket");
            exit(EXIT_FAILURE);
        }
        if (set_nonblocking(sockfd) == -1) {
            perror("client set_nonblocking");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        int rc = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (rc == -1 && errno != EINPROGRESS) {
            perror("client connect");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        int epfd = epoll_create1(0);
        if (epfd == -1) {
            perror("client epoll_create1");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        thread_data[i].socket_fd = sockfd;
        thread_data[i].epoll_fd = epfd;
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].request_rate = 0.0f;
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;

        if (thread_data[i].total_messages > 0) {
            printf("Thread %d Avg RTT: %lld us\n", i,
                   thread_data[i].total_rtt / thread_data[i].total_messages);
            printf("Thread %d Request Rate: %f messages/s\n", i, thread_data[i].request_rate);
        } else {
            printf("Thread %d exchanged 0 messages\n", i);
        }
    }

    if (total_messages == 0) {
        fprintf(stderr, "No messages were successfully exchanged.\n");
        return;
    }

    printf("Average RTT: %lld us\n", total_rtt / total_messages);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
}

void run_server() {

    int listen_fd = -1;
    int epoll_fd = -1;
    struct sockaddr_in addr;
    struct epoll_event ev, events[MAX_EVENTS];

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("server socket");
        exit(EXIT_FAILURE);
    }

    int yes = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        perror("server setsockopt(SO_REUSEADDR)");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (set_nonblocking(listen_fd) == -1) {
        perror("server set_nonblocking(listen_fd)");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip);
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("server bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, 128) == -1) {
        perror("server listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("server epoll_create1");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("server epoll_ctl(ADD listen_fd)");
        close(epoll_fd);
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    /* Server's run-to-completion event loop */
    while (1) {

        int nready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nready == -1) {
            if (errno == EINTR) continue;
            perror("server epoll_wait");
            break;
        }

        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                for (;;) {
                    struct sockaddr_in caddr;
                    socklen_t clen = sizeof(caddr);
                    int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
                    if (cfd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        perror("server accept");
                        break;
                    }

                    if (set_nonblocking(cfd) == -1) {
                        perror("server set_nonblocking(client)");
                        close(cfd);
                        continue;
                    }

                    memset(&ev, 0, sizeof(ev));
                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.fd = cfd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &ev) == -1) {
                        perror("server epoll_ctl(ADD client)");
                        close(cfd);
                        continue;
                    }
                }
            } else {
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }

                if (events[i].events & EPOLLIN) {
                    char buf[MESSAGE_SIZE];
                    ssize_t r = recv(fd, buf, MESSAGE_SIZE, 0);
                    if (r <= 0) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        continue;
                    }

                    size_t recvd = (size_t)r;
                    while (recvd < MESSAGE_SIZE) {
                        r = recv(fd, buf + recvd, MESSAGE_SIZE - recvd, 0);
                        if (r > 0) {
                            recvd += (size_t)r;
                            continue;
                        }
                        if (r == -1 && errno == EINTR) continue;
                        if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        recvd = 0;
                        break;
                    }
                    if (recvd == 0) continue;

                    if (send_all(fd, buf, MESSAGE_SIZE) != 0) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        continue;
                    }
                }
            }
        }
    }

    close(epoll_fd);
    close(listen_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}
