#ifndef AG_CONFIG_H
#define AG_CONFIG_H

#include <string>

struct TlsConfig {
    std::string cert_file;
    std::string key_file;
};

struct AdminConfig {
    std::string username;
    std::string password_hash;
};

struct ServerConfig {
    std::string host;
    int port;
    TlsConfig tls;
    AdminConfig admin;
    std::string secret;
    std::string storage_dir;
    std::string db_path;
    int max_upload_size_mb;
};

/* Load config from JSON file. Returns false on error, sets err_msg. */
bool config_load(const std::string &path, ServerConfig &out, std::string &err_msg);

/* Apply defaults for missing optional fields */
ServerConfig config_defaults();

#endif /* AG_CONFIG_H */
