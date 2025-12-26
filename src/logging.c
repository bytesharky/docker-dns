
#include "config.h"   // for str2int
#include "logging.h"
#include <stdarg.h>   // for va_end, va_start
#include <stdio.h>    // for printf, fflush, vprintf, NULL, stdout
#include <stdlib.h>   // for free
#include <string.h>   // for strlen
#include <strings.h>  // for strcasecmp
#include <time.h>     // for ctime, time, time_t
#include <pthread.h>  // for pthread_mutex_lock

log_level_t log_level = LOG_INFO;
const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
// 定义全局互斥锁，保护日志输出
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// 输出日志
void vlog_msg(log_level_t level, const char *format, va_list args) {
    if (level < log_level) return;

    // 加互斥锁，确保整个日志打印过程是原子操作
    pthread_mutex_lock(&log_mutex);
    time_t now;
    time(&now);
    char *timestr = ctime(&now);
    timestr[strlen(timestr) - 1] = '\0';

    printf("[%s] %-5s ", timestr, level_str[level]);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    // 释放互斥锁，让其他线程可以打印日志
    pthread_mutex_unlock(&log_mutex);
}

// 输出日志
void log_msg(log_level_t level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vlog_msg(level, format, args);
    va_end(args);
}

// 将字符转为日志级别
int parse_log_level(const char *level_str, int default_val) {
    if (level_str == NULL) return default_val;    

    if (strcasecmp(level_str, "DEBUG") == 0) return LOG_DEBUG;
    if (strcasecmp(level_str, "INFO")  == 0) return LOG_INFO;
    if (strcasecmp(level_str, "WARN")  == 0) return LOG_WARN;
    if (strcasecmp(level_str, "ERROR") == 0) return LOG_ERROR;
    if (strcasecmp(level_str, "FATAL") == 0) return LOG_FATAL;

    int *intptr = str2int(level_str);
    
    if (intptr != NULL) {
        int conv_level = (int)*intptr;
        free(intptr);
        if (conv_level >= LOG_DEBUG && 
            conv_level <= LOG_FATAL) return (int)conv_level;
    }

    return default_val;
}

// 程序退出前建议销毁锁
void log_cleanup() {
    pthread_mutex_destroy(&log_mutex);
}