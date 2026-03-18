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

int main(int argc, char *argv[])
{
    LOG_INFO("AGUpdater Server v%s", APP_VERSION_STRING);

    /* Tool mode: -p <password> generates password_hash for config.json */
    if (argc >= 3 && std::string(argv[1]) == "-p") {
        std::string hash = sha256_hex(argv[2]);
        LOG_INFO("password_hash: %s", hash.c_str());
        return 0;
    }

    /* Determine config file path */
    std::string config_path = "config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    /* Initialize file logging */
    log_init_file("ag-server.log");
    LOG_INFO("File logging initialized: ag-server.log");

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
