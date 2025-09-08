#include <stdio.h>
#include <string.h>
#include "helper.h"
#include "config.h"

// 打印帮助信息
void print_help(const char *progname) {
    printf("version: %s\n", VERSION);
    printf("Usage: %s [OPTIONS]\n", progname);
    printf("Options:\n");
    printf("  -L, --log-level    Set log level (DEBUG, default: INFO, WARN, ERROR, FATAL)\n");
    printf("  -G, --gateway      Set gateway name (default: %s)\n", GATEWAY_DEFAULT);
    printf("  -S, --suffix       Set suffix name (default: %s)\n", SUFFIX_DEFAULT);
    printf("  -C, --container    Set container name (default: %s)\n", CONTAINER_DEFAULT);
    printf("  -D, --dns-server   Set forward DNS server (default: %s)\n", FORWARD_DNS_DEFAULT);
    printf("  -P, --port         Set listening port (default: %d)\n", LISTEN_PORT_DEFAULT);
    printf("  -K, --keep-suffix  keep suffix forward dns query (default: %s)\n", KEEP_SUFFIX_DEFAULT ? "keep" : "strip");
    printf("  -M, --max-hops     Set maximum hop count (default: %d)\n", MAX_HOPS_DEFAULT);
    printf("  -W, --workers      Set number of worker threads (default: %d)\n", NUM_WORKERS_DEFAULT);
    printf("  -f, --foreground   Run in foreground mode (do not daemonize)\n");
    printf("  -h, --help         Show this help message and exit\n");
    printf("  -v, --version      Show version and exit\n");
    printf("\n");
    printf("Environment variable:\n");
    printf("  Command-line arguments take precedence over environment variables.\n");
    printf("  --log-level    =>  LOG_LEVEL\n");
    printf("  --gateway      =>  GATEWAY_NAME\n");
    printf("  --suffix       =>  SUFFIX_DOMAIN\n");
    printf("  --container    =>  CONTAINER_NAME\n");
    printf("  --dns-server   =>  FORWARD_DNS\n");
    printf("  --port         =>  LISTEN_PORT\n");
    printf("  --keep-suffix  =>  KEEP_SUFFIX\n");
    printf("  --max-hops     =>  MAX_HOPS\n");
    printf("  --workers      =>  NUM_WORKERS\n");
    printf("\n");
}

// 处理命令行选项
OptionType get_option_type(const char* arg) {
    if (arg == NULL) {
        return OPT_UNKNOWN;
    }
    
    if (strlen(arg) == 2 && arg[0] == '-') {
        switch (arg[1]) {
            case 'L': return OPT_LOG_LEVEL;
            case 'G': return OPT_GATEWAY;
            case 'S': return OPT_SUFFIX;
            case 'C': return OPT_CONTAINER;
            case 'D': return OPT_DNS_SERVER;
            case 'P': return OPT_PORT;
            case 'K': return OPT_KEEP_SUFFIX;
            case 'M': return OPT_MAX_HOPS;
            case 'W': return OPT_NUM_WORKERS;
            case 'f': return OPT_FOREGROUND;
            case 'h': return OPT_HELP;
            case 'v': return OPT_VERSION;
            default:  return OPT_UNKNOWN;
        }
    }
    else if (strlen(arg) > 2 && arg[0] == '-' && arg[1] == '-') {
        const char* opt = arg + 2;
        if (strcmp(opt, "log-level") == 0)    return OPT_LOG_LEVEL;
        if (strcmp(opt, "gateway") == 0)      return OPT_GATEWAY;
        if (strcmp(opt, "container") == 0)    return OPT_CONTAINER;
        if (strcmp(opt, "dns-server") == 0)   return OPT_DNS_SERVER;
        if (strcmp(opt, "port") == 0)         return OPT_PORT;
        if (strcmp(opt, "keep-suffix") == 0)  return OPT_KEEP_SUFFIX;
        if (strcmp(opt, "max-hops") == 0)     return OPT_MAX_HOPS;
        if (strcmp(opt, "workers") == 0)      return OPT_NUM_WORKERS;
        if (strcmp(opt, "foreground") == 0)   return OPT_FOREGROUND;
        if (strcmp(opt, "help") == 0)         return OPT_HELP;
        if (strcmp(opt, "version") == 0)      return OPT_VERSION;
        
        return OPT_UNKNOWN;
    }
    
     return OPT_UNKNOWN;
}
