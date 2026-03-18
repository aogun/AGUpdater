/**
 * ag-server main entry point
 * Reads config, initializes database, starts HTTPS server.
 */
#include "version.h"
#include "config.h"
#include "database.h"
#include "auth.h"
#include "http_server.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char *argv[])
{
    fprintf(stdout, "AGUpdater Server v%s\n", APP_VERSION_STRING);

    /* Determine config file path */
    std::string config_path = "config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    /* Load configuration */
    AppContext ctx;
    std::string err;
    if (!config_load(config_path, ctx.config, err)) {
        fprintf(stderr, "Error loading config: %s\n", err.c_str());
        return 1;
    }
    fprintf(stdout, "Config loaded from %s\n", config_path.c_str());

    /* Initialize database */
    if (!ctx.db.open(ctx.config.db_path, err)) {
        fprintf(stderr, "Error opening database: %s\n", err.c_str());
        return 1;
    }
    fprintf(stdout, "Database opened: %s\n", ctx.config.db_path.c_str());

    /* Initialize authentication */
    ctx.admin_auth.set_password_hash(ctx.config.admin.password_hash);
    ctx.client_auth.set_secret(ctx.config.secret);

    /* Start server */
    if (!start_server(ctx)) {
        fprintf(stderr, "Server exited with error\n");
        return 1;
    }

    return 0;
}
