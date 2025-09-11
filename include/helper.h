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

#if defined(__linux__)
#define OS_NAME "Linux"
#elif defined(__APPLE__)
#define OS_NAME "macOS"
#elif defined(_WIN32) || defined(_WIN64)
#define OS_NAME "Windows"
#else
#define OS_NAME "Unknown"
#endif

#if defined(__x86_64__)
#define ARCH_NAME "x86_64"
#elif defined(__i386__)
#define ARCH_NAME "i386"
#elif defined(__aarch64__)
#define ARCH_NAME "ARM64"
#elif defined(__arm__)
#define ARCH_NAME "ARM32"
#elif defined(__riscv)
#define ARCH_NAME "RISC-V"
#else
#define ARCH_NAME "Unknown"
#endif

void print_version(void);
void print_help(const char *progname);
OptionType get_option_type(const char* arg);
#endif
