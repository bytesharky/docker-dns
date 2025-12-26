#include "config.h"          // for forward_dns, suffix_domain, container_name
#include "dns.h"
#include "gateway.h"         // for handle_gateway_query, is_gateway_domain
#include "logging.h"         // for log_msg, LOG_DEBUG, LOG_ERROR, LOG_WARN
#include "loop_marker.h"     // for add_loop_marker, get_loop_marker
#include <arpa/inet.h>       // for inet_ntoa, ntohs
#include <errno.h>           // for errno
#include <netinet/in.h>      // for sockaddr_in
#include <stdint.h>          // for uint8_t, uint16_t
#include <stdio.h>           // for NULL
#include <stdlib.h>          // for free
#include <string.h>          // for strlen, strcspn, strdup, strerror
#include <strings.h>         // for strncasecmp
#include <sys/time.h>        // for timeval
// #include <ldns/error.h>      // for ldns_enum_status, ldns_status
// #include <ldns/host2str.h>   // for ldns_rr_type2str, ldns_rdf2str
// #include <ldns/host2wire.h>  // for ldns_pkt2wire
// #include <ldns/rr.h>         // for ldns_rr_clone, ldns_rr_list_rr, ldns_rr_...
// #include <ldns/str2host.h>   // for ldns_str2rdf_dname
// #include <ldns/wire2host.h>  // for ldns_wire2pkt
#include <ldns/ldns.h>

// 测试与转发DNS服务器的联通性
int test_forward_dns(void) {
    log_msg(LOG_DEBUG, "Testing connection to forward DNS server %s", forward_dns);
    
    ldns_resolver *test_resolver = ldns_resolver_new();
    if (!test_resolver) {
        log_msg(LOG_ERROR, "Failed to create test resolver");
        return 0;
    }

    ldns_rdf *ns_rdf = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_A, forward_dns);
    if (!ns_rdf) {
        log_msg(LOG_ERROR, "Failed to create nameserver RDF for %s", forward_dns);
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
            log_msg(LOG_DEBUG, "Forward DNS server %s is reachable", forward_dns);
            ldns_pkt_free(test_resp);
            ldns_resolver_deep_free(test_resolver);
            return 1;
        } else {
            log_msg(LOG_DEBUG, "Forward DNS server %s is not responding", forward_dns);
        }
    }
    
    ldns_resolver_deep_free(test_resolver);
    return 0;
}

// 检查是否是匹配后缀
int is_match_suffix(const char *name) {
    if (!name) return 0;
    size_t len = strlen(name);
    size_t suffix_len = strlen(suffix_domain);
    if (len > 0 && name[len-1] == '.') len--;
    int result = (len >= suffix_len && strncasecmp(name + len - suffix_len, suffix_domain, suffix_len) == 0);
    log_msg(LOG_DEBUG, "Checking if '%s' is %s domain: %s",
        name, suffix_domain, result ? "YES" : "NO");
    return result;
}

// 移除尾部的点（FQDN）
void strip_dot(char *name) {
    if (!name) return;
    size_t len = strlen(name);
    
    if (len > 0 && name[len-1] == '.') {
        name[len-1] = '\0';
        len--;
    }
}

// 移除域名后缀
void strip_suffix(char *name) {
    if (!name) return;
    size_t len = strlen(name);

    // 先移除尾部的点（FQDN）
    strip_dot(name);
    
    // 然后移除后缀
    size_t suffix_len = strlen(suffix_domain);
    if (len >= suffix_len) {
        name[len - suffix_len] = '\0';
        log_msg(LOG_DEBUG, "Stripped %s suffix, new name: '%s'", suffix_domain, name);
    }
}

// 创建一个新的resolver
ldns_resolver* create_fresh_resolver(void) {
    ldns_resolver *fresh_resolver = ldns_resolver_new();
    if (!fresh_resolver) {
        log_msg(LOG_ERROR, "Failed to create fresh resolver");
        return NULL;
    }

    ldns_rdf *ns_rdf = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_A, forward_dns);
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

// 克隆一个请求包并修改转发域名
ldns_pkt* modify_query_domain(ldns_pkt *original_pkt,  ldns_rdf *new_domain) {

    ldns_rr_list *questions = ldns_pkt_question(original_pkt);
    ldns_rr *original_question = ldns_rr_list_rr(questions, 0);
    ldns_rr *new_question = ldns_rr_clone(original_question);

    ldns_rr_set_owner(new_question, new_domain);

    ldns_rr_list *new_questions = ldns_rr_list_new();
    if (!new_questions) {
        ldns_rr_free(new_question);
        return NULL;
    }
    ldns_rr_list_push_rr(new_questions, new_question);

    ldns_pkt *new_pkt = ldns_pkt_clone(original_pkt);
    if (!new_pkt) {
        ldns_rr_list_deep_free(new_questions);
        return NULL;
    }

    ldns_pkt_set_question(new_pkt, new_questions);

    return new_pkt;
}

