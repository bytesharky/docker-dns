#ifndef DNS_H
#define DNS_H
#include <stdint.h>         // for uint8_t
#include <sys/socket.h>     // for socklen_t, ssize_t
// #include <ldns/packet.h>    // for ldns_pkt
// #include <ldns/rdata.h>     // for ldns_rdf
// #include <ldns/resolver.h>  // for ldns_resolver
#include <ldns/ldns.h>

struct sockaddr;

#define QUEUE_SIZE 1024

// 把通用 socket 地址格式化为可读字符串（IPv4/IPv6 通用），结果写入 buf 并返回 buf。
// 失败时返回安全占位串，绝不返回 NULL。buf 长度建议 >= INET6_ADDRSTRLEN。
const char *addr_to_str(const struct sockaddr *sa, char *buf, size_t buflen);

int test_forward_dns(void);
int is_match_suffix(const char *name);
void strip_dot(char *name);
void strip_suffix(char *name);
ldns_resolver* create_fresh_resolver(void);
ldns_pkt* modify_query_domain(ldns_pkt *original_pkt,  ldns_rdf *new_domain);
// 解析单个 DNS 查询并返回响应报文（调用方负责 ldns_pkt_free）。
// client 可为 NULL（DoH 路径没有 UDP 客户端地址时使用，此时日志以 "(doh)" 代替）。
ldns_pkt* resolve_dns_query(const uint8_t *buf, size_t len, const struct sockaddr *client);
void process_dns_query(int sockfd, const uint8_t *buf, ssize_t len,
                        struct sockaddr *client, socklen_t client_len);
#endif
