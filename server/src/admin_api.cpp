#include "admin_api.h"
#include "version_util.h"
#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

/* Zip magic number: PK\x03\x04 */
static bool is_zip_file(const std::string &data)
{
    return data.size() >= 4 &&
           data[0] == 'P' && data[1] == 'K' &&
           data[2] == '\x03' && data[3] == '\x04';
}

static bool ensure_directory(const std::string &path)
{
#if defined(_WIN32) && !defined(__MINGW32__)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

static bool write_file(const std::string &path, const char *data, size_t len)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return written == len;
}

static bool remove_file(const std::string &path)
{
    return remove(path.c_str()) == 0;
}

static std::string version_record_to_json(const VersionRecord &rec,
                                           bool include_download_count)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", rec.id);
    cJSON_AddStringToObject(obj, "version", rec.version.c_str());
    cJSON_AddStringToObject(obj, "description", rec.description.c_str());
    cJSON_AddStringToObject(obj, "file_name", rec.file_name.c_str());
    cJSON_AddNumberToObject(obj, "file_size", static_cast<double>(rec.file_size));
    cJSON_AddStringToObject(obj, "file_sha256", rec.file_sha256.c_str());
    if (include_download_count) {
        cJSON_AddNumberToObject(obj, "download_count", rec.download_count);
    }
    cJSON_AddStringToObject(obj, "created_at", rec.created_at.c_str());
    char *str = cJSON_PrintUnformatted(obj);
    std::string result(str);
    free(str);
    cJSON_Delete(obj);
    return result;
}

static std::string version_page_to_json(const VersionPage &page,
                                         bool include_download_count)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "total", page.paging.total);
    cJSON_AddNumberToObject(obj, "page", page.paging.page);
    cJSON_AddNumberToObject(obj, "page_size", page.paging.page_size);

    cJSON *items = cJSON_CreateArray();
    for (size_t i = 0; i < page.items.size(); ++i) {
        cJSON *item_json = cJSON_Parse(
            version_record_to_json(page.items[i], include_download_count).c_str());
        if (item_json) {
            cJSON_AddItemToArray(items, item_json);
        }
    }
    cJSON_AddItemToObject(obj, "items", items);

    char *str = cJSON_PrintUnformatted(obj);
    std::string result(str);
    free(str);
    cJSON_Delete(obj);
    return result;
}

/* Token middleware check - returns true if authorized */
static bool check_admin_token(const httplib::Request &req,
                               httplib::Response &res,
                               AppContext &ctx)
{
    std::string token = extract_bearer_token(req);
    if (token.empty() || !ctx.admin_auth.verify_token(token)) {
        res.set_content(json_error(2001, "unauthorized"), "application/json");
        res.status = 401;
        return false;
    }
    return true;
}

