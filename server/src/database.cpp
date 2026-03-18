#include "database.h"
#include "sqlite3.h"
#include "cJSON.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <sstream>

Database::Database() : db_(NULL) {}

Database::~Database()
{
    close();
}

bool Database::open(const std::string &db_path, std::string &err_msg)
{
    LOG_TRACE("Database::open() enter, path=%s", db_path.c_str());

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        err_msg = std::string("failed to open database: ") + sqlite3_errmsg(db_);
        LOG_ERROR("Failed to open database '%s': %s", db_path.c_str(), sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = NULL;
        return false;
    }

    /* Enable WAL mode for better concurrent read performance */
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    LOG_DEBUG("Database WAL mode enabled");

    /* Enable foreign keys */
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    LOG_DEBUG("Database foreign keys enabled");

    if (!create_tables(err_msg)) {
        LOG_ERROR("Failed to create tables: %s", err_msg.c_str());
        close();
        return false;
    }

    LOG_INFO("Database opened successfully: %s", db_path.c_str());
    return true;
}

void Database::close()
{
    if (db_ != NULL) {
        LOG_DEBUG("Closing database");
        sqlite3_close(db_);
        db_ = NULL;
    }
}

bool Database::create_tables(std::string &err_msg)
{
    LOG_TRACE("Database::create_tables() enter");

    const char *sql =
        "CREATE TABLE IF NOT EXISTS versions ("
        "    id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    version       TEXT    UNIQUE NOT NULL,"
        "    description   TEXT    NOT NULL,"
        "    file_name     TEXT    NOT NULL,"
        "    file_size     INTEGER NOT NULL,"
        "    file_sha256   TEXT    NOT NULL,"
        "    created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),"
        "    summary       TEXT    DEFAULT '{}'"
        ");"
        "CREATE TABLE IF NOT EXISTS download_logs ("
        "    id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    version_id    INTEGER NOT NULL,"
        "    device_id     TEXT    NOT NULL,"
        "    ip_address    TEXT    NOT NULL,"
        "    downloaded_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),"
        "    FOREIGN KEY (version_id) REFERENCES versions(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_download_logs_version ON download_logs(version_id);";

    char *errmsg = NULL;
    int rc = sqlite3_exec(db_, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        err_msg = std::string("failed to create tables: ") +
                  (errmsg ? errmsg : "unknown error");
        LOG_ERROR("Failed to create tables: %s", err_msg.c_str());
        sqlite3_free(errmsg);
        return false;
    }
    LOG_DEBUG("Database tables created/verified successfully");
    return true;
}

