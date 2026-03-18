#ifndef AG_HTTP_SERVER_H
#define AG_HTTP_SERVER_H

#include "config.h"
#include "database.h"
#include "auth.h"

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "httplib.h"

/* Shared application context accessible by all route handlers */
struct AppContext {
    ServerConfig config;
    Database db;
    AdminAuth admin_auth;
    ClientAuth client_auth;
};

/* JSON response helpers */
std::string json_response(int code, const std::string &message,
                          const std::string &data_json = "null");
std::string json_error(int code, const std::string &message);

/* Compute SHA256 of a data buffer, return hex string */
std::string sha256_data_hex(const char *data, size_t len);

/* Extract Bearer token from Authorization header. Returns empty if not found. */
std::string extract_bearer_token(const httplib::Request &req);

/* Get query parameter as int, with default */
int get_query_int(const httplib::Request &req, const std::string &key, int def);

/* Register all routes and start server */
void register_admin_routes(httplib::Server &svr, AppContext &ctx);
void register_client_routes(httplib::Server &svr, AppContext &ctx);

bool start_server(AppContext &ctx);

#endif /* AG_HTTP_SERVER_H */
