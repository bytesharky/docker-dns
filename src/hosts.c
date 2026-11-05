#include "hosts.h"
#include "logging.h"      // for log_msg, LOG_INFO, LOG_WARN
#include <arpa/inet.h>    // for inet_pton, AF_INET, AF_INET6, struct in_addr/in6_addr
#ifdef _WIN32
#include <ws2tcpip.h>     // Windows 下 inet_pton 声明（兼容真实交叉编译环境）
#endif
#include <ctype.h>        // for tolower
#include <stdio.h>        // for FILE, fopen, fgets, fclose
#include <stdlib.h>       // for free, realloc
#include <string.h>       // for strlen, strncpy, strcmp

typedef struct {
    char name[256];   // 规范化后的主机名（小写、去尾点）
    int  family;      // AF_INET / AF_INET6
    char ip[46];      // 文本形式 IP
} hosts_rec_t;

static hosts_rec_t *g_recs = NULL;
static size_t g_count = 0;
static size_t g_cap = 0;

static void norm_lower(char *s) {
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

static int add_record(const char *name, int family, const char *ip) {
    if (g_count >= g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 64;
        hosts_rec_t *nr = realloc(g_recs, ncap * sizeof(*nr));
        if (!nr) return -1;
        g_recs = nr;
        g_cap = ncap;
    }
    hosts_rec_t *r = &g_recs[g_count++];
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->family = family;
    strncpy(r->ip, ip, sizeof(r->ip) - 1);
    r->ip[sizeof(r->ip) - 1] = '\0';
    return 0;
}

int hosts_load(const char *path) {
    hosts_free();
    if (!path || path[0] == '\0') {
        log_msg(LOG_DEBUG, "hosts: disabled (empty path)");
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        log_msg(LOG_WARN, "hosts: cannot open '%s', local hosts resolution disabled", path);
        return -1;
    }

    char line[1024];
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        /* 第一个 token = IP */
        char *ip = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }

        int family = 0;
        struct in_addr v4;
        struct in6_addr v6;
        if (inet_pton(AF_INET, ip, &v4) == 1)       family = AF_INET;
        else if (inet_pton(AF_INET6, ip, &v6) == 1) family = AF_INET6;
        else continue; /* 非合法 IP 行，跳过 */

        /* 其余 token = 主机名 */
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '#') break;

            char *host = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '#') p++;
            int had_end = (*p != '\0');
            if (*p) *p = '\0';

            char norm[256];
            strncpy(norm, host, sizeof(norm) - 1);
            norm[sizeof(norm) - 1] = '\0';
            size_t hl = strlen(norm);
            if (hl > 0 && norm[hl - 1] == '.') norm[hl - 1] = '\0';
            norm_lower(norm);
            if (norm[0]) {
                add_record(norm, family, ip);
                loaded++;
            }
            if (!had_end) break;
            p++; /* 越过刚被置空的定界符，指向下一个 token 起始 */
        }
    }
    fclose(f);
    log_msg(LOG_INFO, "hosts: loaded %d entry(ies) from '%s'", loaded, path);
    return 0;
}

int hosts_lookup(const char *name, int want_family, char out_ip[][46], size_t max_out) {
    if (!name || !g_recs || max_out == 0) return 0;

    char norm[256];
    strncpy(norm, name, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = '\0';
    size_t l = strlen(norm);
    if (l > 0 && norm[l - 1] == '.') norm[l - 1] = '\0';
    norm_lower(norm);

    int found = 0;
    for (size_t i = 0; i < g_count && (size_t)found < max_out; i++) {
        if (strcmp(g_recs[i].name, norm) == 0 &&
            (want_family == 0 || g_recs[i].family == want_family)) {
            strncpy(out_ip[found], g_recs[i].ip, 45);
            out_ip[found][45] = '\0';
            found++;
        }
    }
    return found;
}

void hosts_free(void) {
    free(g_recs);
    g_recs = NULL;
    g_count = g_cap = 0;
}
