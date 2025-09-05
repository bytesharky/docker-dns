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
    timestr[strlen(timestr) - 1] = '\0';  // 去掉换行

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

int get_log_level(const char *env, int default_val) {
    char *val = getenv(env);
    if (!val) return default_val;

    if (strcasecmp(val, "DEBUG") == 0) return LOG_DEBUG;
    if (strcasecmp(val, "INFO") == 0)  return LOG_INFO;
    if (strcasecmp(val, "WARN") == 0)  return LOG_WARN;
    if (strcasecmp(val, "ERROR") == 0) return LOG_ERROR;
    if (strcasecmp(val, "FATAL") == 0) return LOG_FATAL;

    return default_val;
}
