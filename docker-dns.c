#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <ldns/ldns.h>
#include <time.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netdb.h>
#include "logging.h"

#define LISTEN_PORT 53
#define FORWARD_DNS "127.0.0.11"
#define BUF_SIZE 4096
#define LOG_LEVEL "LOG_LEVEL"

volatile sig_atomic_t stop = 0;
char container_name[256] = {0}; // 存储CONTAINER_NAME环境变量值
char gateway_name[256] = {0};  // 存储GATEWAY环境变量值
struct in_addr gateway_addr;   // 存储网关IP地址

// 处理终止信号
void handle_sigterm(int sig) {
    log_msg(LOG_DEBUG, "Received signal %d", sig);
    stop = 1;
}

// 检查是否是.docker域名
int is_docker_domain(const char *name) {
    if (!name) return 0;
    size_t len = strlen(name);
    if (len > 0 && name[len-1] == '.') len--;
    int result = (len >= 7 && strncasecmp(name + len - 7, ".docker", 7) == 0);
    log_msg(LOG_DEBUG, "Checking if '%s' is docker domain: %s",
        name, result ? "YES" : "NO");
    return result;
}

// 移除.docker后缀
void strip_docker_suffix(char *name) {
    if (!name) return;
    size_t len = strlen(name);

    // 先移除尾部的点（FQDN）
    if (len > 0 && name[len-1] == '.') {
        name[len-1] = '\0';
        len--;
    }
    
    // 然后移除.docker后缀
    if (len >= 7) {
        name[len - 7] = '\0';
        log_msg(LOG_DEBUG, "Stripped .docker suffix, new name: '%s'", name);
    }
}

// 检查是否是网关域名
int is_gateway_domain(const char *name) {
    if (!name || !gateway_name[0]) return 0;
    
    size_t len = strlen(name);
    char *temp_name = strdup(name);
    if (!temp_name) return 0;
    
    // 先移除尾部的点（FQDN）
    if (len > 0 && temp_name[len-1] == '.') {
        temp_name[len-1] = '\0';
        len--;
    }

    // 构建预期的网关域名格式：gateway.docker
    char expected_gateway[512];
    snprintf(expected_gateway, sizeof(expected_gateway), "%s.docker", gateway_name);
    
    int result = (strcasecmp(temp_name, expected_gateway) == 0);
    log_msg(LOG_DEBUG, "Checking if '%s' matches gateway domain '%s': %s", 
              name, expected_gateway, result ? "YES" : "NO");
    
    free(temp_name);
    return result;
}

// 获取网关IP地址
int resolve_gateway_ip() {
    log_msg(LOG_DEBUG, "Resolving gateway IP from /proc/net/route");
    
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) {
        perror("fopen /proc/net/route");
        return 1;
    }

    char iface[64];
    unsigned long dest, gateway, mask;
    unsigned int flags;
    int refcnt, use, metric, mtu, window, irtt;
    char line[256];

    // 跳过表头
    if (!fgets(line, sizeof(line), f)) {
        log_msg(LOG_DEBUG, "Failed to read header from /proc/net/route");
        fclose(f);
        return 1;
    }

    // 读取每一行
    while (fscanf(f, "%63s %lx %lx %X %d %d %d %lx %d %d %d\n",
                  iface, &dest, &gateway, &flags,
                  &refcnt, &use, &metric, &mask, &mtu, &window, &irtt) == 11) {
        
        // 默认路由
        if (dest == 0) {
            if (gateway == 0) {
                log_msg(LOG_DEBUG, "Found default route with gateway 0, skipping");
                continue;
            }
            
            struct in_addr gw;
            gw.s_addr = gateway;
            gateway_addr.s_addr = gateway;
            log_msg(LOG_DEBUG, "Found default gateway via interface %s: %s",
                     iface, inet_ntoa(gw));
            fclose(f);
            return 0;
        }
    }

    log_msg(LOG_WARN, "No valid default gateway found");
    fclose(f);
    return 1;
}

