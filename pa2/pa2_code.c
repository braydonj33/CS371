#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define WINDOW_SIZE 32
#define TIMEOUT_US 50000

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = 4;
int num_requests = 100000;

typedef struct {
    int socket_fd;
    struct sockaddr_in server_addr;
    long tx_cnt;
    long rx_cnt;

} client_thread_data_t;

typedef struct {
    int seq;
    char payload[MESSAGE_SIZE];
} packet_t;

typedef struct {
    int ack;
} ack_t;

long get_time_us() {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*1000000LL + tv.tv_usec;
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = arg;

    packet_t pkt;
    ack_t ack;

    socklen_t addr_len = sizeof(data->server_addr);

    int base = 0;
    int nextseq = 0;

    long timer = 0;

    while (base < num_requests) {
        while (nextseq < base + WINDOW_SIZE && nextseq < num_requests) {
            pkt.seq = nextseq;
            memset(pkt.payload,'A',MESSAGE_SIZE);
            sendto(data->socket_fd,
                   &pkt,
                   sizeof(pkt),
                   0,
                   (struct sockaddr*)&data->server_addr,
                   addr_len);

            if (base == nextseq)
                timer = get_time_us();

            nextseq++;
            data->tx_cnt++;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(data->socket_fd,&readfds);

        int rv = select(data->socket_fd+1,&readfds,NULL,NULL,&tv);

        if (rv > 0) {
            recvfrom(data->socket_fd,
                     &ack,
                     sizeof(ack),
                     0,
                     NULL,
                     NULL);

            if (ack.ack >= base) {
                base = ack.ack + 1;
                data->rx_cnt++;

                if (base == nextseq)
                    timer = 0;
                else
                    timer = get_time_us();
            }
        }

        if (timer && get_time_us() - timer > TIMEOUT_US) {
            for (int i = base; i < nextseq; i++) {
                pkt.seq = i;
                sendto(data->socket_fd,
                       &pkt,
                       sizeof(pkt),
                       0,
                       (struct sockaddr*)&data->server_addr,
                       addr_len);
            }

            timer = get_time_us();
        }
    }

    close(data->socket_fd);

    printf("Thread finished: tx=%ld rx=%ld lost=%ld\n",
           data->tx_cnt,
           data->rx_cnt,
           data->tx_cnt - data->rx_cnt);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t data[num_client_threads];

    for (int i=0;i<num_client_threads;i++) {
        data[i].socket_fd = socket(AF_INET,SOCK_DGRAM,0);

        memset(&data[i].server_addr,0,sizeof(struct sockaddr_in));

        data[i].server_addr.sin_family = AF_INET;
        data[i].server_addr.sin_port = htons(server_port);
        inet_pton(AF_INET,server_ip,&data[i].server_addr.sin_addr);

        data[i].tx_cnt = 0;
        data[i].rx_cnt = 0;

        pthread_create(&threads[i],NULL,client_thread_func,&data[i]);
    }

    for (int i=0;i<num_client_threads;i++)
        pthread_join(threads[i],NULL);
}

void run_server() {
    int server_fd = socket(AF_INET,SOCK_DGRAM,0);

    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    memset(&server_addr,0,sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    bind(server_fd,(struct sockaddr*)&server_addr,sizeof(server_addr));

    packet_t pkt;
    ack_t ack;

    int expected = 0;

    while(1) {
        int n = recvfrom(server_fd,
                         &pkt,
                         sizeof(pkt),
                         0,
                         (struct sockaddr*)&client_addr,
                         &addr_len);

        if (n <= 0) continue;

        if (pkt.seq == expected) {
            expected++;
        }

        ack.ack = expected - 1;

        sendto(server_fd,
               &ack,
               sizeof(ack),
               0,
               (struct sockaddr*)&client_addr,
               addr_len);
    }
    close(server_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1],"server")==0) {
        if(argc > 2) server_ip = argv[2];
        if(argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1],"client")==0) {
        if(argc > 2) server_ip = argv[2];
        if(argc > 3) server_port = atoi(argv[3]);
        if(argc > 4) num_client_threads = atoi(argv[4]);
        if(argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage:\n");
        printf("./pa2_binary server <ip> <port>\n");
        printf("./pa2_binary client <ip> <port> <threads> <requests>\n");
    }
    return 0;
}
