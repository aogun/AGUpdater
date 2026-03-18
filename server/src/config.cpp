#include "config.h"
#include "cJSON.h"
#include <cstdio>
#include <cstdlib>
#include <string>

ServerConfig config_defaults()
{
    ServerConfig cfg;
    cfg.host = "0.0.0.0";
    cfg.port = 8443;
    cfg.tls.cert_file = "server.crt";
    cfg.tls.key_file = "server.key";
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
        return std::string();
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fclose(f);
        return std::string();
    }

    std::string buf(static_cast<size_t>(len), '\0');
    size_t read_n = fread(&buf[0], 1, static_cast<size_t>(len), f);
    fclose(f);
    buf.resize(read_n);
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
    std::string content = read_file_contents(path);
    if (content.empty()) {
        err_msg = "failed to read config file: " + path;
        return false;
    }

    cJSON *root = cJSON_Parse(content.c_str());
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        err_msg = "JSON parse error";
        if (error_ptr != NULL) {
            err_msg += std::string(": ") + error_ptr;
        }
        return false;
    }

    ServerConfig defaults = config_defaults();
    out = defaults;

    out.host = json_get_string(root, "host", defaults.host);
    out.port = json_get_int(root, "port", defaults.port);

    const cJSON *tls_obj = cJSON_GetObjectItemCaseSensitive(root, "tls");
    if (cJSON_IsObject(tls_obj)) {
        out.tls.cert_file = json_get_string(tls_obj, "cert_file",
                                            defaults.tls.cert_file);
        out.tls.key_file = json_get_string(tls_obj, "key_file",
                                           defaults.tls.key_file);
    }

    const cJSON *admin_obj = cJSON_GetObjectItemCaseSensitive(root, "admin");
    if (cJSON_IsObject(admin_obj)) {
        out.admin.username = json_get_string(admin_obj, "username",
                                             defaults.admin.username);
        out.admin.password_hash = json_get_string(admin_obj, "password_hash",
                                                  defaults.admin.password_hash);
    }

    out.secret = json_get_string(root, "secret", defaults.secret);
    out.storage_dir = json_get_string(root, "storage_dir", defaults.storage_dir);
    out.db_path = json_get_string(root, "db_path", defaults.db_path);
    out.max_upload_size_mb = json_get_int(root, "max_upload_size_mb",
                                          defaults.max_upload_size_mb);

    /* Validate required fields */
    if (out.admin.password_hash.empty()) {
        cJSON_Delete(root);
        err_msg = "admin.password_hash is required";
        return false;
    }
    if (out.secret.empty()) {
        cJSON_Delete(root);
        err_msg = "secret is required";
        return false;
    }

    cJSON_Delete(root);
    return true;
}
