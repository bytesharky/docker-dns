/*
 * doh.c - docker-dns 内置的 DoH / HTTP-DNS 服务器
 *
 * 本文件把 httpsdns 的 HTTP 能力合并进 docker-dns 二进制，复用 docker-dns 自身的
 * 解析逻辑（resolve_dns_query），因此 .docker 后缀 / gateway.docker 等规则在
 * DoH / HTTP 路径下同样生效。
 *
 * 提供三种查询方式：
 *   1) RFC 8484 DNS-over-HTTPS：
 *        - POST /dns-query  （或任意路径）
 *          Content-Type: application/dns-message
 *          Body: DNS 报文（wire format，二进制）
 *        - GET  /dns-query?dns=<base64url 编码的 DNS 报文>
 *      返回 Content-Type: application/dns-message，Body 为响应 DNS 报文。
 *   2) JSON 调试端点：GET /?domain=example.com&type=A
 *      返回 application/json，方便客户端/浏览器直接解析。
 *   3) 健康检查：GET /healthz  或  GET /
 *
 * 配置：侦听端口由 DOH_PORT 环境变量 / -H / --doh-port 参数控制（默认 80）。
 * 编译：与 docker-dns 一同通过 ./src/*.c 静态编译，链接 -lldns。
 */

#define _GNU_SOURCE
#include "config.h"      // for doh_port, forward_dns, suffix_domain
#include "dns.h"         // for resolve_dns_query
#include "logging.h"     // for log_msg, LOG_*
#include "sigterm.h"     // for stop (volatile sig_atomic_t)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <ldns/ldns.h>

/* ----------------------- 动态字符串缓冲（JSON 用） ----------------------- */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} sbuf;

static void sb_init(sbuf *s) {
    s->cap = 1024;
    s->len = 0;
    s->buf = (char *)malloc(s->cap);
    s->buf[0] = '\0';
}
static void sb_reserve(sbuf *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return;
    while (s->len + extra + 1 > s->cap) s->cap *= 2;
    s->buf = (char *)realloc(s->buf, s->cap);
}
static void sb_append(sbuf *s, const char *str) {
    size_t l = strlen(str);
    sb_reserve(s, l);
    memcpy(s->buf + s->len, str, l + 1);
    s->len += l;
}
static void sb_char(sbuf *s, char c) {
    sb_reserve(s, 1);
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}
static void json_str(sbuf *s, const char *str) {
    sb_append(s, "\"");
    for (const char *p = str; p && *p; p++) {
        char c = *p;
        switch (c) {
            case '"':  sb_append(s, "\\\""); break;
            case '\\': sb_append(s, "\\\\"); break;
            case '\n': sb_append(s, "\\n");  break;
            case '\r': sb_append(s, "\\r");  break;
            case '\t': sb_append(s, "\\t");  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", (unsigned char)c);
                    sb_append(s, buf);
                } else {
                    sb_char(s, c);
                }
        }
    }
    sb_append(s, "\"");
}
static void trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

/* ----------------------- 字节缓冲（请求/响应用） ----------------------- */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} bbuf;

static void bb_init(bbuf *b) {
    b->cap = 4096;
    b->len = 0;
    b->buf = (uint8_t *)malloc(b->cap);
}
static void bb_append(bbuf *b, const uint8_t *d, size_t n) {
    if (b->len + n > b->cap) {
        while (b->len + n > b->cap) b->cap *= 2;
        b->buf = (uint8_t *)realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, d, n);
    b->len += n;
}

