#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <ldns/ldns.h>
#include <time.h>
#include <stdarg.h>

#define LISTEN_PORT 53
#define FORWARD_DNS "127.0.0.11"
#define BUF_SIZE 4096
#define DEBUG 1

volatile sig_atomic_t stop = 0;

void debug_log(const char *format, ...) {
    if (!DEBUG) return;
    
    time_t now;
    time(&now);
    char *timestr = ctime(&now);
    timestr[strlen(timestr)-1] = '\0';
    
    printf("[%s] ", timestr);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void handle_sigterm(int sig) {
    debug_log("Received signal %d", sig);
    stop = 1;
}

int is_docker_domain(const char *name) {
    if (!name) return 0;
    size_t len = strlen(name);
    
    // 处理以点结尾的FQDN
    if (len > 0 && name[len-1] == '.') {
        len--;
    }
    
    int result = (len >= 7 && strncasecmp(name + len - 7, ".docker", 7) == 0);
    debug_log("Checking if '%s' is docker domain: %s", name, result ? "YES" : "NO");
    return result;
}

void strip_docker_suffix(char *name) {
    if (!name) return;
    size_t len = strlen(name);
    
    // 先移除尾部的点（如果存在）
    if (len > 0 && name[len-1] == '.') {
        name[len-1] = '\0';
        len--;
    }
    
    // 然后移除.docker后缀
    if (len >= 7) {
        name[len - 7] = '\0';
        debug_log("Stripped .docker suffix, new name: '%s'", name);
    }
}

int test_forward_dns() {
    debug_log("Testing connection to forward DNS server %s", FORWARD_DNS);
    
    ldns_resolver *test_resolver = ldns_resolver_new();
    if (!test_resolver) {
        debug_log("Failed to create test resolver");
        return 0;
    }

    ldns_rdf *ns_rdf = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_A, FORWARD_DNS);
    if (!ns_rdf) {
        debug_log("Failed to create nameserver RDF for %s", FORWARD_DNS);
        ldns_resolver_deep_free(test_resolver);
        return 0;
    }
    
    ldns_resolver_push_nameserver(test_resolver, ns_rdf);
    ldns_rdf_deep_free(ns_rdf);

    struct timeval tv = {2, 0};
    ldns_resolver_set_timeout(test_resolver, tv);
    ldns_resolver_set_retry(test_resolver, 1);

    ldns_rdf *test_name = NULL;
    ldns_str2rdf_dname(&test_name, "google.com");
    
    if (test_name) {
        ldns_pkt *test_resp = ldns_resolver_query(test_resolver, test_name, 
                                                LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN, LDNS_RD);
        ldns_rdf_deep_free(test_name);
        
        if (test_resp) {
            debug_log("Forward DNS server %s is reachable", FORWARD_DNS);
            ldns_pkt_free(test_resp);
            ldns_resolver_deep_free(test_resolver);
            return 1;
        } else {
            debug_log("Forward DNS server %s is not responding", FORWARD_DNS);
        }
    }
    
    ldns_resolver_deep_free(test_resolver);
    return 0;
}