// 创建网关域名的DNS响应
ldns_pkt* create_gateway_response(ldns_pkt *query_pkt, ldns_rr *qrr, struct in_addr client_addr) {
    if (!query_pkt || !qrr) {
        log_msg(LOG_ERROR, "Invalid parameters for create_gateway_response");
        return NULL;
    }

    log_msg(LOG_DEBUG, "Creating gateway response for query type %s",
            ldns_rr_type2str(ldns_rr_get_type(qrr)));
    
    char *qname_str = ldns_rdf2str(ldns_rr_owner(qrr));
    if (!qname_str) {
        log_msg(LOG_ERROR, "Failed to get query name string");
        return NULL;
    }
    qname_str[strcspn(qname_str, "\n")] = 0;

    ldns_pkt *resp_pkt = ldns_pkt_new();
    if (!resp_pkt) {
        log_msg(LOG_ERROR, "Failed to create response packet");
        return NULL;
    }
    
    ldns_pkt_set_id(resp_pkt, ldns_pkt_id(query_pkt));
    ldns_pkt_set_qr(resp_pkt, 1);
    ldns_pkt_set_aa(resp_pkt, 1);
    ldns_pkt_set_rd(resp_pkt, ldns_pkt_rd(query_pkt));  // 保持原始RD标志
    ldns_pkt_set_ra(resp_pkt, 1);                       // 递归可用
    ldns_pkt_set_rcode(resp_pkt, LDNS_RCODE_NOERROR);
    ldns_pkt_push_rr(resp_pkt, LDNS_SECTION_QUESTION, ldns_rr_clone(qrr));

    // 只有A记录查询才添加答案
    if (ldns_rr_get_type(qrr) == LDNS_RR_TYPE_A) {
        // 确保网关地址有效
        if (gateway_addr.s_addr == 0) {
            log_msg(LOG_WARN, "Gateway address is 0, resolving again");
            if (resolve_gateway_ip() != 0) {
                log_msg(LOG_ERROR, "Failed to resolve gateway IP, returning SERVFAIL");
                ldns_pkt_set_rcode(resp_pkt, LDNS_RCODE_SERVFAIL);
                free(qname_str);
                return resp_pkt;
            }
        }
        
        // 使用 ldns_rr_new_frm_str 创建A记录
        char rr_str[512];
        snprintf(rr_str, sizeof(rr_str), "%s 60 IN A %s", 
                qname_str, inet_ntoa(gateway_addr));

        log_msg(LOG_DEBUG, "Creating A record: %s", rr_str);

        ldns_rr *answer_rr = NULL;
        ldns_status status = ldns_rr_new_frm_str(&answer_rr, rr_str, 0, NULL, NULL);

        if (status == LDNS_STATUS_OK && answer_rr) {
            // 添加到答案段
            ldns_pkt_push_rr(resp_pkt, LDNS_SECTION_ANSWER, answer_rr);
            char* modified_name = strdup(qname_str);
            strip_docker_suffix(modified_name);
            log_msg(LOG_DEBUG, "Successfully created gateway A record response");
            log_msg(LOG_INFO, "Gateway A query '%s' from %s -> %s is gateway", 
                modified_name, 
                inet_ntoa(client_addr), 
                inet_ntoa(gateway_addr));
        } else {
            log_msg(LOG_ERROR, "Failed to create A record from string: %s",
                     ldns_get_errorstr_by_id(status));
            ldns_pkt_set_rcode(resp_pkt, LDNS_RCODE_SERVFAIL);
        }
    } else {
        log_msg(LOG_DEBUG, "Unsupported query type for gateway: %s",
                 ldns_rr_type2str(ldns_rr_get_type(qrr)));
    }

    free(qname_str);
    return resp_pkt;
}

// 创建一个新的resolver用于单次查询，避免状态污染
ldns_resolver* create_fresh_resolver() {
    ldns_resolver *fresh_resolver = ldns_resolver_new();
    if (!fresh_resolver) {
        log_msg(LOG_ERROR, "Failed to create fresh resolver");
        return NULL;
    }

    ldns_rdf *ns_rdf = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_A, FORWARD_DNS);
    if (!ns_rdf) {
        log_msg(LOG_ERROR, "Failed to create nameserver RDF for fresh resolver");
        ldns_resolver_deep_free(fresh_resolver);
        return NULL;
    }
    
    ldns_resolver_push_nameserver(fresh_resolver, ns_rdf);
    ldns_rdf_deep_free(ns_rdf);

    struct timeval tv = {2, 0};
    ldns_resolver_set_timeout(fresh_resolver, tv);
    ldns_resolver_set_retry(fresh_resolver, 1);
    
    return fresh_resolver;
}