/* ----------------------- 工具函数 ----------------------- */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static void urldecode(char *dst, const char *src) {
    char *d = dst;
    const char *s = src;
    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            *d++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 3;
        } else if (*s == '+') {
            *d++ = ' ';
            s++;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
}
static int get_param(const char *query, const char *key, char *out, size_t outsz) {
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && (p[klen] == '=' || p[klen] == '\0')) {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < outsz - 1)
                out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return 0;
}
/* 返回 malloc 的参数字符串（适合较大的 dns 参数） */
static char *get_param_alloc(const char *query, const char *key) {
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && (p[klen] == '=' || p[klen] == '\0')) {
            p += klen + 1;
            const char *e = p;
            while (*e && *e != '&') e++;
            size_t l = (size_t)(e - p);
            char *r = (char *)malloc(l + 1);
            memcpy(r, p, l);
            r[l] = '\0';
            return r;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return NULL;
}
static ldns_rr_type parse_type(const char *t) {
    if (!strcasecmp(t, "A"))     return LDNS_RR_TYPE_A;
    if (!strcasecmp(t, "AAAA"))  return LDNS_RR_TYPE_AAAA;
    if (!strcasecmp(t, "CNAME")) return LDNS_RR_TYPE_CNAME;
    if (!strcasecmp(t, "MX"))    return LDNS_RR_TYPE_MX;
    if (!strcasecmp(t, "TXT"))   return LDNS_RR_TYPE_TXT;
    if (!strcasecmp(t, "NS"))    return LDNS_RR_TYPE_NS;
    if (!strcasecmp(t, "SOA"))   return LDNS_RR_TYPE_SOA;
    if (!strcasecmp(t, "PTR"))   return LDNS_RR_TYPE_PTR;
    if (!strcasecmp(t, "SRV"))   return LDNS_RR_TYPE_SRV;
    if (!strcasecmp(t, "CAA"))   return LDNS_RR_TYPE_CAA;
    return LDNS_RR_TYPE_A;
}
static const char *rcode_name(int r) {
    switch (r) {
        case 0: return "NOERROR";
        case 1: return "FORMERR";
        case 2: return "SERVFAIL";
        case 3: return "NXDOMAIN";
        case 4: return "NOTIMP";
        case 5: return "REFUSED";
        default: return "UNKNOWN";
    }
}

/* ----------------------- 发送辅助 ----------------------- */
static ssize_t send_all(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, (const char *)buf + sent, n - sent, 0);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return (ssize_t)sent;
}

/* ----------------------- 后端解析（复用 docker-dns 逻辑） ----------------------- */
/* 把 DNS 查询报文交给 docker-dns 的 resolve_dns_query 处理，并序列化为 wire 格式。
 * 成功返回 0，*resp 为 malloc 的响应报文（调用方需 free），*rlen 为其长度。 */
static int resolve_to_wire(const uint8_t *q, size_t qlen, uint8_t **resp, size_t *rlen) {
    ldns_pkt *pkt = resolve_dns_query(q, qlen, NULL);
    if (!pkt) {
        *resp = NULL;
        *rlen = 0;
        return -1;
    }
    uint8_t *wire = NULL;
    size_t wlen = 0;
    int rc = (ldns_pkt2wire(&wire, pkt, &wlen) == LDNS_STATUS_OK && wire) ? 0 : -1;
    ldns_pkt_free(pkt);
    *resp = wire;   /* 可能为空；调用方在 *resp 非空时 free */
    *rlen = wlen;
    return rc;
}

