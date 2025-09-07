#ifndef LOOP_MARKER_H
#define LOOP_MARKER_H
#include <ldns/ldns.h>
#include <ldns/edns.h>
#include <arpa/inet.h>

#define MY_OPTION_CODE 65001
#define HOP_COUNT_DATA_LEN 2

void add_loop_marker(ldns_pkt *pkt, uint16_t hop_count);
uint16_t get_loop_marker(ldns_pkt *pkt);
#endif