// 测试与转发DNS服务器的连接
int test_forward_dns() {
    log_msg(LOG_DEBUG, "Testing connection to forward DNS server %s", FORWARD_DNS);
    
    ldns_resolver *test_resolver = ldns_resolver_new();
    if (!test_resolver) {
        log_msg(LOG_ERROR, "Failed to create test resolver");
        return 0;
    }

    ldns_rdf *ns_rdf = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_A, FORWARD_DNS);
    if (!ns_rdf) {
        log_msg(LOG_ERROR, "Failed to create nameserver RDF for %s", FORWARD_DNS);
        ldns_resolver_deep_free(test_resolver);
        return 0;
    }
    
    ldns_resolver_push_nameserver(test_resolver, ns_rdf);
    ldns_rdf_deep_free(ns_rdf);

    struct timeval tv = {2, 0};
    ldns_resolver_set_timeout(test_resolver, tv);
    ldns_resolver_set_retry(test_resolver, 1);

    ldns_rdf *test_name = NULL;
    ldns_str2rdf_dname(&test_name, container_name);
    
    if (test_name) {
        ldns_pkt *test_resp = ldns_resolver_query(test_resolver, test_name, 
                                                LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN, LDNS_RD);
        ldns_rdf_deep_free(test_name);
        
        if (test_resp) {
            log_msg(LOG_DEBUG, "Forward DNS server %s is reachable", FORWARD_DNS);
            ldns_pkt_free(test_resp);
            ldns_resolver_deep_free(test_resolver);
            return 1;
        } else {
            log_msg(LOG_DEBUG, "Forward DNS server %s is not responding", FORWARD_DNS);
        }
    }
    
    ldns_resolver_deep_free(test_resolver);
    return 0;
}