/* base64url 解码（RFC 4648 §5，无填充也接受） */
static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
static int base64url_decode(const char *in, uint8_t **out, size_t *outlen) {
    size_t inlen = strlen(in);
    while (inlen > 0 && in[inlen - 1] == '=') inlen--;
    uint8_t *buf = (uint8_t *)malloc(inlen * 3 / 4 + 4);
    size_t oi = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < inlen; i++) {
        int v = b64val(in[i]);
        if (v < 0) { free(buf); return -1; }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[oi++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    *out = buf;
    *outlen = oi;
    return 0;
}

/* ----------------------- JSON GET 路径 ----------------------- */
static void rr_to_json(ldns_rr *rr, sbuf *out) {
    ldns_rdf *owner = ldns_rr_owner(rr);
    char *owner_s = owner ? ldns_rdf2str(owner) : strdup("");
    uint32_t ttl = ldns_rr_ttl(rr);
    ldns_rr_type t = ldns_rr_get_type(rr);
    char *type_s = ldns_rr_type2str(t);
    trim(owner_s);
    trim(type_s);

    sb_append(out, "{\"name\":");
    json_str(out, owner_s);
    sb_append(out, ",\"type\":");
    json_str(out, type_s);
    sb_append(out, ",\"ttl\":");
    char nb[16];
    snprintf(nb, sizeof nb, "%u", ttl);
    sb_append(out, nb);
    sb_append(out, ",\"data\":[");

    size_t rc = ldns_rr_rd_count(rr);
    for (size_t j = 0; j < rc; j++) {
        ldns_rdf *rdf = ldns_rr_rdf(rr, j);
        char *fs = rdf ? ldns_rdf2str(rdf) : strdup("");
        trim(fs);
        if (j) sb_append(out, ",");
        json_str(out, fs);
        free(fs);
    }
    sb_append(out, "]}");

    free(owner_s);
    free(type_s);
}

/* 手工构造 DNS 查询报文（12 字节头 + QNAME + QTYPE + QCLASS）。
 * 仅把 ldns 用于「解析响应」，自己拼装 wire 以避免隐式声明截断指针的隐患。 */
static size_t build_query_wire(const char *domain, uint16_t qtype,
                               uint8_t *out, size_t outcap) {
    size_t pos = 0;
    if (outcap < 12) return 0;
    out[pos++] = 0x00; out[pos++] = 0x01;          /* ID */
    out[pos++] = 0x01; out[pos++] = 0x00;          /* Flags: RD */
    out[pos++] = 0x00; out[pos++] = 0x01;          /* QDCOUNT = 1 */
    out[pos++] = 0x00; out[pos++] = 0x00;          /* ANCOUNT = 0 */
    out[pos++] = 0x00; out[pos++] = 0x00;          /* NSCOUNT = 0 */
    out[pos++] = 0x00; out[pos++] = 0x00;          /* ARCOUNT = 0 */

    const char *p = domain;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t lablen = dot ? (size_t)(dot - p) : strlen(p);
        if (lablen == 0 || lablen > 63) return 0;
        if (pos + 1 + lablen + 1 > outcap) return 0;
        out[pos++] = (uint8_t)lablen;
        memcpy(out + pos, p, lablen);
        pos += lablen;
        if (!dot) break;
        p = dot + 1;
    }
    if (pos + 1 > outcap) return 0;
    out[pos++] = 0x00;                              /* root label */

    if (pos + 4 > outcap) return 0;
    out[pos++] = (uint8_t)(qtype >> 8);            /* QTYPE */
    out[pos++] = (uint8_t)(qtype & 0xff);
    out[pos++] = 0x00; out[pos++] = 0x01;          /* QCLASS = IN */

    return pos;
}

static int do_query_json(const char *domain, const char *type_str, sbuf *out) {
    ldns_rr_type type = parse_type(type_str);
    uint8_t qbuf[512];
    size_t qlen = build_query_wire(domain, (uint16_t)type, qbuf, sizeof qbuf);
    if (qlen == 0) {
        sb_append(out, "{\"error\":\"invalid domain or type\"}");
        return 0;
    }

    ldns_pkt *pkt = resolve_dns_query(qbuf, qlen, NULL);
    if (!pkt) {
        sb_append(out, "{\"error\":\"failed to build/parse DNS query\"}");
        return 0;
    }

    int rcode = (int)ldns_pkt_get_rcode(pkt);
    const char *status = rcode_name(rcode);
    ldns_rr_list *ans = ldns_pkt_answer(pkt);
    size_t n = ldns_rr_list_rr_count(ans);

    sb_append(out, "{\"domain\":"); json_str(out, domain);
    sb_append(out, ",\"type\":");   json_str(out, type_str);
    sb_append(out, ",\"server\":"); json_str(out, forward_dns);
    sb_append(out, ",\"rcode\":");
    char nb[16];
    snprintf(nb, sizeof nb, "%d", rcode);
    sb_append(out, nb);
    sb_append(out, ",\"status\":"); json_str(out, status);
    sb_append(out, ",\"answers\":");
    snprintf(nb, sizeof nb, "%zu", n);
    sb_append(out, nb);
    sb_append(out, ",\"answer\":[");
    for (size_t i = 0; i < n; i++) {
        if (i) sb_append(out, ",");
        rr_to_json(ldns_rr_list_rr(ans, i), out);
    }
    sb_append(out, "]}");

    ldns_pkt_free(pkt);
    return 1;
}

