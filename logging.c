#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

log_level_t log_level = LOG_INFO;
const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

static void vlog(log_level_t level, const char *format, va_list args) {
    if (level < log_level) return;

    time_t now;
    time(&now);
    char *timestr = ctime(&now);
    timestr[strlen(timestr) - 1] = '\0';

    printf("[%s] %-5s ", timestr, level_str[level]);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
}

void log_msg(log_level_t level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vlog(level, format, args);
    va_end(args);
}

int isAllDigits(const char *str) {
    if (str == NULL || *str == '\0') return 0;
    
    while (*str != '\0') {
        if (*str < '0' || *str > '9') return 0;
        str++;
    }
    
    return 1;
}

int parse_log_level(const char *level_str, int default_val) {
    if (level_str == NULL) return -1;    

    if (strcasecmp(level_str, "DEBUG") == 0) return LOG_DEBUG;
    if (strcasecmp(level_str, "INFO")  == 0) return LOG_INFO;
    if (strcasecmp(level_str, "WARN")  == 0) return LOG_WARN;
    if (strcasecmp(level_str, "ERROR") == 0) return LOG_ERROR;
    if (strcasecmp(level_str, "FATAL") == 0) return LOG_FATAL;

    if (isAllDigits(level_str)) {
        char *endptr;
        long conv_level = strtol(level_str, &endptr, 10);

        if (*endptr == '\0' 
            && conv_level >= LOG_DEBUG 
            && conv_level <= LOG_FATAL) {
            return (int)conv_level;
        }
    }

    return default_val;
}

int get_log_level(const char *env, int default_val) {
    char *val = getenv(env);
    if (!val) return default_val;
    return parse_log_level(val, default_val);
}
