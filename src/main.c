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
#ifdef _WIN32
#include <winsock2.h>     // Windows: select / fd_set / FD_* 由 winsock2.h 提供
#else
#include <sys/select.h>  // POSIX: select / fd_set 由 sys/select.h 提供
#endif
#include <sys/time.h>    // for timeval
#include <unistd.h>      // for close, NULL

struct in_addr gateway_addr;                // 存储网关IP地址

// 创建并绑定一个 UDP 监听 socket。成功返回 fd（>=0），失败返回 -1（由调用方决定降级策略）。
// IPv6 监听器强制 IPV6_V6ONLY=1，只接收原生 IPv6 报文，避免与 IPv4 监听器争用同一端口。
static int create_udp_listener(int family, int port, const char *fam_name) {
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_msg(LOG_WARN, "Failed to create %s socket: %s", fam_name, strerror(errno));
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        log_msg(LOG_WARN, "Failed to set SO_REUSEADDR on %s socket: %s", fam_name, strerror(errno));
        close(fd);
        return -1;
    }

#ifdef IPV6_V6ONLY
    if (family == AF_INET6) {
        int only = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(only));
    }
#endif

    int ok = 0;
    if (family == AF_INET) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons((uint16_t)port);
        ok = (bind(fd, (struct sockaddr *)&a, sizeof a) == 0);
    } else {
        struct sockaddr_in6 a;
        memset(&a, 0, sizeof a);
        a.sin6_family = AF_INET6;
        a.sin6_port = htons((uint16_t)port);   // sin6_addr 全 0 等价于 in6addr_any
        ok = (bind(fd, (struct sockaddr *)&a, sizeof a) == 0);
    }

    if (!ok) {
        log_msg(LOG_WARN, "Failed to bind %s socket to port %d: %s", fam_name, port, strerror(errno));
        close(fd);
        return -1;
    }

    struct timeval tv = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    log_msg(LOG_INFO, "%s DNS listener bound to port %d", fam_name, port);
    return fd;
}

// 任务线程
void* worker_thread(void *arg) {
    (void)arg;
    while (1) {
        dns_request_t req;
        dequeue_request(&req);

        // 用收到请求的同一 socket 回包（IPv4 回给 v4 socket，IPv6 回给 v6 socket）
        process_dns_query(req.sockfd, req.data, req.len,
                          (struct sockaddr *)&req.client_addr, req.client_len);
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

    // 双栈监听：IPv4 与 IPv6 各自独立绑定，互不干扰。IPv6 不可用时自动降级为仅 IPv4。
    int dns_socks[2];
    int dns_nsocks = 0;
    int v4 = create_udp_listener(AF_INET,  listen_port, "IPv4");
    if (v4 >= 0) dns_socks[dns_nsocks++] = v4;
    int v6 = create_udp_listener(AF_INET6, listen_port, "IPv6");
    if (v6 >= 0) dns_socks[dns_nsocks++] = v6;

    if (dns_nsocks == 0) {
        log_msg(LOG_FATAL, "Failed to create any DNS listener socket on port %d", listen_port);
        return 1;
    }
    if (dns_nsocks == 1) {
        log_msg(LOG_WARN, "Only %s listener active (dual-stack unavailable on this host)",
                dns_socks[0] == v4 ? "IPv4" : "IPv6");
    }

    log_msg(LOG_INFO, "DNS forwarder listening on port %d, forwarding *%s to %s (suffix: %s)",
            listen_port, suffix_domain, forward_dns, keep_suffix ? "keep" : "strip");

    log_msg(LOG_INFO, "Create DNS pthread (workers: %d, hops: %d)", num_workers, max_hops);
    for (int i = 0; i < num_workers; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, worker_thread, NULL);
        pthread_detach(tid);
    }

    /* 启动内置 DoH / HTTP-DNS 服务器线程（复用同一份解析逻辑） */
    pthread_t doh_tid;
    if (pthread_create(&doh_tid, NULL, doh_server_thread, NULL) == 0) {
        pthread_detach(doh_tid);
    } else {
        log_msg(LOG_WARN, "Failed to start DoH/HTTP server thread");
    }

    char clip[INET6_ADDRSTRLEN];
    while (!stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < dns_nsocks; i++) {
            FD_SET(dns_socks[i], &rfds);
            if (dns_socks[i] > maxfd) maxfd = dns_socks[i];
        }

        struct timeval tv = {1, 0};
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) {
            /* 超时或信号中断：继续循环以重新检查 stop 标志，绝不视为致命错误。 */
#ifdef _WIN32
            if (r < 0 && WSAGetLastError() != WSAEINTR) {
                log_msg(LOG_DEBUG, "select returned -1 (WSA=%d), continuing", WSAGetLastError());
            }
#else
            if (r < 0 && errno != EINTR) {
                log_msg(LOG_DEBUG, "select returned -1 (errno=%d), continuing", errno);
            }
#endif
            continue;
        }

        /* 依次检查每个就绪的监听 socket（IPv4 / IPv6），收到的请求连同来源 socket 一并入队。 */
        for (int i = 0; i < dns_nsocks; i++) {
            if (!FD_ISSET(dns_socks[i], &rfds)) continue;
            dns_request_t req;
            req.client_len = sizeof(req.client_addr);
            ssize_t n = recvfrom(dns_socks[i], req.data, sizeof(req.data), 0,
                                 (struct sockaddr *)&req.client_addr, &req.client_len);
            if (n > 0) {
                req.len = n;
                req.sockfd = dns_socks[i];
                enqueue_request(&req);
                log_msg(LOG_DEBUG, "Received DNS query from %s (%zd bytes)",
                        addr_to_str((struct sockaddr *)&req.client_addr, clip, sizeof clip), n);
            }
            /* 单个 socket 出错只跳过本 socket，不中断整个服务 */
        }
    }

    log_msg(LOG_INFO, "Shutting down gracefully");
    log_cleanup();
    for (int i = 0; i < dns_nsocks; i++) close(dns_socks[i]);
    return 0;
}