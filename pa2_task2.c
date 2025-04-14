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
#define DEFAULT_CLIENT_THREADS 4
#define TIMEOUT_MS 100

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int client_id;
    int seq_num;
    char payload[MESSAGE_SIZE];
} __attribute__((packed)) frame_t;

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
    long tx_cnt;
    long rx_cnt;
    struct sockaddr_in server_addr;
    int client_id;
} client_thread_data_t;

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    struct timeval start, end;
    socklen_t addrlen = sizeof(data->server_addr);

    frame_t send_frame, recv_frame;
    memset(&send_frame, 0, sizeof(send_frame));
    memcpy(send_frame.payload, "ABCDEFGHIJKMLNOP", MESSAGE_SIZE);

    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl failed");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < num_requests; i++) {
        int acked = 0;
        send_frame.client_id = data->client_id;
        send_frame.seq_num = i;

        while (!acked) {
            gettimeofday(&start, NULL);
            if (sendto(data->socket_fd, &send_frame, sizeof(send_frame), 0,
                       (struct sockaddr *)&data->server_addr, addrlen) < 0) {
                perror("sendto failed");
                exit(EXIT_FAILURE);
            }
            data->tx_cnt++;

            int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, TIMEOUT_MS);
            if (nfds > 0) {
                int recv_len = recvfrom(data->socket_fd, &recv_frame, sizeof(recv_frame), 0,
                                        NULL, NULL);
                if (recv_len < 0) {
                    perror("recvfrom failed");
                    exit(EXIT_FAILURE);
                }

                if (recv_len == sizeof(frame_t) &&
                    recv_frame.client_id == data->client_id &&
                    recv_frame.seq_num == send_frame.seq_num) {
                    gettimeofday(&end, NULL);
                    long long rtt = (end.tv_sec - start.tv_sec) * 1000000LL +
                                    (end.tv_usec - start.tv_usec);
                    data->total_rtt += rtt;
                    data->total_messages++;
                    data->rx_cnt++;
                    acked = 1;
                }
            }
        }
    }

    data->request_rate = (float)data->total_messages / ((float)data->total_rtt / 1000000.0);
    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];

    for (int i = 0; i < num_client_threads; i++) {
        if ((thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            perror("socket failed");
            exit(EXIT_FAILURE);
        }

        if ((thread_data[i].epoll_fd = epoll_create1(0)) < 0) {
            perror("epoll_create1 failed");
            exit(EXIT_FAILURE);
        }

        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].tx_cnt = 0;
        thread_data[i].rx_cnt = 0;
        thread_data[i].client_id = i;

        thread_data[i].server_addr.sin_family = AF_INET;
        thread_data[i].server_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_ip, &thread_data[i].server_addr.sin_addr) <= 0) {
            perror("inet_pton failed");
            exit(EXIT_FAILURE);
        }

        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    long long total_rtt = 0;
    long total_messages = 0;
    long total_tx = 0;
    long total_rx = 0;
    float total_request_rate = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
    }

    printf("Average RTT: %lld us\n", total_rtt / total_messages);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Packets Sent (tx): %ld\n", total_tx);
    printf("Packets Received (rx): %ld\n", total_rx);
    printf("Packets Lost (retransmitted): %ld\n", total_tx - total_rx);
}

void run_server() {
    int server_fd, epoll_fd;
    struct epoll_event event, events[MAX_EVENTS];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    frame_t buffer;

    if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton failed");
        exit(EXIT_FAILURE);
    }

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if ((epoll_fd = epoll_create1(0)) < 0) {
        perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }

    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("epoll_ctl failed");
        exit(EXIT_FAILURE);
    }

    printf("UDP Server (ARQ) started on port %d. Now waiting for packets!\n", server_port);

    while (1) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < num_events; i++) {
            int bytes_read = recvfrom(server_fd, &buffer, sizeof(buffer), 0,
                                      (struct sockaddr *)&client_addr, &client_len);
            if (bytes_read < 0) {
                perror("recvfrom failed");
                continue;
            }

            if (sendto(server_fd, &buffer, bytes_read, 0,
                       (struct sockaddr *)&client_addr, client_len) < 0) {
                perror("sendto failed");
                continue;
            }
        }
    }
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
