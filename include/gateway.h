#ifndef GATEWAY_H
#define GATEWAY_H
#include <ldns/ldns.h>
#include <netinet/in.h>

extern struct in_addr gateway_addr; 

int is_gateway_domain(const char *name);
int resolve_gateway_ip();
ldns_pkt* handle_gateway_query(ldns_pkt *query_pkt, ldns_rr *qrr, 
    struct in_addr client_addr);

#endif
