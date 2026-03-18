#include "client_api.h"
#include "version_util.h"
#include "cJSON.h"

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

/* X-Auth middleware check - returns true if authorized, sets device_id */
static bool check_client_auth(const httplib::Request &req,
                               httplib::Response &res,
                               AppContext &ctx,
                               std::string &device_id)
{
    std::string xauth;
    if (req.has_header("X-Auth")) {
        xauth = req.get_header_value("X-Auth");
    }

    std::string err_msg;
    if (!ctx.client_auth.verify_xauth(xauth, device_id, err_msg)) {
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

            if (!req.has_param("current_version")) {
                res.set_content(
                    json_error(1001, "missing current_version parameter"),
                    "application/json");
                res.status = 400;
                return;
            }

            std::string current_version = req.get_param_value("current_version");
            if (!semver_validate(current_version)) {
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
            res.set_content(json_response(0, msg, data_json),
                           "application/json");
        });

    /* GET /api/v1/client/versions?page=1&page_size=20 */
    svr.Get("/api/v1/client/versions",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            std::string device_id;
            if (!check_client_auth(req, res, ctx, device_id)) return;

            int page = get_query_int(req, "page", 1);
            int page_size = get_query_int(req, "page_size", 20);

            VersionPage vpage;
            std::string err;
            if (!ctx.db.get_versions_paged(page, page_size, vpage, err)) {
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

            res.set_content(json_response(0, "success", data_json),
                           "application/json");
        });

    /* GET /api/v1/client/download/:version */
    svr.Get(R"(/api/v1/client/download/([^/]+))",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            std::string device_id;
            if (!check_client_auth(req, res, ctx, device_id)) return;

            std::string version = req.matches[1].str();

            /* Look up version in database */
            VersionRecord rec;
            std::string err;
            if (!ctx.db.get_version_by_name(version, rec, err)) {
                res.set_content(json_error(1003, "version not found"),
                               "application/json");
                res.status = 404;
                return;
            }

            /* Fix 4: Validate file_name has no path traversal */
            if (!is_safe_filename(rec.file_name)) {
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
                        std::string err;
                        ctx.db.increment_download_count(version_id, err);
                        ctx.db.log_download(version_id, dev_id_copy, ip, err);
                    }
                });
        });
}