// 处理单个DNS查询
void process_dns_query(int sockfd, const uint8_t *buf, ssize_t len,
                        struct sockaddr_in *client, socklen_t client_len) {

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

    ldns_pkt *resp_pkt = NULL;

    // 防止环路
    uint16_t hops = get_loop_marker(query_pkt);
    if (hops < max_hops) {

        qname_str[strcspn(qname_str, "\n")] = 0;
        log_msg(LOG_DEBUG, "Query for: '%s', Type: %s, ID: %d", qname_str,
                    ldns_rr_type2str(ldns_rr_get_type(qrr)), ldns_pkt_id(query_pkt));

        // 首先检查是否是配后缀的域名
        if (!is_match_suffix(qname_str)) {
            log_msg(LOG_DEBUG, "Not a %s domain, returning REFUSED", suffix_domain);
        } 
        else {
            // 然后检查是否是网关域名
            if (gateway_name[0] && is_gateway_domain(qname_str)) {
                log_msg(LOG_DEBUG, "Handling gateway domain: %s", qname_str);
                resp_pkt = handle_gateway_query(query_pkt, qrr, client->sin_addr);
            }
            // 其他匹配后缀的域名
            else {
                char *modified_name = strdup(qname_str);
                if (modified_name) {
                    if (!keep_suffix){
                        strip_suffix(modified_name);
                    } else {
                        strip_dot(modified_name);
                    }

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
                        
                        ldns_pkt *clone_pkt = modify_query_domain(query_pkt, rdf_name);
                        add_loop_marker(clone_pkt, hops + 1);
                        log_msg(LOG_DEBUG, "Add loop marker hops -> %d", hops);

                        int reqType = ldns_rr_get_type(qrr);
                        // Type A:1 or Type AAAA: 28
                        if (reqType != 1 && reqType != 28) {
                            log_msg(LOG_INFO, "Response %s(%d) query for '%s' from %s",
                                ldns_rr_type2str(ldns_rr_get_type(qrr)),
                                ldns_rr_get_type(qrr),
                                modified_name,
                                "NOERROR"
                                );

                            // 返回空响应
                            log_msg(LOG_DEBUG, "Creating NOERROR response");
                            resp_pkt = ldns_pkt_new();
                            if (resp_pkt) {
                                ldns_pkt_set_id(resp_pkt, ldns_pkt_id(query_pkt));
                                ldns_pkt_set_qr(resp_pkt, 1);
                                ldns_pkt_set_aa(resp_pkt, 1);
                                ldns_pkt_set_rcode(resp_pkt, LDNS_RCODE_NOERROR);
                                ldns_pkt_push_rr(resp_pkt, LDNS_SECTION_QUESTION, ldns_rr_clone(qrr));
                            }
                        } else {
                            // 转发DNS请求
                            log_msg(LOG_INFO, "Forwarding %s(%d) query for '%s' from %s to %s",
                                ldns_rr_type2str(ldns_rr_get_type(qrr)),
                                ldns_rr_get_type(qrr),
                                modified_name,
                                inet_ntoa(client->sin_addr),
                                forward_dns);

                            ldns_pkt *forward_resp = NULL;
                            ldns_status status = ldns_resolver_send_pkt(&forward_resp, fresh_resolver, clone_pkt);
                            // ldns_pkt *forward_resp = ldns_resolver_query(fresh_resolver, rdf_name, 
                            //                             ldns_rr_get_type(qrr), LDNS_RR_CLASS_IN, LDNS_RD);
                            
                            if (status == LDNS_STATUS_OK && forward_resp) {
                                uint8_t rcode = ldns_pkt_get_rcode(forward_resp);
                                const char* rcode_str = "UNKNOWN";
                                switch(rcode) {
                                    case LDNS_RCODE_NOERROR:  rcode_str = "NOERROR";  break;
                                    case LDNS_RCODE_FORMERR:  rcode_str = "FORMERR";  break;
                                    case LDNS_RCODE_SERVFAIL: rcode_str = "SERVFAIL"; break;
                                    case LDNS_RCODE_NXDOMAIN: rcode_str = "NXDOMAIN"; break;
                                    case LDNS_RCODE_NOTIMPL:  rcode_str = "NOTIMPL";  break;
                                    case LDNS_RCODE_REFUSED:  rcode_str = "REFUSED";  break;
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
                                log_msg(LOG_DEBUG, "No response from forward DNS server for '%s' (this is expected for non-existent record)", modified_name);
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
    }else{
        log_msg(LOG_WARN, "DNS forwarding loop detected: query for '%s' exceeded maximum hop count (5)", qname_str);
        log_msg(LOG_DEBUG, "Creating SERVFAIL response");

        resp_pkt = ldns_pkt_new();
        if (resp_pkt) {
            ldns_pkt_set_id(resp_pkt, ldns_pkt_id(query_pkt));
            ldns_pkt_set_qr(resp_pkt, 1);
            ldns_pkt_set_aa(resp_pkt, 1);
            ldns_pkt_set_rcode(resp_pkt, LDNS_RCODE_SERVFAIL);
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
