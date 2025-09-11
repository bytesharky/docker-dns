#ifndef GATEWAY_H
#define GATEWAY_H
#include <netinet/in.h>   // for in_addr
// #include <ldns/packet.h>  // for ldns_pkt
// #include <ldns/rr.h>      // for ldns_rr
#include <ldns/ldns.h>

extern struct in_addr gateway_addr; 

int is_gateway_domain(const char *name);
int resolve_gateway_ip(void);
ldns_pkt* handle_gateway_query(ldns_pkt *query_pkt, ldns_rr *qrr, 
    struct in_addr client_addr);

#endif