// 处理单个DNS查询
void process_query(int sockfd, const char *buf, ssize_t len,struct sockaddr_in *client, socklen_t client_len) {

    log_msg(LOG_DEBUG, "Processing DNS query from %s:%d (%zd bytes)", 
                inet_ntoa(client->sin_addr), ntohs(client->sin_port), len);

    ldns_pkt *query_pkt = NULL;
    if (ldns_wire2pkt(&query_pkt, buf, len) != LDNS_STATUS_OK || !query_pkt) {
        log_msg(LOG_ERROR, "Failed to parse DNS query packet");
        return;
    }

    ldns_rr_list *question = ldns_pkt_question(query_pkt);
    if (!question || ldns_rr_list_rr_count(question) == 0) { 
        log_msg(LOG_ERROR, "Failed to find questions in DNS query");
        ldns_pkt_free(query_pkt); 
        return; 
    }

    ldns_rr *qrr = ldns_rr_list_rr(question, 0);
    if (!qrr) {
        log_msg(LOG_ERROR, "Failed to get question RR");
        ldns_pkt_free(query_pkt);
        return;
    }

    ldns_rdf *qname = ldns_rr_owner(qrr);
    if (!qname) {
        log_msg(LOG_ERROR, "Failed to get question name");
        ldns_pkt_free(query_pkt);
        return;
    }

    char *qname_str = ldns_rdf2str(qname);
    if (!qname_str) {
        log_msg(LOG_ERROR, "Failed to convert question name to string");
        ldns_pkt_free(query_pkt);
        return;
    }

    // 移除换行符
    qname_str[strcspn(qname_str, "\n")] = 0;
    log_msg(LOG_DEBUG, "Query for: '%s', Type: %s, ID: %d", qname_str,
                ldns_rr_type2str(ldns_rr_get_type(qrr)), ldns_pkt_id(query_pkt));

    ldns_pkt *resp_pkt = NULL;

    // 首先检查是否是docker域名
    if (!is_docker_domain(qname_str)) {
        log_msg(LOG_DEBUG, "Not a .docker domain, returning REFUSED");
    } 
    else {
        // 然后检查是否是网关域名
        if (gateway_name[0] && is_gateway_domain(qname_str)) {
            log_msg(LOG_DEBUG, "Handling gateway domain: %s", qname_str);
            resp_pkt = create_gateway_response(query_pkt, qrr, client->sin_addr);
        }
        // 其他docker域名
        else {
            char *modified_name = strdup(qname_str);
            if (modified_name) {
                strip_docker_suffix(modified_name);
                log_msg(LOG_INFO, "Forwarding %s query for '%s' from %s to %s",
                        ldns_rr_type2str(ldns_rr_get_type(qrr)),
                        modified_name,
                        inet_ntoa(client->sin_addr),
                        FORWARD_DNS);

                ldns_rdf *rdf_name = NULL;
                if (ldns_str2rdf_dname(&rdf_name, modified_name) == LDNS_STATUS_OK && rdf_name) {
                    // 为每个查询创建新的resolver，避免状态污染
                    ldns_resolver *fresh_resolver = create_fresh_resolver();
                    if (!fresh_resolver) {
                        log_msg(LOG_ERROR, "Failed to create fresh resolver for query");
                        ldns_rdf_deep_free(rdf_name);
                        free(modified_name);
                        free(qname_str);
                        ldns_pkt_free(query_pkt);
                        return;
                    }
                    
                    ldns_pkt *forward_resp = ldns_resolver_query(fresh_resolver, rdf_name, 
                                                ldns_rr_get_type(qrr), LDNS_RR_CLASS_IN, LDNS_RD);
                    
                    if (forward_resp) {
                        uint8_t rcode = ldns_pkt_get_rcode(forward_resp);
                        const char* rcode_str = "UNKNOWN";
                        switch(rcode) {
                            case LDNS_RCODE_NOERROR: rcode_str = "NOERROR"; break;
                            case LDNS_RCODE_FORMERR: rcode_str = "FORMERR"; break;
                            case LDNS_RCODE_SERVFAIL: rcode_str = "SERVFAIL"; break;
                            case LDNS_RCODE_NXDOMAIN: rcode_str = "NXDOMAIN"; break;
                            case LDNS_RCODE_NOTIMPL: rcode_str = "NOTIMPL"; break;
                            case LDNS_RCODE_REFUSED: rcode_str = "REFUSED"; break;
                        }
                        log_msg(LOG_DEBUG, "Forward DNS response: %s (%d answers)", 
                                rcode_str,
                                ldns_rr_list_rr_count(ldns_pkt_answer(forward_resp)));
                        
                        // 创建新的响应包，保持原始Question Section
                        resp_pkt = ldns_pkt_new();
                        if (resp_pkt) {
                            // 复制基本属性
                            ldns_pkt_set_id(resp_pkt, ldns_pkt_id(query_pkt));
                            ldns_pkt_set_qr(resp_pkt, 1);
                            ldns_pkt_set_aa(resp_pkt, ldns_pkt_aa(forward_resp));
                            ldns_pkt_set_tc(resp_pkt, ldns_pkt_tc(forward_resp));
                            ldns_pkt_set_rd(resp_pkt, ldns_pkt_rd(forward_resp));
                            ldns_pkt_set_ra(resp_pkt, ldns_pkt_ra(forward_resp));
                            ldns_pkt_set_rcode(resp_pkt, ldns_pkt_get_rcode(forward_resp));
                            
                            // 使用原始查询的Question Section
                            ldns_pkt_push_rr(resp_pkt, LDNS_SECTION_QUESTION, ldns_rr_clone(qrr));
                            
                            // 复制Answer Section中的记录，但需要修改域名
                            ldns_rr_list *answers = ldns_pkt_answer(forward_resp);
                            if (answers) {
                                for (size_t i = 0; i < ldns_rr_list_rr_count(answers); i++) {
                                    ldns_rr *answer_rr = ldns_rr_clone(ldns_rr_list_rr(answers, i));
                                    if (answer_rr) {
                                        // 将答案记录的域名改回原始域名
                                        ldns_rr_set_owner(answer_rr, ldns_rdf_clone(qname));
                                        ldns_pkt_push_rr(resp_pkt, LDNS_SECTION_ANSWER, answer_rr);
                                    }
                                }
                            }
                            
                            // 复制Authority Section
                            ldns_rr_list *authority = ldns_pkt_authority(forward_resp);
                            if (authority) {
                                for (size_t i = 0; i < ldns_rr_list_rr_count(authority); i++) {
                                    ldns_pkt_push_rr(resp_pkt, LDNS_SECTION_AUTHORITY, 
                                                    ldns_rr_clone(ldns_rr_list_rr(authority, i)));
                                }
                            }
                            
                            // 复制Additional Section
                            ldns_rr_list *additional = ldns_pkt_additional(forward_resp);
                            if (additional) {
                                for (size_t i = 0; i < ldns_rr_list_rr_count(additional); i++) {
                                    ldns_pkt_push_rr(resp_pkt, LDNS_SECTION_ADDITIONAL, 
                                                    ldns_rr_clone(ldns_rr_list_rr(additional, i)));
                                }
                            }
                            
                            log_msg(LOG_DEBUG, "Created response with original question section");
                        }
                        
                        ldns_pkt_free(forward_resp);
                    } else {
                        log_msg(LOG_DEBUG, "No response from forward DNS server for '%s' (this is expected for non-existent containers)", modified_name);
                    }
                    
                    // 释放为此查询创建的resolver
                    ldns_resolver_deep_free(fresh_resolver);
                    ldns_rdf_deep_free(rdf_name);
                } else {
                    log_msg(LOG_ERROR, "Failed to create RDF name for '%s'", modified_name);
                }
                free(modified_name);
            }
        }
    }
    
    if (!resp_pkt) {
        log_msg(LOG_DEBUG, "Creating REFUSED response");
        resp_pkt = ldns_pkt_new();
        if (resp_pkt) {
            ldns_pkt_set_id(resp_pkt, ldns_pkt_id(query_pkt));
            ldns_pkt_set_qr(resp_pkt, 1);
            ldns_pkt_set_aa(resp_pkt, 1);
            ldns_pkt_set_rcode(resp_pkt, LDNS_RCODE_REFUSED);
            ldns_pkt_push_rr(resp_pkt, LDNS_SECTION_QUESTION, ldns_rr_clone(qrr));
        }
    }

    if (resp_pkt) {
        uint8_t *out = NULL;
        size_t outlen = 0;
        
        if (ldns_pkt2wire(&out, resp_pkt, &outlen) == LDNS_STATUS_OK && out) {
            ssize_t sent = sendto(sockfd, out, outlen, 0, 
                                (struct sockaddr*)client, client_len);
            if (sent == -1) {
                log_msg(LOG_ERROR, "Failed to send response: %s", strerror(errno));
            } else if (sent != outlen) {
                log_msg(LOG_WARN, "Partial send: %zd of %zu bytes", sent, outlen);
            } else {
                log_msg(LOG_DEBUG, "Sent response (%zd bytes)", sent);
            }
            free(out);
        } else {
            log_msg(LOG_DEBUG, "Failed to serialize response packet");
        }
        
        ldns_pkt_free(resp_pkt);
    }
    log_msg(LOG_DEBUG, "Finished processing query for '%s'", qname_str);
    free(qname_str);
    ldns_pkt_free(query_pkt);
}

