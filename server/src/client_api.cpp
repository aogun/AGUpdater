#include "client_api.h"
#include "version_util.h"
#include "cJSON.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <memory>

/* Check if a filename is safe (no path traversal characters) */
static bool is_safe_filename(const std::string &name)
{
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    return true;
}

/* Sanitize filename for Content-Disposition: only allow safe characters */
static std::string sanitize_filename(const std::string &name)
{
    std::string result;
    result.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            result += c;
        }
    }
    return result;
}

/* X-Auth middleware check - returns true if authorized, sets device_id.
 * Also accepts a valid admin Bearer token to skip HMAC verification,
 * allowing logged-in admin users to call client APIs directly. */
static bool check_client_auth(const httplib::Request &req,
                               httplib::Response &res,
                               AppContext &ctx,
                               std::string &device_id)
{
    /* Check if request carries a valid admin token */
    std::string token = extract_bearer_token(req);
    if (!token.empty() && ctx.admin_auth.verify_token(token)) {
        device_id = "admin";
        LOG_DEBUG("Client auth: admin token accepted for %s %s",
                  req.method.c_str(), req.path.c_str());
        return true;
    }

    /* Fall back to X-Auth HMAC verification */
    std::string xauth;
    if (req.has_header("X-Auth")) {
        xauth = req.get_header_value("X-Auth");
    }

    std::string err_msg;
    if (!ctx.client_auth.verify_xauth(xauth, device_id, err_msg)) {
        LOG_WARN("Client auth failed: %s (path=%s)", err_msg.c_str(), req.path.c_str());
        res.set_content(json_error(2002, err_msg), "application/json");
        res.status = 403;
        return false;
    }
    return true;
}

static std::string version_info_to_json(const VersionRecord &rec)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "version", rec.version.c_str());
    cJSON_AddStringToObject(obj, "description", rec.description.c_str());
    cJSON_AddNumberToObject(obj, "file_size", static_cast<double>(rec.file_size));
    cJSON_AddStringToObject(obj, "file_sha256", rec.file_sha256.c_str());
    cJSON_AddStringToObject(obj, "created_at", rec.created_at.c_str());
    char *str = cJSON_PrintUnformatted(obj);
    std::string result(str);
    free(str);
    cJSON_Delete(obj);
    return result;
}

