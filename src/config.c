#include "config.h"
#include "helper.h"   // for print_help, get_option_type, OPT_CONTAINER, OPT...
#include "logging.h"  // for log_msg, LOG_FATAL, parse_log_level, log_level
#include <errno.h>    // for errno, ERANGE
#include <limits.h>   // for INT_MAX, INT_MIN, LONG_MAX, LONG_MIN
#include <stdio.h>    // for fprintf, printf, stderr
#include <stdlib.h>   // for exit, free, getenv, malloc, strtol
#include <string.h>   // for strncpy, memmove, strlen

int max_hops = MAX_HOPS_DEFAULT;
int num_workers = NUM_WORKERS_DEFAULT;
int keep_suffix = KEEP_SUFFIX_DEFAULT;
int foreground = 0;
int listen_port = LISTEN_PORT_DEFAULT;
char forward_dns[16] = FORWARD_DNS_DEFAULT;
char container_name[256] = {0};
char gateway_name[64] = {0};
char suffix_domain[64] = {0};

// 初始化配置(环境变量)
void init_config_env(void) {

    // 从环境变量读取
    log_level = parse_log_level(getenv(LOG_LEVEL_ENV), LOG_INFO);
    read_env(GATEWAY_ENV, GATEWAY_DEFAULT, gateway_name, sizeof(gateway_name));
    read_env(CONTAINER_ENV, CONTAINER_DEFAULT, container_name, sizeof(container_name));
    read_env(SUFFIX_ENV, SUFFIX_DEFAULT, suffix_domain, sizeof(suffix_domain));
    read_env(FORWARD_DNS_ENV, FORWARD_DNS_DEFAULT, forward_dns, sizeof(forward_dns));

    char *endptr;
    
    // 侦听端口号
    int *env_port;
    env_port = str2int(getenv(LISTEN_PORT_ENV));
    if (env_port != NULL) {
        listen_port = *env_port;
        free(env_port);
        if (listen_port <= 0 || listen_port > 65535){
            log_msg(LOG_FATAL, "Invalid port number %d", listen_port);
            exit(1);
        }
    }
    
    // 保留后缀转发
    int *env_keep_suffix;
    env_keep_suffix =  str2int(getenv(KEEP_SUFFIX_ENV));
    if (env_keep_suffix != NULL){
        keep_suffix = (*env_keep_suffix == 0) ? 0 : 1;
        free(env_keep_suffix);
    }
    
    // 最大跳数（防止循环）
    int *env_max_hops;
    env_max_hops =  str2int(getenv(MAX_HOPS_ENV));
    if (env_max_hops != NULL){
        max_hops = *env_max_hops;
        free(env_max_hops);
        if (max_hops <= 0 || max_hops > 10){
            log_msg(LOG_FATAL, "Invalid number of max hops. Must be between 1 and 10."); 
            exit(1);
        }    
    }

    // 工作线程数
    int *env_num_workers;
    env_num_workers =  str2int(getenv(NUM_WORKERS_ENV));
    if (env_num_workers != NULL){
        num_workers = *env_num_workers;
        free(env_num_workers);
        if (num_workers <= 0 || num_workers > 10){
            log_msg(LOG_FATAL, "Invalid number of workers. Must be between 1 and 10."); 
            exit(1);
        }       
    }
}

