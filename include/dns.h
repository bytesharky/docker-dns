#ifndef DNS_H
#define DNS_H
#include <stdint.h>         // for uint8_t
#include <sys/socket.h>     // for socklen_t, ssize_t
// #include <ldns/packet.h>    // for ldns_pkt
// #include <ldns/rdata.h>     // for ldns_rdf
// #include <ldns/resolver.h>  // for ldns_resolver
#include <ldns/ldns.h>

struct sockaddr_in;

#define QUEUE_SIZE 1024

int test_forward_dns(void);
int is_match_suffix(const char *name);
void strip_dot(char *name);
void strip_suffix(char *name);
ldns_resolver* create_fresh_resolver(void);
ldns_pkt* modify_query_domain(ldns_pkt *original_pkt,  ldns_rdf *new_domain);
// 解析单个 DNS 查询并返回响应报文（调用方负责 ldns_pkt_free）。
// client 可为 NULL（DoH 路径没有 UDP 客户端地址时使用，此时日志以 "(doh)" 代替）。
ldns_pkt* resolve_dns_query(const uint8_t *buf, size_t len, const struct sockaddr_in *client);
void process_dns_query(int sockfd, const uint8_t *buf, ssize_t len,
                        struct sockaddr_in *client, socklen_t client_len);
#endif