bool Database::version_exists(const std::string &version)
{
    LOG_TRACE("Database::version_exists() version=%s", version.c_str());

    const char *sql = "SELECT COUNT(*) FROM versions WHERE version = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("version_exists prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(stmt, 1, version.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    LOG_DEBUG("version_exists(%s) = %s", version.c_str(), exists ? "true" : "false");
    return exists;
}

bool Database::insert_version(const std::string &version,
                              const std::string &description,
                              const std::string &file_name,
                              int64_t file_size,
                              const std::string &file_sha256,
                              int &out_id,
                              std::string &err_msg)
{
    LOG_TRACE("Database::insert_version() enter, version=%s", version.c_str());

    const char *sql =
        "INSERT INTO versions (version, description, file_name, file_size, "
        "file_sha256, summary) VALUES (?, ?, ?, ?, ?, '{\"download_count\":0}');";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("insert_version prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, file_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, file_size);
    sqlite3_bind_text(stmt, 5, file_sha256.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        err_msg = std::string("insert error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("insert_version error for '%s': %s", version.c_str(), sqlite3_errmsg(db_));
        return false;
    }

    out_id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    LOG_INFO("Inserted version '%s' with id=%d, file_size=%lld",
             version.c_str(), out_id, (long long)file_size);
    return true;
}

int Database::get_download_count_from_summary(int version_id)
{
    LOG_TRACE("Database::get_download_count_from_summary() version_id=%d", version_id);

    const char *sql = "SELECT summary FROM versions WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("get_download_count prepare error: %s", sqlite3_errmsg(db_));
        return 0;
    }
    sqlite3_bind_int(stmt, 1, version_id);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *summary = (const char *)sqlite3_column_text(stmt, 0);
        if (summary != NULL) {
            cJSON *json = cJSON_Parse(summary);
            if (json != NULL) {
                const cJSON *dc = cJSON_GetObjectItemCaseSensitive(json,
                                                                    "download_count");
                if (cJSON_IsNumber(dc)) {
                    count = dc->valueint;
                }
                cJSON_Delete(json);
            }
        }
    }
    sqlite3_finalize(stmt);
    LOG_DEBUG("Download count for version_id=%d: %d", version_id, count);
    return count;
}

static VersionRecord row_to_version(sqlite3_stmt *stmt, int download_count)
{
    VersionRecord rec;
    rec.id = sqlite3_column_int(stmt, 0);

    const char *ver = (const char *)sqlite3_column_text(stmt, 1);
    rec.version = ver ? ver : "";

    const char *desc = (const char *)sqlite3_column_text(stmt, 2);
    rec.description = desc ? desc : "";

    const char *fname = (const char *)sqlite3_column_text(stmt, 3);
    rec.file_name = fname ? fname : "";

    rec.file_size = sqlite3_column_int64(stmt, 4);

    const char *sha = (const char *)sqlite3_column_text(stmt, 5);
    rec.file_sha256 = sha ? sha : "";

    const char *created = (const char *)sqlite3_column_text(stmt, 6);
    rec.created_at = created ? created : "";

    rec.download_count = download_count;
    return rec;
}

static int extract_download_count(sqlite3_stmt *stmt, int summary_col)
{
    int count = 0;
    const char *summary = (const char *)sqlite3_column_text(stmt, summary_col);
    if (summary != NULL) {
        cJSON *json = cJSON_Parse(summary);
        if (json != NULL) {
            const cJSON *dc = cJSON_GetObjectItemCaseSensitive(json,
                                                                "download_count");
            if (cJSON_IsNumber(dc)) {
                count = dc->valueint;
            }
            cJSON_Delete(json);
        }
    }
    return count;
}

bool Database::get_version_by_name(const std::string &version, VersionRecord &out,
                                   std::string &err_msg)
{
    LOG_TRACE("Database::get_version_by_name() version=%s", version.c_str());

    const char *sql =
        "SELECT id, version, description, file_name, file_size, file_sha256, "
        "created_at, summary FROM versions WHERE version = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("get_version_by_name prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, version.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        err_msg = "version not found: " + version;
        LOG_DEBUG("Version not found: %s", version.c_str());
        return false;
    }

    int dl_count = extract_download_count(stmt, 7);
    out = row_to_version(stmt, dl_count);
    sqlite3_finalize(stmt);
    LOG_DEBUG("Found version '%s' (id=%d)", version.c_str(), out.id);
    return true;
}

bool Database::get_all_versions(std::vector<VersionRecord> &out,
                                std::string &err_msg)
{
    LOG_TRACE("Database::get_all_versions() enter");

    const char *sql =
        "SELECT id, version, description, file_name, file_size, file_sha256, "
        "created_at, summary FROM versions ORDER BY id DESC;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("get_all_versions prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }

    out.clear();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int dl_count = extract_download_count(stmt, 7);
        out.push_back(row_to_version(stmt, dl_count));
    }
    sqlite3_finalize(stmt);
    LOG_DEBUG("get_all_versions returned %zu records", out.size());
    return true;
}