// 主程序入口
int main() {
    signal(SIGTERM, handle_sigterm);
    signal(SIGINT, handle_sigterm);

    log_level = get_log_level(LOG_LEVEL, LOG_INFO);
    log_msg(LOG_INFO, "Starting Docker DNS forwarder (log level=%d->%s)", log_level, level_str[log_level]);

    // 读取GATEWAY环境变量
    const char *gateway_env = getenv("GATEWAY");
    if (gateway_env) {
        strncpy(gateway_name, gateway_env, sizeof(gateway_name) - 1);
        log_msg(LOG_DEBUG, "Gateway name from environment: '%s'", gateway_name);
    } else {
        strncpy(gateway_name, "gateway", sizeof(gateway_name) - 1);
        gateway_name[sizeof(gateway_name) - 1] = '\0';
        log_msg(LOG_DEBUG, "No GATEWAY environment variable set, using default: '%s'",
             gateway_name);
    }
    // 读取CONTAINER_NAME环境变量
    const char *container_env = getenv("CONTAINER_NAME");
    if (container_env) {
        strncpy(container_name, container_env, sizeof(container_name) - 1);
        log_msg(LOG_DEBUG, "Container name from environment: '%s'", container_name);
    } else {
        strncpy(container_name, "docker-dns", sizeof(container_name) - 1);
        container_name[sizeof(container_name) - 1] = '\0';
        log_msg(LOG_DEBUG, "No CONTAINER_NAME environment variable set, using default: '%s'",
            container_name);
    }

    if (!test_forward_dns()) {
        log_msg(LOG_WARN, "Forward DNS server may not be available");
    }

    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    uint8_t buf[BUF_SIZE];

    // 初始化网关IP地址
    gateway_addr.s_addr = 0;
    if (resolve_gateway_ip() != 0) {
        log_msg(LOG_WARN, "Failed to resolve gateway IP at startup");
    } else {
        log_msg(LOG_INFO, "Gateway IP resolved to: %s", inet_ntoa(gateway_addr));
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
    server_addr.sin_port = htons(LISTEN_PORT);

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_msg(LOG_FATAL, "Failed to bind socket to port %d", LISTEN_PORT);
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

    if (gateway_name[0]) {
        log_msg(LOG_DEBUG, "Special handling for %s.docker -> %s", 
            gateway_name, inet_ntoa(gateway_addr));
    }

    log_msg(LOG_INFO, "DNS forwarder listening on port %d, forwarding *.docker to %s",
            LISTEN_PORT, FORWARD_DNS);

    while (!stop) {

        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, 
                            (struct sockaddr*)&client_addr, &client_len);
        if (n < 0) {
            if (errno == EINTR ||
                errno == EAGAIN || 
                errno == EWOULDBLOCK) continue;
            log_msg(LOG_ERROR, "Failed to receive data: %s", strerror(errno));
            break;
        }

        process_query(sockfd, buf, n, &client_addr, client_len);
    }

    log_msg(LOG_INFO, "Shutting down gracefully");
    close(sockfd);
    return 0;
}