int main() {
    signal(SIGTERM, handle_sigterm);
    signal(SIGINT, handle_sigterm);

    debug_log("Starting Docker DNS forwarder");

    if (!test_forward_dns()) {
        debug_log("WARNING: Forward DNS server may not be available");
    }

    int sockfd;
    struct sockaddr_in addr, client;
    socklen_t client_len = sizeof(client);
    uint8_t buf[BUF_SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { 
        perror("socket"); 
        return 1; 
    }

    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(sockfd);
        return 1;
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(LISTEN_PORT);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); 
        close(sockfd);
        return 1;
    }

    struct timeval tv = {1, 0};
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sockfd);
        return 1;
    }

    ldns_resolver *resolver = ldns_resolver_new();
    if (!resolver) { 
        debug_log("Failed to create resolver"); 
        close(sockfd);
        return 1; 
    }

    ldns_rdf *ns_rdf = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_A, FORWARD_DNS);
    if (!ns_rdf) {
        debug_log("Failed to create nameserver RDF");
        ldns_resolver_deep_free(resolver);
        close(sockfd);
        return 1;
    }
    
    ldns_resolver_push_nameserver(resolver, ns_rdf);
    ldns_rdf_deep_free(ns_rdf);

    tv.tv_sec = 2; 
    tv.tv_usec = 0;
    ldns_resolver_set_timeout(resolver, tv);
    ldns_resolver_set_retry(resolver, 1);

    debug_log("DNS forwarder listening on port %d, forwarding *.docker to %s", 
              LISTEN_PORT, FORWARD_DNS);

    while (!stop) {
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, 
                            (struct sockaddr*)&client, &client_len);
        if (n <= 0) continue;

        debug_log("Received DNS query from %s:%d (%zd bytes)", 
                  inet_ntoa(client.sin_addr), ntohs(client.sin_port), n);

        ldns_pkt *query_pkt = NULL;
        if (ldns_wire2pkt(&query_pkt, buf, n) != LDNS_STATUS_OK || !query_pkt) {
            debug_log("Failed to parse DNS query packet");
            continue;
        }

        ldns_rr_list *question = ldns_pkt_question(query_pkt);
        if (!question || ldns_rr_list_rr_count(question) == 0) { 
            debug_log("No questions in DNS query");
            ldns_pkt_free(query_pkt); 
            continue; 
        }

        ldns_rr *qrr = ldns_rr_list_rr(question, 0);
        if (!qrr) {
            debug_log("Failed to get question RR");
            ldns_pkt_free(query_pkt);
            continue;
        }

        ldns_rdf *qname = ldns_rr_owner(qrr);
        if (!qname) {
            debug_log("Failed to get question name");
            ldns_pkt_free(query_pkt);
            continue;
        }

        char *qname_str = ldns_rdf2str(qname);
        if (!qname_str) {
            debug_log("Failed to convert question name to string");
            ldns_pkt_free(query_pkt);
            continue;
        }

        qname_str[strcspn(qname_str, "\n")] = 0;
        debug_log("Query for: '%s', Type: %s, ID: %d", qname_str, 
                  ldns_rr_type2str(ldns_rr_get_type(qrr)), ldns_pkt_id(query_pkt));

        ldns_pkt *resp_pkt = NULL;

        if (is_docker_domain(qname_str)) {
            char *modified_name = strdup(qname_str);
            if (modified_name) {
                strip_docker_suffix(modified_name);
                debug_log("Forwarding query for '%s' to %s", modified_name, FORWARD_DNS);

                ldns_rdf *rdf_name = NULL;
                if (ldns_str2rdf_dname(&rdf_name, modified_name) == LDNS_STATUS_OK && rdf_name) {
                    ldns_pkt *forward_resp = ldns_resolver_query(resolver, rdf_name, 
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
                        debug_log("Forward DNS response: %s (%d answers)", 
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
                            
                            debug_log("Created response with original question section");
                        }
                        
                        ldns_pkt_free(forward_resp);
                    } else {
                        debug_log("No response from forward DNS server");
                    }
                    
                    ldns_rdf_deep_free(rdf_name);
                } else {
                    debug_log("Failed to create RDF name for '%s'", modified_name);
                }
                free(modified_name);
            }
        } else {
            debug_log("Not a .docker domain, returning REFUSED");
        }

        if (!resp_pkt) {
            debug_log("Creating REFUSED response");
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
                                    (struct sockaddr*)&client, client_len);
                debug_log("Sent response (%zd bytes)", sent);
                free(out);
            } else {
                debug_log("Failed to serialize response packet");
            }
            
            ldns_pkt_free(resp_pkt);
        }

        free(qname_str);
        ldns_pkt_free(query_pkt);
    }

    debug_log("Shutting down gracefully");
    ldns_resolver_deep_free(resolver);
    close(sockfd);
    return 0;
}