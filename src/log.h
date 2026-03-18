#ifndef AG_LOG_H
#define AG_LOG_H

#include <cstdio>
#include <cstring>
#include <ctime>

/*
 * Lightweight logging via printf - no external dependencies
 * Format: [LEVEL] [HH:MM:SS.mmm] [file:line] message
 * Log level is configurable at runtime via g_log_level
 */

enum LogLevel {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_TRACE = 4,
};

/* Runtime log level (default: INFO) */
extern int g_log_level;

/* File logging (NULL = disabled, default) */
extern FILE *g_log_file;
void log_init_file(const char *path);
void log_shutdown_file(void);

/* String <-> level conversion */
const char *log_level_name(int level);
int log_level_from_name(const char *name);

/* Extract filename from __FILE__ (strip directory path) */
#define LOG_FILENAME (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 \
                    : strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 \
                    : __FILE__)

/* Get current time with milliseconds - cross-platform */
#ifdef _WIN32
#include <windows.h>
#define LOG_PRINT(fp, tag, fmt, ...) do { \
    SYSTEMTIME _st; \
    GetLocalTime(&_st); \
    fprintf(fp, "[%s] [%02d:%02d:%02d.%03d] [%s:%d] " fmt "\n", \
            tag, _st.wHour, _st.wMinute, _st.wSecond, _st.wMilliseconds, \
            LOG_FILENAME, __LINE__, ##__VA_ARGS__); \
    if (g_log_file) { \
        fprintf(g_log_file, "[%s] [%02d:%02d:%02d.%03d] [%s:%d] " fmt "\n", \
                tag, _st.wHour, _st.wMinute, _st.wSecond, _st.wMilliseconds, \
                LOG_FILENAME, __LINE__, ##__VA_ARGS__); \
        fflush(g_log_file); \
    } \
} while(0)
#else
#include <sys/time.h>
#define LOG_PRINT(fp, tag, fmt, ...) do { \
    struct timeval _tv; \
    gettimeofday(&_tv, NULL); \
    struct tm _tm; \
    time_t _t = _tv.tv_sec; \
    localtime_r(&_t, &_tm); \
    fprintf(fp, "[%s] [%02d:%02d:%02d.%03d] [%s:%d] " fmt "\n", \
            tag, _tm.tm_hour, _tm.tm_min, _tm.tm_sec, \
            (int)(_tv.tv_usec / 1000), \
            LOG_FILENAME, __LINE__, ##__VA_ARGS__); \
    if (g_log_file) { \
        fprintf(g_log_file, "[%s] [%02d:%02d:%02d.%03d] [%s:%d] " fmt "\n", \
                tag, _tm.tm_hour, _tm.tm_min, _tm.tm_sec, \
                (int)(_tv.tv_usec / 1000), \
                LOG_FILENAME, __LINE__, ##__VA_ARGS__); \
        fflush(g_log_file); \
    } \
} while(0)
#endif

#define LOG_ERROR(fmt, ...) do { if (g_log_level >= LOG_LEVEL_ERROR) LOG_PRINT(stderr, "ERROR", fmt, ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_WARN)  LOG_PRINT(stderr, "WARN ", fmt, ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_INFO)  LOG_PRINT(stdout, "INFO ", fmt, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...) do { if (g_log_level >= LOG_LEVEL_DEBUG) LOG_PRINT(stdout, "DEBUG", fmt, ##__VA_ARGS__); } while(0)
#define LOG_TRACE(fmt, ...) do { if (g_log_level >= LOG_LEVEL_TRACE) LOG_PRINT(stdout, "TRACE", fmt, ##__VA_ARGS__); } while(0)

#endif /* AG_LOG_H */