void register_admin_routes(httplib::Server &svr, AppContext &ctx)
{
    /* GET /api/v1/admin/challenge */
    svr.Get("/api/v1/admin/challenge",
        [&ctx](const httplib::Request &, httplib::Response &res) {
            std::string nonce = ctx.admin_auth.generate_nonce();

            cJSON *data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "nonce", nonce.c_str());
            char *data_str = cJSON_PrintUnformatted(data);
            std::string data_json(data_str);
            free(data_str);
            cJSON_Delete(data);

            res.set_content(json_response(0, "success", data_json),
                           "application/json");
        });

    /* POST /api/v1/admin/login */
    svr.Post("/api/v1/admin/login",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            cJSON *body = cJSON_Parse(req.body.c_str());
            if (!body) {
                res.set_content(json_error(1001, "invalid JSON"),
                               "application/json");
                res.status = 400;
                return;
            }

            const cJSON *j_user = cJSON_GetObjectItemCaseSensitive(body, "username");
            const cJSON *j_nonce = cJSON_GetObjectItemCaseSensitive(body, "nonce");
            const cJSON *j_sign = cJSON_GetObjectItemCaseSensitive(body, "sign");

            if (!cJSON_IsString(j_user) || !cJSON_IsString(j_nonce) ||
                !cJSON_IsString(j_sign)) {
                cJSON_Delete(body);
                res.set_content(json_error(1001, "missing required fields"),
                               "application/json");
                res.status = 400;
                return;
            }

            std::string username = j_user->valuestring;
            std::string nonce = j_nonce->valuestring;
            std::string sign = j_sign->valuestring;
            cJSON_Delete(body);

            std::string token = ctx.admin_auth.verify_login(
                username, nonce, sign, ctx.config.admin.username);

            if (token.empty()) {
                res.set_content(json_error(2001, "login failed"),
                               "application/json");
                res.status = 401;
                return;
            }

            cJSON *data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "token", token.c_str());
            char *data_str = cJSON_PrintUnformatted(data);
            std::string data_json(data_str);
            free(data_str);
            cJSON_Delete(data);

            res.set_content(json_response(0, "success", data_json),
                           "application/json");
        });

    /* POST /api/v1/admin/versions — upload new version (multipart) */
    svr.Post("/api/v1/admin/versions",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            if (!check_admin_token(req, res, ctx)) return;

            /* Extract multipart fields */
            if (!req.has_file("version") || !req.has_file("description") ||
                !req.has_file("file")) {
                res.set_content(
                    json_error(1001, "missing version, description, or file"),
                    "application/json");
                res.status = 400;
                return;
            }

            std::string version = req.get_file_value("version").content;
            std::string description = req.get_file_value("description").content;
            const httplib::MultipartFormData &file_data =
                req.get_file_value("file");

            /* Validate version format */
            if (!semver_validate(version)) {
                res.set_content(json_error(1001, "invalid version format"),
                               "application/json");
                res.status = 400;
                return;
            }

            /* Check version doesn't already exist */
            VersionRecord existing;
            std::string err;
            if (ctx.db.get_version_by_name(version, existing, err)) {
                res.set_content(json_error(1002, "version already exists"),
                               "application/json");
                res.status = 409;
                return;
            }

            /* Validate zip magic number */
            if (!is_zip_file(file_data.content)) {
                res.set_content(json_error(1004, "file is not a valid zip"),
                               "application/json");
                res.status = 400;
                return;
            }

            /* Validate file size */
            int64_t max_bytes = static_cast<int64_t>(ctx.config.max_upload_size_mb)
                                * 1024 * 1024;
            if (static_cast<int64_t>(file_data.content.size()) > max_bytes) {
                res.set_content(json_error(1005, "file size exceeds limit"),
                               "application/json");
                res.status = 413;
                return;
            }

            /* Compute SHA256 */
            std::string file_sha256 = sha256_data_hex(
                file_data.content.c_str(), file_data.content.size());

            /* Save file to storage_dir/<version>.zip */
            ensure_directory(ctx.config.storage_dir);
            std::string file_name = version + ".zip";
            std::string file_path = ctx.config.storage_dir + "/" + file_name;

            if (!write_file(file_path, file_data.content.c_str(),
                           file_data.content.size())) {
                res.set_content(json_error(3001, "failed to save file"),
                               "application/json");
                res.status = 500;
                return;
            }

            /* Insert into database */
            int new_id = 0;
            std::string db_err;
            if (!ctx.db.insert_version(version, description, file_name,
                                       static_cast<int64_t>(file_data.content.size()),
                                       file_sha256, new_id, db_err)) {
                remove_file(file_path);
                /* Check if duplicate */
                if (db_err.find("UNIQUE") != std::string::npos) {
                    res.set_content(json_error(1002, "version already exists"),
                                   "application/json");
                    res.status = 409;
                } else {
                    res.set_content(json_error(3001, "internal server error"),
                                   "application/json");
                    res.status = 500;
                }
                return;
            }

            /* Build response */
            cJSON *data = cJSON_CreateObject();
            cJSON_AddNumberToObject(data, "id", new_id);
            cJSON_AddStringToObject(data, "version", version.c_str());
            cJSON_AddStringToObject(data, "file_sha256", file_sha256.c_str());
            char *data_str = cJSON_PrintUnformatted(data);
            std::string data_json(data_str);
            free(data_str);
            cJSON_Delete(data);

            res.set_content(json_response(0, "success", data_json),
                           "application/json");
            res.status = 201;
        });

    /* GET /api/v1/admin/versions — list versions (paged) */
    svr.Get("/api/v1/admin/versions",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            if (!check_admin_token(req, res, ctx)) return;

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

            std::string data_json = version_page_to_json(vpage, true);
            res.set_content(json_response(0, "success", data_json),
                           "application/json");
        });

    /* PUT /api/v1/admin/versions/:version — update version description */
    svr.Put(R"(/api/v1/admin/versions/([^/]+))",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            if (!check_admin_token(req, res, ctx)) return;

            std::string version = req.matches[1].str();

            /* Validate version format */
            if (!semver_validate(version)) {
                res.set_content(json_error(1001, "invalid version format"),
                               "application/json");
                res.status = 400;
                return;
            }

            cJSON *body = cJSON_Parse(req.body.c_str());
            if (!body) {
                res.set_content(json_error(1001, "invalid JSON"),
                               "application/json");
                res.status = 400;
                return;
            }

            const cJSON *j_desc = cJSON_GetObjectItemCaseSensitive(body,
                                                                     "description");
            if (!cJSON_IsString(j_desc)) {
                cJSON_Delete(body);
                res.set_content(json_error(1001, "missing description"),
                               "application/json");
                res.status = 400;
                return;
            }

            std::string description = j_desc->valuestring;
            cJSON_Delete(body);

            std::string err;
            if (!ctx.db.update_version_description(version, description, err)) {
                if (err.find("not found") != std::string::npos) {
                    res.set_content(json_error(1003, "version not found"),
                                   "application/json");
                    res.status = 404;
                } else {
                    res.set_content(json_error(3001, "internal server error"),
                                   "application/json");
                    res.status = 500;
                }
                return;
            }

            res.set_content(json_response(0, "success"), "application/json");
        });

    /* DELETE /api/v1/admin/versions — batch delete (JSON body) */
    svr.Delete("/api/v1/admin/versions",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            if (!check_admin_token(req, res, ctx)) return;

            cJSON *body = cJSON_Parse(req.body.c_str());
            if (!body) {
                res.set_content(json_error(1001, "invalid JSON"),
                               "application/json");
                res.status = 400;
                return;
            }

            const cJSON *j_versions = cJSON_GetObjectItemCaseSensitive(body,
                                                                        "versions");
            if (!cJSON_IsArray(j_versions)) {
                cJSON_Delete(body);
                res.set_content(json_error(1001, "missing versions array"),
                               "application/json");
                res.status = 400;
                return;
            }

            /* Collect and validate version strings */
            std::vector<std::string> versions;
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, j_versions) {
                if (cJSON_IsString(item)) {
                    std::string ver = item->valuestring;
                    if (!semver_validate(ver)) {
                        cJSON_Delete(body);
                        res.set_content(json_error(1001, "invalid version format in array"),
                                       "application/json");
                        res.status = 400;
                        return;
                    }
                    versions.push_back(ver);
                }
            }
            cJSON_Delete(body);

            /* Delete from database first, then remove files */
            std::string err;
            if (!ctx.db.delete_versions_batch(versions, err)) {
                res.set_content(json_error(3001, "internal server error"),
                               "application/json");
                res.status = 500;
                return;
            }

            /* Delete storage files after successful DB deletion */
            for (size_t i = 0; i < versions.size(); ++i) {
                std::string fpath = ctx.config.storage_dir + "/" +
                                    versions[i] + ".zip";
                remove_file(fpath);
            }

            res.set_content(json_response(0, "success"), "application/json");
        });

    /* DELETE /api/v1/admin/versions/:version — delete single version */
    svr.Delete(R"(/api/v1/admin/versions/([^/]+))",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            if (!check_admin_token(req, res, ctx)) return;

            std::string version = req.matches[1].str();

            /* Validate version format to prevent path traversal */
            if (!semver_validate(version)) {
                res.set_content(json_error(1001, "invalid version format"),
                               "application/json");
                res.status = 400;
                return;
            }

            /* Delete from database first */
            std::string err;
            if (!ctx.db.delete_version(version, err)) {
                if (err.find("not found") != std::string::npos) {
                    res.set_content(json_error(1003, "version not found"),
                                   "application/json");
                    res.status = 404;
                } else {
                    res.set_content(json_error(3001, "internal server error"),
                                   "application/json");
                    res.status = 500;
                }
                return;
            }

            /* Delete storage file after successful DB deletion */
            std::string fpath = ctx.config.storage_dir + "/" + version + ".zip";
            remove_file(fpath);

            res.set_content(json_response(0, "success"), "application/json");
        });

    /* GET /api/v1/admin/versions/:version/downloads — download statistics */
    svr.Get(R"(/api/v1/admin/versions/([^/]+)/downloads)",
        [&ctx](const httplib::Request &req, httplib::Response &res) {
            if (!check_admin_token(req, res, ctx)) return;

            std::string version = req.matches[1].str();

            /* Look up version to get its id */
            VersionRecord rec;
            std::string err;
            if (!ctx.db.get_version_by_name(version, rec, err)) {
                res.set_content(json_error(1003, "version not found"),
                               "application/json");
                res.status = 404;
                return;
            }

            int page = get_query_int(req, "page", 1);
            int page_size = get_query_int(req, "page_size", 20);

            DownloadLogPage dlpage;
            if (!ctx.db.get_download_logs(rec.id, page, page_size, dlpage, err)) {
                res.set_content(json_error(3001, "internal server error"),
                               "application/json");
                res.status = 500;
                return;
            }

            /* Build response JSON */
            cJSON *data = cJSON_CreateObject();
            cJSON_AddNumberToObject(data, "total", dlpage.paging.total);
            cJSON_AddNumberToObject(data, "page", dlpage.paging.page);
            cJSON_AddNumberToObject(data, "page_size", dlpage.paging.page_size);

            cJSON *items = cJSON_CreateArray();
            for (size_t i = 0; i < dlpage.items.size(); ++i) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "device_id",
                                        dlpage.items[i].device_id.c_str());
                cJSON_AddStringToObject(item, "ip_address",
                                        dlpage.items[i].ip_address.c_str());
                cJSON_AddStringToObject(item, "downloaded_at",
                                        dlpage.items[i].downloaded_at.c_str());
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
}