/* ----------------------- HTTP 响应 ----------------------- */
static const char *code_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 405: return "Method Not Allowed";
        case 415: return "Unsupported Media Type";
        case 502: return "Bad Gateway";
        default:  return "Error";
    }
}
static void send_response(int fd, int code, const char *codetext, sbuf *body) {
    char header[256];
    int hl = snprintf(header, sizeof header,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, codetext, body->len);
    send_all(fd, header, (size_t)hl);
    send_all(fd, body->buf, body->len);
}
static void send_dns_response(int fd, int code, const uint8_t *body, size_t blen) {
    char header[256];
    int hl = snprintf(header, sizeof header,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/dns-message\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, code_text(code), blen);
    send_all(fd, header, (size_t)hl);
    if (blen) send_all(fd, body, blen);
}
static void send_error(int fd, int code, const char *msg) {
    sbuf b; sb_init(&b);
    sb_append(&b, "{\"error\":");
    json_str(&b, msg);
    sb_append(&b, "}");
    send_response(fd, code, code_text(code), &b);
    free(b.buf);
}

/* ----------------------- 请求读取（含 body） ----------------------- */
static size_t find_header_end(const uint8_t *b, size_t len) {
    if (len < 4) return 0;
    for (size_t i = 0; i + 3 < len; i++)
        if (b[i] == '\r' && b[i+1] == '\n' && b[i+2] == '\r' && b[i+3] == '\n')
            return i + 4;
    return 0;
}
/* 大小写不敏感的定长子串查找（hay 长度为 hlen，允许含内嵌 NUL） */
static const char *str_ci_str(const char *hay, size_t hlen, const char *needle) {
    if (!*needle) return hay;
    size_t nlen = strlen(needle);
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == nlen) return hay + i;
    }
    return NULL;
}

static long parse_content_length(const char *hdr, size_t hlen) {
    const char *p = str_ci_str(hdr, hlen, "content-length:");
    if (!p) return -1;
    p += strlen("content-length:");
    while (*p == ' ' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    long v = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; }
    return v;
}

