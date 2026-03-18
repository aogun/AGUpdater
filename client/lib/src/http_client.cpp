#include "http_client.h"
#include "auth.h"
#include "log.h"

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "httplib.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef AG_SERVER_URL
#define AG_SERVER_URL "https://localhost:8443"
#endif

/* Parse host and port from AG_SERVER_URL */
static bool parse_server_url(std::string &host, int &port)
{
    std::string url = AG_SERVER_URL;
    LOG_DEBUG("parse_server_url: parsing %s", url.c_str());

    /* Strip scheme */
    std::string scheme;
    size_t scheme_end = url.find("://");
    if (scheme_end != std::string::npos) {
        scheme = url.substr(0, scheme_end);
        url = url.substr(scheme_end + 3);
    }

    /* Find port */
    size_t colon = url.rfind(':');
    if (colon != std::string::npos) {
        host = url.substr(0, colon);
        port = atoi(url.substr(colon + 1).c_str());
    } else {
        host = url;
        port = (scheme == "https") ? 443 : 80;
    }

    LOG_DEBUG("parse_server_url: host=%s, port=%d", host.c_str(), port);
    return !host.empty() && port > 0;
}

static httplib::SSLClient *create_client()
{
    std::string host;
    int port;
    if (!parse_server_url(host, port)) {
        LOG_ERROR("create_client: failed to parse server URL");
        return NULL;
    }

    LOG_DEBUG("create_client: connecting to %s:%d", host.c_str(), port);
    httplib::SSLClient *cli = new httplib::SSLClient(host, port);
    /* SSL certificate verification is enabled by default.
     * For development/debugging, temporarily set to false:
     *   cli->enable_server_certificate_verification(false); */
    cli->set_connection_timeout(10, 0);
    cli->set_read_timeout(30, 0);
    return cli;
}

HttpResult http_get(const std::string &path)
{
    HttpResult result;
    result.status_code = 0;
    result.ok = false;

    httplib::SSLClient *cli = create_client();
    if (cli == NULL) {
        result.error = "failed to create HTTP client";
        LOG_ERROR("http_get: failed to create HTTP client");
        return result;
    }

    /* Generate X-Auth header */
    std::string xauth = ag_generate_xauth();

    httplib::Headers headers;
    headers.emplace("X-Auth", xauth);

    LOG_TRACE("http_get: sending GET %s", path.c_str());
    auto res = cli->Get(path.c_str(), headers);
    delete cli;

    if (!res) {
        result.error = "network error";
        LOG_WARN("http_get: network error for %s", path.c_str());
        return result;
    }

    result.status_code = res->status;
    result.body = res->body;
    result.ok = true;
    LOG_DEBUG("http_get: status=%d, body_size=%zu", res->status, res->body.size());
    return result;
}

bool http_download(const std::string &path,
                   const std::string &dest_file,
                   int64_t expected_size,
                   ProgressCallback progress_cb,
                   std::string &err_msg)
{
    LOG_DEBUG("http_download: path=%s, dest=%s, expected_size=%lld",
              path.c_str(), dest_file.c_str(), (long long)expected_size);

    httplib::SSLClient *cli = create_client();
    if (cli == NULL) {
        err_msg = "failed to create HTTP client";
        LOG_ERROR("http_download: failed to create HTTP client");
        return false;
    }

    /* Longer timeout for downloads */
    cli->set_read_timeout(300, 0);

    std::string xauth = ag_generate_xauth();
    httplib::Headers headers;
    headers.emplace("X-Auth", xauth);

    FILE *out = fopen(dest_file.c_str(), "wb");
    if (!out) {
        delete cli;
        err_msg = "cannot create output file: " + dest_file;
        LOG_ERROR("http_download: cannot create output file: %s", dest_file.c_str());
        return false;
    }

    int64_t downloaded = 0;
    bool write_error = false;
    auto last_cb_time = std::chrono::steady_clock::now();

    auto res = cli->Get(path.c_str(), headers,
        [&](const char *data, size_t data_length) {
            if (fwrite(data, 1, data_length, out) != data_length) {
                write_error = true;
                return false;
            }
            downloaded += static_cast<int64_t>(data_length);
            LOG_TRACE("http_download: received %lld / %lld bytes",
                      (long long)downloaded, (long long)expected_size);
            if (progress_cb) {
                /* Throttle progress callbacks to at most once per 500ms
                 * to avoid flooding the UI thread with messages */
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds>(now - last_cb_time).count();
                if (elapsed >= 500 || downloaded >= expected_size) {
                    last_cb_time = now;
                    return progress_cb(downloaded, expected_size);
                }
            }
            return true;
        });

    fclose(out);
    delete cli;

    if (write_error) {
        remove(dest_file.c_str());
        err_msg = "write error";
        LOG_ERROR("http_download: write error to %s", dest_file.c_str());
        return false;
    }

    if (!res) {
        remove(dest_file.c_str());
        err_msg = "network error during download";
        LOG_ERROR("http_download: network error during download");
        return false;
    }

    if (res->status != 200) {
        remove(dest_file.c_str());
        err_msg = "server returned status " + std::to_string(res->status);
        LOG_ERROR("http_download: server returned status %d", res->status);
        return false;
    }

    LOG_DEBUG("http_download: download complete, %lld bytes", (long long)downloaded);
    return true;
}
