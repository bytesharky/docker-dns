
#include "config.h"         // for gateway_name, keep_suffix, suffix_domain
#include "dns.h"            // for strip_suffix
#include "gateway.h"
#include "logging.h"        // for log_msg, LOG_DEBUG, LOG_ERROR, LOG_WARN
#include <arpa/inet.h>      // for inet_ntoa
#include <stdio.h>          // for fclose, NULL, snprintf, fgets, fopen, fscanf
#include <stdlib.h>         // for free
#include <string.h>         // for strdup, strcspn, strlen
#include <strings.h>        // for strcasecmp, size_t
// #include <ldns/error.h>     // for ldns_get_errorstr_by_id, ldns_enum_status
// #include <ldns/host2str.h>  // for ldns_rr_type2str, ldns_rdf2str
#include <ldns/ldns.h>

// 检查是否是网关域名
int is_gateway_domain(const char *name) {
    if (!name || !gateway_name[0]) return 0;
    
    size_t len = strlen(name);
    char *temp_name = strdup(name);
    if (!temp_name) return 0;
    
    // 先移除尾部的点（FQDN）
    strip_dot(temp_name);

    // 构建预期的网关域名格式
    char expected_gateway[512];
    snprintf(expected_gateway, sizeof(expected_gateway), "%s%s", gateway_name, suffix_domain);
    
    int result = (strcasecmp(temp_name, expected_gateway) == 0);
    log_msg(LOG_DEBUG, "Checking if '%s' matches gateway domain '%s': %s", 
              name, expected_gateway, result ? "YES" : "NO");
    
    free(temp_name);
    return result;
}

// 获取网关IP地址
int resolve_gateway_ip(void) {
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
ldns_pkt* handle_gateway_query(ldns_pkt *query_pkt, ldns_rr *qrr, struct in_addr client_addr) {
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
            if (!keep_suffix) strip_suffix(modified_name);
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