/* ----------------------- 连接处理线程 ----------------------- */
static void *handle_client(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    pthread_detach(pthread_self());

    /* 读取完整请求（头部 + body） */
    bbuf req;
    bb_init(&req);
    char tmp[4096];
    size_t hend = 0;
    long needed = -1;
    int parsed_cl = 0;
    int ok = 1;
    while (1) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) { if (req.len == 0) ok = 0; break; }
        bb_append(&req, (uint8_t *)tmp, (size_t)n);
        if (hend == 0) hend = find_header_end(req.buf, req.len);
        if (hend && !parsed_cl) {
            char *hcopy = (char *)malloc(hend + 1);
            memcpy(hcopy, req.buf, hend);
            hcopy[hend] = '\0';
            long cl = parse_content_length(hcopy, hend);
            free(hcopy);
            needed = (cl < 0) ? (long)hend : hend + cl;
            parsed_cl = 1;
        }
        if (hend && (long)req.len >= needed) break;
        if (req.len > 70000) { ok = 0; break; } /* 防滥用 */
    }

    if (!ok || hend == 0) {
        if (req.len) send_error(fd, 400, "Bad Request");
        close(fd);
        free(req.buf);
        return NULL;
    }

    /* 解析请求行 */
    char *hdr = (char *)malloc(hend + 1);
    memcpy(hdr, req.buf, hend);
    hdr[hend] = '\0';

    char *sp = strchr(hdr, ' ');
    if (!sp) { send_error(fd, 400, "Bad Request"); free(hdr); free(req.buf); close(fd); return NULL; }
    *sp = '\0';
    char *method = hdr;
    char *url = sp + 1;
    char *sp2 = strchr(url, ' ');
    if (sp2) *sp2 = '\0';

    char *q = strchr(url, '?');
    char *query = "";
    if (q) { *q = '\0'; query = q + 1; }

    const uint8_t *body = req.buf + hend;
    size_t bodylen = req.len - hend;

    /* -------- POST：RFC 8484 DoH -------- */
    if (strcmp(method, "POST") == 0) {
        if (!str_ci_str(hdr, hend, "application/dns-message")) {
            send_error(fd, 415, "Content-Type must be application/dns-message");
        } else if (bodylen < 12) {
            send_error(fd, 400, "Body is not a valid DNS message");
        } else {
            uint8_t *resp = NULL;
            size_t rlen = 0;
            if (resolve_to_wire(body, bodylen, &resp, &rlen) == 0) {
                send_dns_response(fd, 200, resp, rlen);
                free(resp);
            } else {
                send_error(fd, 502, "DNS resolving failed");
            }
        }
        free(hdr);
        free(req.buf);
        close(fd);
        return NULL;
    }

    if (strcmp(method, "GET") != 0) {
        send_error(fd, 405, "Only GET and POST are supported");
        free(hdr); free(req.buf); close(fd);
        return NULL;
    }

    /* -------- GET：健康检查 -------- */
    if (strcmp(url, "/healthz") == 0 ||
        (strcmp(url, "/") == 0 && query[0] == '\0')) {
        sbuf b; sb_init(&b);
        sb_append(&b, "{\"status\":\"ok\",\"server\":");
        json_str(&b, forward_dns);
        sb_append(&b, ",\"suffix\":");
        json_str(&b, suffix_domain);
        sb_append(&b, "}");
        send_response(fd, 200, "OK", &b);
        free(b.buf);
        free(hdr); free(req.buf); close(fd);
        return NULL;
    }

    /* -------- GET：RFC 8484 DoH（?dns=base64url） -------- */
    char *dnsparam = get_param_alloc(query, "dns");
    if (dnsparam) {
        urldecode(dnsparam, dnsparam);
        uint8_t *qb = NULL;
        size_t ql = 0;
        if (base64url_decode(dnsparam, &qb, &ql) == 0 && ql >= 12) {
            uint8_t *resp = NULL;
            size_t rlen = 0;
            if (resolve_to_wire(qb, ql, &resp, &rlen) == 0) {
                send_dns_response(fd, 200, resp, rlen);
                free(resp);
            } else {
                send_error(fd, 502, "DNS resolving failed");
            }
        } else {
            send_error(fd, 400, "Invalid 'dns' parameter");
        }
        free(dnsparam);
        free(qb);
        free(hdr); free(req.buf); close(fd);
        return NULL;
    }

    /* -------- GET：JSON 查询 -------- */
    char domain[1024];
    char type_str[32];
    if (!get_param(query, "domain", domain, sizeof domain)) {
        send_error(fd, 400, "Missing 'domain' parameter");
        free(hdr); free(req.buf); close(fd);
        return NULL;
    }
    urldecode(domain, domain);
    if (!get_param(query, "type", type_str, sizeof type_str))
        strcpy(type_str, "A");
    urldecode(type_str, type_str);

    sbuf b; sb_init(&b);
    int good = do_query_json(domain, type_str, &b);
    if (good)
        send_response(fd, 200, "OK", &b);
    else
        send_response(fd, 502, "Bad Gateway", &b);
    free(b.buf);

    free(hdr);
    free(req.buf);
    close(fd);
    return NULL;
}

/* ----------------------- DoH 服务器线程 ----------------------- */
void* doh_server_thread(void *arg) {
    (void)arg;
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        log_msg(LOG_FATAL, "DoH: failed to create socket: %s", strerror(errno));
        return NULL;
    }
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((uint16_t)doh_port);

    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        log_msg(LOG_FATAL, "DoH: failed to bind port %d: %s", doh_port, strerror(errno));
        close(lfd);
        return NULL;
    }
    if (listen(lfd, 128) < 0) {
        log_msg(LOG_FATAL, "DoH: listen failed: %s", strerror(errno));
        close(lfd);
        return NULL;
    }

    log_msg(LOG_INFO, "DoH/HTTP DNS server listening on port %d", doh_port);

    while (!stop) {
        int *cfd = (int *)malloc(sizeof(int));
        *cfd = accept(lfd, NULL, NULL);
        if (*cfd < 0) {
            free(cfd);
            if (errno == EINTR) continue;
            log_msg(LOG_ERROR, "DoH: accept failed: %s", strerror(errno));
            break;
        }
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cfd) != 0)
            handle_client(cfd); /* 线程创建失败则同步处理 */
    }

    close(lfd);
    return NULL;
}
