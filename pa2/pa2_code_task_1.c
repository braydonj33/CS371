#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <pthread.h>
#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define DEFAULT_NUM_REQUESTS 1000000
#define PIPELINE_DEPTH 32
#define EPOLL_TIMEOUT_MS 50
char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = DEFAULT_NUM_REQUESTS;
typedef struct {
    int epoll_fd;
    int socket_fd;
    struct sockaddr_in server_addr;

    long tx_cnt;
    long rx_cnt;
    long lost_pkt_cnt;

    long long total_runtime_us;
    float request_rate;
} client_thread_data_t;

static long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKLMNO";
    char recv_buf[MESSAGE_SIZE];
    socklen_t addr_len = sizeof(data->server_addr);
    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->lost_pkt_cnt = 0;
    data->request_rate = 0.0f;
    data->total_runtime_us = 0;
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl add client socket");
        close(data->socket_fd);
        close(data->epoll_fd);
        return NULL;
    }
    long long start_time = now_us();
    while (data->tx_cnt < num_requests) {
        int sent_this_round = 0;

        while (data->tx_cnt < num_requests && sent_this_round < PIPELINE_DEPTH) {
            ssize_t n = sendto(data->socket_fd,
                               send_buf,
                               MESSAGE_SIZE,
                               0,
                               (struct sockaddr *)&data->server_addr,
                               addr_len);
            if (n == MESSAGE_SIZE) {
                data->tx_cnt++;
                sent_this_round++;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                perror("sendto");
                break;
            }
        }
        int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);
        if (n_events < 0) {
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == data->socket_fd) {
                while (1) {
                    ssize_t n = recvfrom(data->socket_fd,
                                         recv_buf,
                                         MESSAGE_SIZE,
                                         0,
                                         NULL,
                                         NULL);
                    if (n == MESSAGE_SIZE) {
                        data->rx_cnt++;
                    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else if (n < 0) {
                        perror("recvfrom");
                        break;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    long long drain_deadline = now_us() + 500000;
    while (now_us() < drain_deadline) {
        int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 10);
        if (n_events < 0) {
            perror("epoll_wait drain");
            break;
        }
        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == data->socket_fd) {
                while (1) {
                    ssize_t n = recvfrom(data->socket_fd,
                                         recv_buf,
                                         MESSAGE_SIZE,
                                         0,
                                         NULL,
                                         NULL);
                    if (n == MESSAGE_SIZE) {
                        data->rx_cnt++;
                    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    long long end_time = now_us();
    data->total_runtime_us = end_time - start_time;
    data->lost_pkt_cnt = data->tx_cnt - data->rx_cnt;
    if (data->total_runtime_us > 0) {
        data->request_rate =
            (float)data->rx_cnt * 1000000.0f / (float)data->total_runtime_us;
    }
    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client(void) {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];

    for (int i = 0; i < num_client_threads; i++) {
        memset(&thread_data[i], 0, sizeof(thread_data[i]));
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd < 0) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0) {
            perror("socket");
            close(thread_data[i].epoll_fd);
            exit(EXIT_FAILURE);
        }
        if (set_nonblocking(thread_data[i].socket_fd) < 0) {
            perror("set_nonblocking");
            close(thread_data[i].socket_fd);
            close(thread_data[i].epoll_fd);
            exit(EXIT_FAILURE);
        }
        memset(&thread_data[i].server_addr, 0, sizeof(thread_data[i].server_addr));
        thread_data[i].server_addr.sin_family = AF_INET;
        thread_data[i].server_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_ip, &thread_data[i].server_addr.sin_addr) <= 0) {
            perror("inet_pton");
            close(thread_data[i].socket_fd);
            close(thread_data[i].epoll_fd);
            exit(EXIT_FAILURE);
        }
    }
    for (int i = 0; i < num_client_threads; i++) {
        if (pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    long total_tx = 0;
    long total_rx = 0;
    long total_lost = 0;
    float total_request_rate = 0.0f;
    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_lost += thread_data[i].lost_pkt_cnt;
        total_request_rate += thread_data[i].request_rate;

        printf("Thread %d: tx=%ld rx=%ld lost=%ld rate=%.2f msg/s\n",
               i,
               thread_data[i].tx_cnt,
               thread_data[i].rx_cnt,
               thread_data[i].lost_pkt_cnt,
               thread_data[i].request_rate);
    }
    printf("\n=== Client Summary ===\n");
    printf("Total tx_cnt      : %ld\n", total_tx);
    printf("Total rx_cnt      : %ld\n", total_rx);
    printf("Total lost_pkt_cnt: %ld\n", total_lost);
    printf("Aggregate goodput : %.2f msg/s\n", total_request_rate);
}

void run_server(void) {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("epoll_ctl add server socket");
        close(server_fd);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n_events < 0) {
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == server_fd) {
                while (1) {
                    char buffer[MESSAGE_SIZE];
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    ssize_t n = recvfrom(server_fd,
                                         buffer,
                                         MESSAGE_SIZE,
                                         MSG_DONTWAIT,
                                         (struct sockaddr *)&client_addr,
                                         &client_len);

                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    if (n < 0) {
                        perror("recvfrom");
                        break;
                    }
                    if (n > 0) {
                        ssize_t s = sendto(server_fd,
                                           buffer,
                                           n,
                                           0,
                                           (struct sockaddr *)&client_addr,
                                           client_len);
                        if (s < 0) {
                            perror("sendto");
                        }
                    }
                }
            }
        }
    }
    close(server_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }
        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }
        if (argc > 4) {
            num_client_threads = atoi(argv[4]);
        }
        if (argc > 5) {
            num_requests = atoi(argv[5]);
        }
        run_client();
    } else {
        printf("Usage:\n");
        printf("  %s server <server_ip> <server_port>\n", argv[0]);
        printf("  %s client <server_ip> <server_port> <num_client_threads> <num_requests>\n", argv[0]);
    }
    return 0;
}
