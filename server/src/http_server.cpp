#include "http_server.h"
#include "cJSON.h"
#include "log.h"
#include <openssl/sha.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <climits>

std::string json_response(int code, const std::string &message,
                          const std::string &data_json)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message.c_str());
    if (data_json == "null") {
        cJSON_AddNullToObject(root, "data");
    } else {
        cJSON *data = cJSON_Parse(data_json.c_str());
        if (data != NULL) {
            cJSON_AddItemToObject(root, "data", data);
        } else {
            cJSON_AddNullToObject(root, "data");
        }
    }
    char *str = cJSON_PrintUnformatted(root);
    std::string result(str);
    free(str);
    cJSON_Delete(root);
    return result;
}

std::string json_error(int code, const std::string &message)
{
    return json_response(code, message, "null");
}

std::string sha256_data_hex(const char *data, size_t len)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data), len, hash);
    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return std::string(hex, SHA256_DIGEST_LENGTH * 2);
}

std::string extract_bearer_token(const httplib::Request &req)
{
    if (!req.has_header("Authorization")) {
        return std::string();
    }
    std::string auth = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.size() > prefix.size() &&
        auth.compare(0, prefix.size(), prefix) == 0) {
        return auth.substr(prefix.size());
    }
    return std::string();
}

int get_query_int(const httplib::Request &req, const std::string &key, int def)
{
    if (!req.has_param(key)) {
        return def;
    }
    std::string val = req.get_param_value(key);
    char *end = NULL;
    errno = 0;
    long result = strtol(val.c_str(), &end, 10);
    if (end == val.c_str() || errno == ERANGE ||
        result < INT_MIN || result > INT_MAX) {
        return def;
    }

    int int_result = static_cast<int>(result);

    /* Apply range constraints based on parameter name */
    if (key == "page") {
        if (int_result < 1) int_result = 1;
    } else if (key == "page_size") {
        if (int_result < 1) int_result = 1;
        if (int_result > 200) int_result = 200;
    }

    return int_result;
}

static void setup_and_run(httplib::Server &svr, AppContext &ctx)
{
    svr.set_payload_max_length(
        static_cast<size_t>(ctx.config.max_upload_size_mb) * 1024 * 1024);

    svr.set_mount_point("/", "./web");

    register_admin_routes(svr, ctx);
    register_client_routes(svr, ctx);

    LOG_DEBUG("Routes registered, max upload size: %dMB", ctx.config.max_upload_size_mb);
}

bool start_server(AppContext &ctx)
{
    if (ctx.config.tls_enabled) {
        httplib::SSLServer svr(ctx.config.tls.cert_file.c_str(),
                               ctx.config.tls.key_file.c_str());

        if (!svr.is_valid()) {
            LOG_ERROR("Failed to initialize SSL server. "
                      "Check cert_file and key_file paths.");
            return false;
        }

        setup_and_run(svr, ctx);

        LOG_INFO("AGUpdater server starting on https://%s:%d",
                 ctx.config.host.c_str(), ctx.config.port);

        bool ok = svr.listen(ctx.config.host.c_str(), ctx.config.port);
        if (!ok) {
            LOG_ERROR("Server failed to start on %s:%d",
                      ctx.config.host.c_str(), ctx.config.port);
        }
        return ok;
    } else {
        httplib::Server svr;

        setup_and_run(svr, ctx);

        LOG_INFO("AGUpdater server starting on http://%s:%d",
                 ctx.config.host.c_str(), ctx.config.port);

        bool ok = svr.listen(ctx.config.host.c_str(), ctx.config.port);
        if (!ok) {
            LOG_ERROR("Server failed to start on %s:%d",
                      ctx.config.host.c_str(), ctx.config.port);
        }
        return ok;
    }
}
