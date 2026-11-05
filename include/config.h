#ifndef CONFIG_H
#define CONFIG_H
#include <stddef.h>  // for size_t

#define VERSION "1.2.0"
#define LISTEN_PORT_ENV "LISTEN_PORT"
#define DOH_PORT_ENV "DOH_PORT"
#define FORWARD_DNS_ENV "FORWARD_DNS"
#define GATEWAY_ENV "GATEWAY_NAME"
#define CONTAINER_ENV "CONTAINER_NAME"
#define SUFFIX_ENV "SUFFIX_DOMAIN"
#define LOG_LEVEL_ENV "LOG_LEVEL"
#define KEEP_SUFFIX_ENV "KEEP_SUFFIX"
#define MAX_HOPS_ENV "MAX_HOPS"
#define NUM_WORKERS_ENV "NUM_WORKERS"

#define LISTEN_PORT_DEFAULT 53
#define DOH_PORT_DEFAULT 80
#define FORWARD_DNS_DEFAULT "8.8.8.8"
#define GATEWAY_DEFAULT "gateway"
// 默认空后缀：开箱即转发所有域名（Docker 镜像通过环境变量 SUFFIX_DOMAIN=.docker 还原 Docker 行为）
#define SUFFIX_DEFAULT ""
#define LOG_LEVEL_DEFAULT LOG_INFO
#define KEEP_SUFFIX_DEFAULT 0
#define MAX_HOPS_DEFAULT 3
#define NUM_WORKERS_DEFAULT 4

extern int max_hops;
extern int num_workers;
extern int keep_suffix;
extern int foreground;
extern int listen_port;
extern int doh_port;
extern char forward_dns[16];
extern char container_name[256];
extern char gateway_name[64];
extern char suffix_domain[64];

void init_config_env(void);
void init_config_argc(int argc, char *argv[]);
int* str2int(const char *nptr);
void read_env(const char *env_name, const char *default_val, char *dest, size_t dest_size);

#endif