// 初始化配置(命令行参数)
void init_config_argc(int argc, char *argv[]) {

    // 从命令行参数读取
    for (int i = 1; i < argc; i++) {
        OptionType opt = get_option_type(argv[i]);
        
        switch (opt) {
            case OPT_LOG_LEVEL:
                if (i + 1 >= argc) {
                    log_msg(LOG_FATAL, "--log-level requires a value");
                    exit(1);
                }
                const char* level_str = argv[++i];
                int conv_level = parse_log_level(level_str, -1);
                if (conv_level >= LOG_DEBUG){
                    log_level = conv_level;
                } else {
                    log_msg(LOG_FATAL,"Invalid log level '%s'", level_str);
                    exit(1);
                }
                break; 

            case OPT_GATEWAY:
                if (i + 1 >= argc) {
                    log_msg(LOG_FATAL, "--gateway requires a value");
                    exit(1);
                }
                strncpy(gateway_name, argv[++i], sizeof(gateway_name) - 1);
                gateway_name[sizeof(gateway_name) - 1] = '\0';
                break;

            case OPT_SUFFIX:
                if (i + 1 >= argc) {
                    log_msg(LOG_FATAL, "--suffix requires a value");
                    exit(1);
                }
                strncpy(suffix_domain, argv[++i], sizeof(suffix_domain) - 1);
                suffix_domain[sizeof(suffix_domain) - 1] = '\0';
                break;

            case OPT_CONTAINER:
                if (i + 1 >= argc) {
                    log_msg(LOG_FATAL, "--container requires a value");
                    exit(1);
                }
                strncpy(container_name, argv[++i], sizeof(container_name) - 1);
                container_name[sizeof(container_name) - 1] = '\0';
                break;
                
            case OPT_DNS_SERVER:
                if (i + 1 >= argc) {
                    log_msg(LOG_FATAL,  "--dns-server requires a value");
                    exit(1);
                }
                strncpy(forward_dns, argv[++i], sizeof(forward_dns) - 1);
                forward_dns[sizeof(forward_dns) - 1] = '\0';
                break;
                
            case OPT_PORT:
                if (i + 1 >= argc) {
                    log_msg(LOG_FATAL, "--port requires a value");
                    exit(1);
                }
                char *argv_port = argv[++i];
                int *argv_listen_port = str2int(argv_port);
                if (argv_listen_port != NULL){
                    listen_port = *argv_listen_port;
                    free(argv_listen_port);
                    if (listen_port <= 0 || listen_port > 65535) {
                        log_msg(LOG_FATAL, "Invalid number of port. Must be between 1 and 65535."); 
                        exit(1);
                    }
                } else {
                    log_msg(LOG_FATAL, "Invalid port number %s", argv_port);
                    exit(1);
                }
                break;

            case OPT_KEEP_SUFFIX:
                keep_suffix = 1;
                break;

            case OPT_FOREGROUND:
                foreground = 1;
                break;

            case OPT_MAX_HOPS:
                if (i + 1 >= argc) {
                    log_msg(LOG_FATAL, "--max-hops requires a value");
                    exit(1);
                }
                char *argv_hops = argv[++i];
                int *argv_max_hops = str2int(argv_hops);
                if (argv_max_hops != NULL){
                    max_hops = *argv_max_hops;
                    free(argv_max_hops);
                    if (max_hops <= 0 || max_hops > 10) {
                        log_msg(LOG_FATAL, "Invalid number of max hops. Must be between 1 and 10."); 
                        exit(1);
                    }
                } else {
                    log_msg(LOG_FATAL, "Invalid max hops number %s", argv_hops);
                    exit(1);
                }
                break;

            case OPT_NUM_WORKERS:
                if (i + 1 >= argc) {
                    log_msg(LOG_FATAL, "--port requires a value");
                    exit(1);
                }
                char *argv_workers = argv[++i];
                int *argv_num_workers = str2int(argv_workers);
                if (argv_num_workers != NULL){
                    num_workers = *argv_num_workers;
                    free(argv_num_workers);
                    if (num_workers <= 0 || num_workers > 10) {
                        log_msg(LOG_FATAL, "Invalid number of workers. Must be between 1 and 10."); 
                        exit(1);
                    }
                } else {
                    log_msg(LOG_FATAL, "Invalid workers number %d", argv_workers);
                }
                break;
                
            case OPT_HELP:
                print_help(argv[0]);
                exit(0);
            case OPT_VERSION: 
                printf("version: %s\n", VERSION);
                exit(0);
            case OPT_UNKNOWN:
                fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
                print_help(argv[0]);
                exit(1);
        }
    }

    // 防止后缀.缺失
    if (suffix_domain[0] != '\0' && suffix_domain[0] != '.') {
        memmove(suffix_domain + 1, suffix_domain, strlen(suffix_domain) + 1);
        suffix_domain[0] = '.';
    }

}

// 将字符农村转为int
int* str2int(const char *nptr) {

    if (nptr == NULL) return NULL; 

    char *endptr;
    errno = 0;

    long val = strtol(nptr, &endptr, 10);

    // 检查转换错误
    if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN)) || 
        (errno != 0 && val == 0) || endptr == nptr || 
        *endptr != '\0' ||val < INT_MIN || val > INT_MAX
    ) return NULL; 
    
    int *result = malloc(sizeof(int));
    if (result == NULL) return NULL; 

    *result = (int)val;
    return result;
}

// 读取环境变量
void read_env(const char *env_name, const char *default_val, char *dest, size_t dest_size) {
    if (!env_name || !default_val || !dest || dest_size == 0) {
        log_msg(LOG_FATAL, "Invalid parameters for read_env_variable");
        exit(1);
    }

    const char *env_value = getenv(env_name);
    if (env_value) {
        strncpy(dest, env_value, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else {
        strncpy(dest, default_val, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}
