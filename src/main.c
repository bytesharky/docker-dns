#include "config.h"      // for init_config_argc, init_config_env, listen_port
#include "daemon.h"      // for daemonize
#include "hosts.h"       // for hosts_load
#include "dns.h"         // for process_dns_query, test_forward_dns
#include "doh.h"         // for doh_server_thread
#include "gateway.h"     // for resolve_gateway_ip
#include "logging.h"     // for log_msg, LOG_INFO, LOG_FATAL, LOG_WARN, log_...
#include "queue.h"       // for dns_request_t, dequeue_request, enqueue_request
#include "sigterm.h"     // for setup_signal_handlers, stop
#include <arpa/inet.h>   // for inet_ntoa, htons
#include <errno.h>       // for errno, EAGAIN, EINTR, EWOULDBLOCK
#include <netinet/in.h>  // for sockaddr_in, in_addr, INADDR_ANY
#include <pthread.h>     // for pthread_create, pthread_detach, pthread_t
#include <stdio.h>       // for perror, ssize_t
#include <string.h>      // for strerror
#include <sys/socket.h>  // for setsockopt, bind, recvfrom, socket, AF_INET
#include <sys/time.h>    // for timeval
#include <unistd.h>      // for close, NULL

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

#ifdef _WIN32
    /* Winsock 必须先初始化，否则所有 socket() 调用都会失败。
     * 守护进程会重新拉起自身（CreateProcessW），每个子进程都会重新执行 main()，
     * 因此在这里初始化即可覆盖前后台两种运行模式。 */
    {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            fprintf(stderr, "WSAStartup failed (%d)\n", WSAGetLastError());
            return 1;
        }
    }
#endif

    init_config_env();

    init_config_argc(argc, argv);

    /* 加载本地 hosts 文件（路径来自 --hosts / HOSTS_FILE，默认按平台取 /etc/hosts
     * 或 Windows 的 drivers/etc/hosts）。文件不存在时只打 WARN，不影响启动。 */
    hosts_load(hosts_path);

    if (!foreground) daemonize();

    setup_signal_handlers();

    log_msg(LOG_INFO, "Welcome to use Sharky DNS forwarder");
    
    log_msg(LOG_INFO, "Version: %s. ldns version: %s", VERSION, LDNS_VERSION);

    log_msg(LOG_INFO, "Starting Sharky DNS forwarder...");

    log_msg(LOG_INFO, "Set log level to %d: %s", log_level, level_str[log_level]);

    log_msg(LOG_INFO, "Server hostname: %s", container_name);


    if (!test_forward_dns()) {
        log_msg(LOG_WARN, "Forward DNS server may not be available");
    } else {
        log_msg(LOG_INFO, "The forward DNS server is available");
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

    /* 启动内置 DoH / HTTP-DNS 服务器线程（复用同一份解析逻辑） */
    pthread_t doh_tid;
    if (pthread_create(&doh_tid, NULL, doh_server_thread, NULL) == 0) {
        pthread_detach(doh_tid);
    } else {
        log_msg(LOG_WARN, "Failed to start DoH/HTTP server thread");
    }

    while (!stop) {
        dns_request_t req;
        req.client_len = sizeof(req.client_addr);

        ssize_t n = recvfrom(sockfd, req.data, sizeof(req.data), 0,
                            (struct sockaddr*)&req.client_addr, &req.client_len);
        if (n > 0) {
            req.len = n;
            enqueue_request(&req);

            log_msg(LOG_DEBUG, "Received DNS query from %s:%d (%zd bytes)",
                inet_ntoa(req.client_addr.sin_addr),  // 客户端IP字符串
                ntohs(req.client_addr.sin_port),      // 客户端端口（网络字节序转主机序）
                n);                                   // 接收的字节数
            continue;
        }

        /* n <= 0：超时或出错。
         * 对 UDP 监听器而言，SO_RCVTIMEO 触发的超时属于正常情况，必须继续等待。
         * POSIX 下超时表现为 EAGAIN/EWOULDBLOCK/EINTR；Windows 下 recvfrom 超时
         * 表现为 WSAETIMEDOUT（MinGW 有时不会改写 errno，残留之前 fopen 的 ENOENT），
         * 因此不能把“读不到数据”视为致命错误——除非 stop 已被置位。 */
        if (n == 0) continue;
#ifdef _WIN32
        if (WSAGetLastError() == WSAETIMEDOUT) continue;
        log_msg(LOG_DEBUG, "recvfrom returned -1 (errno=%d, WSA=%d), continuing",
                errno, WSAGetLastError());
        continue;
#else
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        log_msg(LOG_ERROR, "Failed to receive data: %s", strerror(errno));
        break;
#endif
    }

    log_msg(LOG_INFO, "Shutting down gracefully");
    log_cleanup();
    close(sockfd);
    return 0;
}