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
# Student #1: Alex Raketich
# Student #2: N/A
# Student #3: N/A
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

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
    long tx_cnt;
    long rx_cnt;
    struct sockaddr_in server_addr;
} client_thread_data_t;

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    struct timeval start, end;
    socklen_t addrlen = sizeof(data->server_addr);

    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP";
    char recv_buf[MESSAGE_SIZE];

    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl failed");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < num_requests; i++) {
        gettimeofday(&start, NULL);
        if (sendto(data->socket_fd, send_buf, MESSAGE_SIZE, 0,
                   (struct sockaddr *)&data->server_addr, addrlen) < 0) {
            perror("sendto failed");
            exit(EXIT_FAILURE);
        }
        data->tx_cnt++;

        epoll_wait(data->epoll_fd, events, MAX_EVENTS, -1);
        int recv_len = recvfrom(data->socket_fd, recv_buf, MESSAGE_SIZE, 0,
                                NULL, NULL);
        if (recv_len > 0) {
            gettimeofday(&end, NULL);
            long long rtt = (end.tv_sec - start.tv_sec) * 1000000LL +
                            (end.tv_usec - start.tv_usec);
            data->total_rtt += rtt;
            data->total_messages++;
            data->rx_cnt++;
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
    printf("Packets Lost: %ld\n", total_tx - total_rx);
}

void run_server() {
    int server_fd, epoll_fd;
    struct epoll_event event, events[MAX_EVENTS];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[MESSAGE_SIZE];

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

    printf("UDP Server started on port %d. Now waiting for packets!\n", server_port);

    while (1) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < num_events; i++) {
            int bytes_read = recvfrom(server_fd, buffer, MESSAGE_SIZE, 0,
                                      (struct sockaddr *)&client_addr, &client_len);
            if (bytes_read < 0) {
                perror("recvfrom failed");
                continue;
            }

            if (sendto(server_fd, buffer, bytes_read, 0,
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
