/**
 * ag-server main entry point
 * Reads config, initializes database, starts HTTPS server.
 */
#include "version.h"
#include "config.h"
#include "database.h"
#include "auth.h"
#include "http_server.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static void print_usage(const char *prog)
{
    fprintf(stdout,
        "Usage: %s [options] [config_path]\n"
        "Options:\n"
        "  -p <password>  Generate password_hash for config.json\n"
        "  -l <level>     Set log level (error/warn/info/debug/trace)\n"
        "  -h             Show this help\n"
        "\n"
        "config_path defaults to config.json\n", prog);
}

int main(int argc, char *argv[])
{
    /* Parse command line arguments */
    std::string config_path = "config.json";
    std::string log_level_str;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) {
            /* Tool mode: generate password_hash */
            std::string hash = sha256_hex(argv[i + 1]);
            fprintf(stdout, "password_hash: %s\n", hash.c_str());
            return 0;
        } else if (arg == "-l" && i + 1 < argc) {
            log_level_str = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg[0] != '-') {
            config_path = arg;
        }
    }

    /* Apply log level if specified */
    if (!log_level_str.empty()) {
        g_log_level = log_level_from_name(log_level_str.c_str());
    }

    /* Initialize file logging */
    log_init_file("ag-server.log");

    LOG_INFO("AGUpdater Server v%s", APP_VERSION_STRING);
    LOG_INFO("Log level: %s", log_level_name(g_log_level));

    /* Load configuration */
    AppContext ctx;
    std::string err;
    if (!config_load(config_path, ctx.config, err)) {
        LOG_ERROR("Error loading config: %s", err.c_str());
        log_shutdown_file();
        return 1;
    }
    LOG_INFO("Config loaded from %s", config_path.c_str());

    /* Initialize database */
    if (!ctx.db.open(ctx.config.db_path, err)) {
        LOG_ERROR("Error opening database: %s", err.c_str());
        log_shutdown_file();
        return 1;
    }
    LOG_INFO("Database opened: %s", ctx.config.db_path.c_str());

    /* Initialize authentication */
    ctx.admin_auth.set_password_hash(ctx.config.admin.password_hash);
    ctx.client_auth.set_secret(ctx.config.secret);
    LOG_DEBUG("Authentication modules initialized");

    /* Start server */
    if (!start_server(ctx)) {
        LOG_ERROR("Server exited with error");
        log_shutdown_file();
        return 1;
    }

    log_shutdown_file();
    return 0;
}
