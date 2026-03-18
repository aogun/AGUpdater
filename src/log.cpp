#include "log.h"
#include <cstring>

int g_log_level = LOG_LEVEL_INFO;
FILE *g_log_file = NULL;

void log_init_file(const char *path)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    if (path) {
        g_log_file = fopen(path, "w");
    }
}

void log_shutdown_file(void)
{
    FILE *f = g_log_file;
    if (f) {
        g_log_file = NULL;
        fflush(f);
        fclose(f);
    }
}

const char *log_level_name(int level)
{
    switch (level) {
        case LOG_LEVEL_ERROR: return "error";
        case LOG_LEVEL_WARN:  return "warn";
        case LOG_LEVEL_INFO:  return "info";
        case LOG_LEVEL_DEBUG: return "debug";
        case LOG_LEVEL_TRACE: return "trace";
        default:              return "unknown";
    }
}

int log_level_from_name(const char *name)
{
    if (!name) return LOG_LEVEL_INFO;
#ifdef _WIN32
    if (_stricmp(name, "error") == 0) return LOG_LEVEL_ERROR;
    if (_stricmp(name, "warn") == 0)  return LOG_LEVEL_WARN;
    if (_stricmp(name, "info") == 0)  return LOG_LEVEL_INFO;
    if (_stricmp(name, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (_stricmp(name, "trace") == 0) return LOG_LEVEL_TRACE;
#else
    if (strcasecmp(name, "error") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(name, "warn") == 0)  return LOG_LEVEL_WARN;
    if (strcasecmp(name, "info") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(name, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(name, "trace") == 0) return LOG_LEVEL_TRACE;
#endif
    return LOG_LEVEL_INFO;
}