void register_client_routes(httplib::Server &svr, AppContext &ctx)
{
    /* GET /api/v1/client/updates?current_version=x.y.z */
    svr.Get("/api/v1/client/updates",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            std::string device_id;
            if (!check_client_auth(req, res, ctx, device_id)) return;

            LOG_DEBUG("GET /api/v1/client/updates from %s (device=%s, query=%s)",
                     req.remote_addr.c_str(), device_id.c_str(),
                     req.has_param("current_version") ?
                         req.get_param_value("current_version").c_str() : "");

            if (!req.has_param("current_version")) {
                LOG_WARN("Update check: missing current_version param (device=%s)",
                         device_id.c_str());
                res.set_content(
                    json_error(1001, "missing current_version parameter"),
                    "application/json");
                res.status = 400;
                return;
            }

            std::string current_version = req.get_param_value("current_version");
            if (!semver_validate(current_version)) {
                LOG_WARN("Update check: invalid version format '%s' (device=%s)",
                         current_version.c_str(), device_id.c_str());
                res.set_content(
                    json_error(1001, "invalid version format"),
                    "application/json");
                res.status = 400;
                return;
            }

            /* Get all versions from DB */
            std::vector<VersionRecord> all_versions;
            std::string err;
            if (!ctx.db.get_all_versions(all_versions, err)) {
                LOG_ERROR("Update check: failed to get versions: %s", err.c_str());
                res.set_content(json_error(3001, "internal server error"),
                               "application/json");
                res.status = 500;
                return;
            }

            /* Collect version strings */
            std::vector<std::string> version_strs;
            for (size_t i = 0; i < all_versions.size(); ++i) {
                version_strs.push_back(all_versions[i].version);
            }

            /* Filter newer versions */
            std::vector<std::string> newer = semver_filter_newer(
                version_strs, current_version);

            /* Build updates array */
            cJSON *data = cJSON_CreateObject();
            cJSON_AddBoolToObject(data, "has_update", newer.empty() ? 0 : 1);

            cJSON *updates = cJSON_CreateArray();
            for (size_t i = 0; i < newer.size(); ++i) {
                /* Find the full record */
                for (size_t j = 0; j < all_versions.size(); ++j) {
                    if (all_versions[j].version == newer[i]) {
                        cJSON *item = cJSON_Parse(
                            version_info_to_json(all_versions[j]).c_str());
                        if (item) {
                            cJSON_AddItemToArray(updates, item);
                        }
                        break;
                    }
                }
            }
            cJSON_AddItemToObject(data, "updates", updates);

            char *data_str = cJSON_PrintUnformatted(data);
            std::string data_json(data_str);
            free(data_str);
            cJSON_Delete(data);

            std::string msg = newer.empty() ? "no update available" : "success";
            std::string resp_body = json_response(0, msg, data_json);
            LOG_INFO("Update check: device=%s, current=%s, updates_available=%zu",
                     device_id.c_str(), current_version.c_str(), newer.size());
            LOG_DEBUG("GET /api/v1/client/updates response: %s", resp_body.c_str());
            res.set_content(resp_body, "application/json");
        });

    /* GET /api/v1/client/versions?page=1&page_size=20 */
    svr.Get("/api/v1/client/versions",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            std::string device_id;
            if (!check_client_auth(req, res, ctx, device_id)) return;

            LOG_DEBUG("GET /api/v1/client/versions from %s (device=%s, page=%d, page_size=%d)",
                     req.remote_addr.c_str(), device_id.c_str(),
                     get_query_int(req, "page", 1),
                     get_query_int(req, "page_size", 20));

            int page = get_query_int(req, "page", 1);
            int page_size = get_query_int(req, "page_size", 20);

            VersionPage vpage;
            std::string err;
            if (!ctx.db.get_versions_paged(page, page_size, vpage, err)) {
                LOG_ERROR("Client versions list failed: %s", err.c_str());
                res.set_content(json_error(3001, "internal server error"),
                               "application/json");
                res.status = 500;
                return;
            }

            /* Build response without download_count */
            cJSON *data = cJSON_CreateObject();
            cJSON_AddNumberToObject(data, "total", vpage.paging.total);
            cJSON_AddNumberToObject(data, "page", vpage.paging.page);
            cJSON_AddNumberToObject(data, "page_size", vpage.paging.page_size);

            cJSON *items = cJSON_CreateArray();
            for (size_t i = 0; i < vpage.items.size(); ++i) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", vpage.items[i].id);
                cJSON_AddStringToObject(item, "version",
                                        vpage.items[i].version.c_str());
                cJSON_AddStringToObject(item, "description",
                                        vpage.items[i].description.c_str());
                cJSON_AddStringToObject(item, "file_name",
                                        vpage.items[i].file_name.c_str());
                cJSON_AddNumberToObject(item, "file_size",
                    static_cast<double>(vpage.items[i].file_size));
                cJSON_AddStringToObject(item, "file_sha256",
                                        vpage.items[i].file_sha256.c_str());
                cJSON_AddStringToObject(item, "created_at",
                                        vpage.items[i].created_at.c_str());
                cJSON_AddItemToArray(items, item);
            }
            cJSON_AddItemToObject(data, "items", items);

            char *data_str = cJSON_PrintUnformatted(data);
            std::string data_json(data_str);
            free(data_str);
            cJSON_Delete(data);

            std::string resp_body = json_response(0, "success", data_json);
            LOG_DEBUG("GET /api/v1/client/versions response: %s", resp_body.c_str());
            res.set_content(resp_body, "application/json");
        });

    /* GET /api/v1/client/download/:version */
    svr.Get(R"(/api/v1/client/download/([^/]+))",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            std::string device_id;
            if (!check_client_auth(req, res, ctx, device_id)) return;

            std::string version = req.matches[1].str();
            LOG_DEBUG("GET /api/v1/client/download/%s from %s (device=%s)",
                      version.c_str(), req.remote_addr.c_str(), device_id.c_str());

            /* Look up version in database */
            VersionRecord rec;
            std::string err;
            if (!ctx.db.get_version_by_name(version, rec, err)) {
                LOG_WARN("Download request: version '%s' not found (device=%s)",
                         version.c_str(), device_id.c_str());
                res.set_content(json_error(1003, "version not found"),
                               "application/json");
                res.status = 404;
                return;
            }

            /* Fix 4: Validate file_name has no path traversal */
            if (!is_safe_filename(rec.file_name)) {
                LOG_ERROR("Download request: unsafe file_name '%s' in database for version '%s'",
                          rec.file_name.c_str(), version.c_str());
                res.set_content(json_error(3001, "invalid file name in database"),
                               "application/json");
                res.status = 500;
                return;
            }

            std::string file_path = ctx.config.storage_dir + "/" + rec.file_name;

            /* Fix 13: Open file once and share handle via shared_ptr */
            struct FileCloser {
                void operator()(FILE *fp) const { if (fp) fclose(fp); }
            };
            std::shared_ptr<FILE> file_handle(fopen(file_path.c_str(), "rb"),
                                               FileCloser());
            if (!file_handle) {
                LOG_ERROR("Download request: file not found on disk '%s' for version '%s'",
                          file_path.c_str(), version.c_str());
                res.set_content(json_error(3001, "file not found on server"),
                               "application/json");
                res.status = 500;
                return;
            }

            /* Get file size */
            fseek(file_handle.get(), 0, SEEK_END);
            size_t total_size = static_cast<size_t>(ftell(file_handle.get()));
            fseek(file_handle.get(), 0, SEEK_SET);

            /* Fix 5: Sanitize filename for Content-Disposition header */
            std::string safe_name = sanitize_filename(rec.file_name);
            if (safe_name.empty()) {
                safe_name = "download.zip";
            }
            std::string disposition = "attachment; filename=\"" +
                                       safe_name + "\"";
            res.set_header("Content-Disposition", disposition.c_str());

            LOG_INFO("Download starting: version='%s', file='%s', size=%zu, device=%s, ip=%s",
                     version.c_str(), rec.file_name.c_str(), total_size,
                     device_id.c_str(), req.remote_addr.c_str());

            /* Use content provider for streaming */
            int version_id = rec.id;
            std::string dev_id_copy = device_id;
            std::string ip = req.remote_addr;

            res.set_content_provider(
                total_size,
                "application/octet-stream",
                [file_handle](size_t offset, size_t length, httplib::DataSink &sink) {
                    FILE *fp = file_handle.get();
                    if (!fp) return false;

                    fseek(fp, static_cast<long>(offset), SEEK_SET);
                    const size_t BUF_SIZE = 65536;
                    char buf[BUF_SIZE];
                    size_t remaining = length;
                    while (remaining > 0) {
                        size_t to_read = remaining < BUF_SIZE ? remaining : BUF_SIZE;
                        size_t n = fread(buf, 1, to_read, fp);
                        if (n == 0) break;
                        sink.write(buf, n);
                        remaining -= n;
                    }
                    return true;
                },
                [&ctx, version_id, dev_id_copy, ip](bool success) {
                    if (success) {
                        LOG_INFO("Download completed: version_id=%d, device=%s, ip=%s",
                                 version_id, dev_id_copy.c_str(), ip.c_str());
                        std::string err;
                        ctx.db.increment_download_count(version_id, err);
                        ctx.db.log_download(version_id, dev_id_copy, ip, err);
                    } else {
                        LOG_WARN("Download interrupted: version_id=%d, device=%s, ip=%s",
                                 version_id, dev_id_copy.c_str(), ip.c_str());
                    }
                });
        });
}