bool Database::get_versions_paged(int page, int page_size, VersionPage &out,
                                  std::string &err_msg)
{
    LOG_TRACE("Database::get_versions_paged() page=%d, page_size=%d", page, page_size);

    if (page < 1) page = 1;
    if (page_size < 1) page_size = 20;

    /* Get total count */
    const char *count_sql = "SELECT COUNT(*) FROM versions;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, count_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("get_versions_paged count prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }
    int total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    out.paging.total = total;
    out.paging.page = page;
    out.paging.page_size = page_size;
    out.items.clear();

    if (total == 0) {
        LOG_DEBUG("get_versions_paged: no versions found");
        return true;
    }

    int64_t offset = (int64_t)(page - 1) * page_size;
    const char *sql =
        "SELECT id, version, description, file_name, file_size, file_sha256, "
        "created_at, summary FROM versions ORDER BY id DESC LIMIT ? OFFSET ?;";

    rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("get_versions_paged query prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int(stmt, 1, page_size);
    sqlite3_bind_int64(stmt, 2, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int dl_count = extract_download_count(stmt, 7);
        out.items.push_back(row_to_version(stmt, dl_count));
    }
    sqlite3_finalize(stmt);
    LOG_DEBUG("get_versions_paged: total=%d, returned %zu items (page %d)",
              total, out.items.size(), page);
    return true;
}

bool Database::update_version_description(const std::string &version,
                                          const std::string &description,
                                          std::string &err_msg)
{
    LOG_TRACE("Database::update_version_description() version=%s", version.c_str());

    const char *sql = "UPDATE versions SET description = ? WHERE version = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("update_version_description prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(stmt, 1, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, version.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        err_msg = std::string("update error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("update_version_description error for '%s': %s",
                  version.c_str(), sqlite3_errmsg(db_));
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        err_msg = "version not found: " + version;
        LOG_WARN("update_version_description: version not found: %s", version.c_str());
        return false;
    }
    LOG_INFO("Updated description for version '%s'", version.c_str());
    return true;
}

bool Database::delete_version(const std::string &version, std::string &err_msg)
{
    LOG_TRACE("Database::delete_version() version=%s", version.c_str());

    const char *sql = "DELETE FROM versions WHERE version = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("delete_version prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(stmt, 1, version.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        err_msg = std::string("delete error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("delete_version error for '%s': %s", version.c_str(), sqlite3_errmsg(db_));
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        err_msg = "version not found: " + version;
        LOG_WARN("delete_version: version not found: %s", version.c_str());
        return false;
    }
    LOG_INFO("Deleted version '%s'", version.c_str());
    return true;
}

bool Database::delete_versions_batch(const std::vector<std::string> &versions,
                                     std::string &err_msg)
{
    LOG_TRACE("Database::delete_versions_batch() count=%zu", versions.size());

    if (versions.empty()) {
        return true;
    }

    char *errmsg = NULL;
    sqlite3_exec(db_, "BEGIN TRANSACTION;", NULL, NULL, &errmsg);
    if (errmsg) {
        err_msg = std::string("begin transaction error: ") + errmsg;
        LOG_ERROR("delete_versions_batch begin transaction error: %s", errmsg);
        sqlite3_free(errmsg);
        return false;
    }

    const char *sql = "DELETE FROM versions WHERE version = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", NULL, NULL, NULL);
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("delete_versions_batch prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }

    for (size_t i = 0; i < versions.size(); ++i) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, versions[i].c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db_, "ROLLBACK;", NULL, NULL, NULL);
            err_msg = std::string("delete error for ") + versions[i] + ": " +
                      sqlite3_errmsg(db_);
            LOG_ERROR("delete_versions_batch error for '%s': %s",
                      versions[i].c_str(), sqlite3_errmsg(db_));
            return false;
        }
        LOG_DEBUG("Batch delete: removed version '%s'", versions[i].c_str());
    }

    sqlite3_finalize(stmt);

    char *commit_err = NULL;
    int commit_rc = sqlite3_exec(db_, "COMMIT;", NULL, NULL, &commit_err);
    if (commit_rc != SQLITE_OK) {
        err_msg = std::string("commit error: ") +
                  (commit_err ? commit_err : "unknown");
        LOG_ERROR("delete_versions_batch commit error: %s", err_msg.c_str());
        sqlite3_free(commit_err);
        sqlite3_exec(db_, "ROLLBACK;", NULL, NULL, NULL);
        return false;
    }
    LOG_INFO("Batch deleted %zu versions", versions.size());
    return true;
}

