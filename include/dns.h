#ifndef DNS_H
#define DNS_H
#include <ldns/ldns.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define QUEUE_SIZE 1024

int test_forward_dns();
int is_match_suffix(const char *name);
void strip_suffix(char *name);
ldns_resolver* create_fresh_resolver();
ldns_pkt* modify_query_domain(ldns_pkt *original_pkt,  ldns_rdf *new_domain);
void process_dns_query(int sockfd, const char *buf, ssize_t len,
                        struct sockaddr_in *client, socklen_t client_len);
#endif
