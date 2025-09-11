#ifndef HELPER_H
#define HELPER_H

typedef enum {
    OPT_UNKNOWN,
    OPT_LOG_LEVEL,
    OPT_GATEWAY,
    OPT_SUFFIX,
    OPT_CONTAINER,
    OPT_DNS_SERVER,
    OPT_PORT,
    OPT_KEEP_SUFFIX,
    OPT_MAX_HOPS,
    OPT_NUM_WORKERS,
    OPT_FOREGROUND,
    OPT_HELP,
    OPT_VERSION
} OptionType;

void print_help(const char *progname);
OptionType get_option_type(const char* arg);
#endif
