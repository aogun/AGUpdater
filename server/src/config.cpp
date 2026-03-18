#include "config.h"
#include "cJSON.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <string>

ServerConfig config_defaults()
{
    ServerConfig cfg;
    cfg.host = "0.0.0.0";
    cfg.port = 8443;
    cfg.tls_enabled = false;
    cfg.tls.cert_file = "";
    cfg.tls.key_file = "";
    cfg.admin.username = "admin";
    cfg.admin.password_hash = "";
    cfg.secret = "";
    cfg.storage_dir = "./packages";
    cfg.db_path = "./agupdate.db";
    cfg.max_upload_size_mb = 100;
    return cfg;
}

static std::string read_file_contents(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        LOG_ERROR("Failed to open file: %s", path.c_str());
        return std::string();
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        LOG_WARN("File is empty or unreadable: %s", path.c_str());
        fclose(f);
        return std::string();
    }

    std::string buf(static_cast<size_t>(len), '\0');
    size_t read_n = fread(&buf[0], 1, static_cast<size_t>(len), f);
    fclose(f);
    buf.resize(read_n);
    LOG_DEBUG("Read %zu bytes from %s", read_n, path.c_str());
    return buf;
}

static std::string json_get_string(const cJSON *obj, const char *key,
                                   const std::string &def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return std::string(item->valuestring);
    }
    return def;
}

static int json_get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return def;
}

bool config_load(const std::string &path, ServerConfig &out, std::string &err_msg)
{
    LOG_TRACE("config_load() enter, path=%s", path.c_str());

    std::string content = read_file_contents(path);
    if (content.empty()) {
        err_msg = "failed to read config file: " + path;
        LOG_ERROR("Failed to read config file: %s", path.c_str());
        return false;
    }

    cJSON *root = cJSON_Parse(content.c_str());
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        err_msg = "JSON parse error";
        if (error_ptr != NULL) {
            err_msg += std::string(": ") + error_ptr;
        }
        LOG_ERROR("Config JSON parse error: %s", err_msg.c_str());
        return false;
    }

    ServerConfig defaults = config_defaults();
    out = defaults;

    out.host = json_get_string(root, "host", defaults.host);
    out.port = json_get_int(root, "port", defaults.port);
    LOG_DEBUG("Config: host=%s, port=%d", out.host.c_str(), out.port);

    const cJSON *tls_obj = cJSON_GetObjectItemCaseSensitive(root, "tls");
    if (cJSON_IsObject(tls_obj)) {
        out.tls.cert_file = json_get_string(tls_obj, "cert_file", "");
        out.tls.key_file = json_get_string(tls_obj, "key_file", "");
        out.tls_enabled = !out.tls.cert_file.empty() &&
                          !out.tls.key_file.empty();
        LOG_DEBUG("Config: tls_enabled=%d, cert=%s, key=%s",
                  out.tls_enabled, out.tls.cert_file.c_str(),
                  out.tls.key_file.c_str());
    } else {
        out.tls_enabled = false;
        LOG_DEBUG("Config: TLS not configured");
    }

    const cJSON *admin_obj = cJSON_GetObjectItemCaseSensitive(root, "admin");
    if (cJSON_IsObject(admin_obj)) {
        out.admin.username = json_get_string(admin_obj, "username",
                                             defaults.admin.username);
        out.admin.password_hash = json_get_string(admin_obj, "password_hash",
                                                  defaults.admin.password_hash);
        LOG_DEBUG("Config: admin.username=%s", out.admin.username.c_str());
    }

    out.secret = json_get_string(root, "secret", defaults.secret);
    out.storage_dir = json_get_string(root, "storage_dir", defaults.storage_dir);
    out.db_path = json_get_string(root, "db_path", defaults.db_path);
    out.max_upload_size_mb = json_get_int(root, "max_upload_size_mb",
                                          defaults.max_upload_size_mb);
    LOG_DEBUG("Config: storage_dir=%s, db_path=%s, max_upload=%dMB",
              out.storage_dir.c_str(), out.db_path.c_str(),
              out.max_upload_size_mb);

    /* Validate required fields */
    if (out.admin.password_hash.empty()) {
        cJSON_Delete(root);
        err_msg = "admin.password_hash is required";
        LOG_ERROR("Config validation failed: %s", err_msg.c_str());
        return false;
    }
    if (out.secret.empty()) {
        cJSON_Delete(root);
        err_msg = "secret is required";
        LOG_ERROR("Config validation failed: %s", err_msg.c_str());
        return false;
    }

    cJSON_Delete(root);
    LOG_INFO("Configuration loaded successfully from %s", path.c_str());
    LOG_TRACE("config_load() exit");
    return true;
}
