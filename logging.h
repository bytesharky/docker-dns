#ifndef LOGGING_H
#define LOGGING_H

#include <stdarg.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

extern log_level_t log_level;
extern const char *level_str[];

void log_msg(log_level_t level, const char *format, ...);
int get_log_level(const char *env, int default_val);

#endif // LOGGING_H
