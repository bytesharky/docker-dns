#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "config.h"
#include "logging.h"
#include "daemon.h"
#include "sigterm.h"
#include "queue.h"
#include "dns.h"
#include "gateway.h"


struct in_addr gateway_addr;                // 存储网关IP地址

// 任务线程
void* worker_thread(void *arg) {
    int sockfd = *(int*)arg;
    while (1) {
        dns_request_t req;
        dequeue_request(&req);

        process_dns_query(sockfd, req.data, req.len, &req.client_addr, req.client_len);
    }
    return NULL;
}

// 主程序入口
int main(int argc, char *argv[]) {

    init_config_env();

    init_config_argc(argc, argv);

    if (!foreground) daemonize();

    setup_signal_handlers();

    log_msg(LOG_INFO, "Starting Sharky DNS forwarder (log level=%d->%s)", log_level, level_str[log_level]);

    log_msg(LOG_INFO, "Set container name to %s", container_name);
        
    if (!test_forward_dns()) {
        log_msg(LOG_WARN, "Forward DNS server may not be available");
    } 

    int sockfd;
    struct sockaddr_in server_addr;

    gateway_addr.s_addr = 0;
    if (resolve_gateway_ip() != 0) {
        log_msg(LOG_WARN, "Failed to resolve gateway IP at startup");
    } else {
        if (gateway_name[0]) {
            log_msg(LOG_INFO, "Gateway %s%s IP resolved to: %s",gateway_name ,suffix_domain , inet_ntoa(gateway_addr));
        } else {
            log_msg(LOG_WARN, "Gateway name undefined");
            log_msg(LOG_INFO, "Gateway IP resolved to: %s", inet_ntoa(gateway_addr));
        }
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        log_msg(LOG_FATAL, "Failed to create socket: %s", strerror(errno));
        perror("socket");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        log_msg(LOG_FATAL, "Failed to set SO_REUSEADDR on socket: %s", strerror(errno));
        perror("setsockopt SO_REUSEADDR");
        close(sockfd);
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(listen_port);

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_msg(LOG_FATAL, "Failed to bind socket to port %d", listen_port);
        perror("bind"); 
        close(sockfd);
        return 1;
    }

    struct timeval tv = {1, 0};
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        log_msg(LOG_FATAL, "Failed to set SO_RCVTIMEO on socket");
        perror("setsockopt SO_RCVTIMEO");
        close(sockfd);
        return 1;
    }

    log_msg(LOG_INFO, "DNS forwarder listening on port %d, forwarding *%s to %s (suffix: %s)",
            listen_port, suffix_domain, forward_dns, keep_suffix ? "keep" : "strip");

    log_msg(LOG_INFO, "Create DNS pthread (workers: %d, hops: %d)", num_workers, max_hops);
    for (int i = 0; i < num_workers; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, worker_thread, &sockfd);
        pthread_detach(tid);
    }

    while (!stop) {
        dns_request_t req;
        req.client_len = sizeof(req.client_addr);

        ssize_t n = recvfrom(sockfd, req.data, sizeof(req.data), 0, 
                            (struct sockaddr*)&req.client_addr, &req.client_len);
        if (n < 0) {
            if (errno == EINTR ||
                errno == EAGAIN || 
                errno == EWOULDBLOCK) continue;
            log_msg(LOG_ERROR, "Failed to receive data: %s", strerror(errno));
            break;
        }
        enqueue_request(&req);
    }

    log_msg(LOG_INFO, "Shutting down gracefully");
    close(sockfd);
    return 0;
}