bool Database::log_download(int version_id, const std::string &device_id,
                            const std::string &ip_address, std::string &err_msg)
{
    LOG_TRACE("Database::log_download() version_id=%d, device=%s, ip=%s",
              version_id, device_id.c_str(), ip_address.c_str());

    const char *sql =
        "INSERT INTO download_logs (version_id, device_id, ip_address) "
        "VALUES (?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("log_download prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_int(stmt, 1, version_id);
    sqlite3_bind_text(stmt, 2, device_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ip_address.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        err_msg = std::string("insert error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("log_download insert error: %s", sqlite3_errmsg(db_));
        return false;
    }
    LOG_DEBUG("Logged download: version_id=%d, device=%s, ip=%s",
              version_id, device_id.c_str(), ip_address.c_str());
    return true;
}

bool Database::increment_download_count(int version_id, std::string &err_msg)
{
    LOG_TRACE("Database::increment_download_count() version_id=%d", version_id);

    /* Atomic SQL update using json_set/json_extract (SQLite 3.38+) */
    const char *sql =
        "UPDATE versions SET summary = json_set("
        "COALESCE(summary,'{}'), '$.download_count', "
        "COALESCE(json_extract(summary,'$.download_count'),0)+1"
        ") WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("increment_download_count prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_int(stmt, 1, version_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        err_msg = std::string("update error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("increment_download_count error for version_id=%d: %s",
                  version_id, sqlite3_errmsg(db_));
        return false;
    }
    LOG_DEBUG("Incremented download count for version_id=%d", version_id);
    return true;
}

bool Database::get_download_logs(int version_id, int page, int page_size,
                                 DownloadLogPage &out, std::string &err_msg)
{
    LOG_TRACE("Database::get_download_logs() version_id=%d, page=%d, page_size=%d",
              version_id, page, page_size);

    if (page < 1) page = 1;
    if (page_size < 1) page_size = 20;

    /* Get total count */
    const char *count_sql =
        "SELECT COUNT(*) FROM download_logs WHERE version_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db_, count_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("get_download_logs count prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int(stmt, 1, version_id);

    int total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    out.paging.total = total;
    out.paging.page = page;
    out.paging.page_size = page_size;
    out.items.clear();

    if (total == 0) {
        LOG_DEBUG("get_download_logs: no logs for version_id=%d", version_id);
        return true;
    }

    int64_t offset = (int64_t)(page - 1) * page_size;
    const char *sql =
        "SELECT id, version_id, device_id, ip_address, downloaded_at "
        "FROM download_logs WHERE version_id = ? "
        "ORDER BY id DESC LIMIT ? OFFSET ?;";

    rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        err_msg = std::string("prepare error: ") + sqlite3_errmsg(db_);
        LOG_ERROR("get_download_logs query prepare error: %s", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int(stmt, 1, version_id);
    sqlite3_bind_int(stmt, 2, page_size);
    sqlite3_bind_int64(stmt, 3, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DownloadLogRecord rec;
        rec.id = sqlite3_column_int(stmt, 0);
        rec.version_id = sqlite3_column_int(stmt, 1);

        const char *dev = (const char *)sqlite3_column_text(stmt, 2);
        rec.device_id = dev ? dev : "";

        const char *ip = (const char *)sqlite3_column_text(stmt, 3);
        rec.ip_address = ip ? ip : "";

        const char *dl_at = (const char *)sqlite3_column_text(stmt, 4);
        rec.downloaded_at = dl_at ? dl_at : "";

        out.items.push_back(rec);
    }
    sqlite3_finalize(stmt);
    LOG_DEBUG("get_download_logs: version_id=%d, total=%d, returned %zu items",
              version_id, total, out.items.size());
    return true;
}
