#ifndef AG_DATABASE_H
#define AG_DATABASE_H

#include <string>
#include <vector>
#include <cstdint>

struct VersionRecord {
    int id;
    std::string version;
    std::string description;
    std::string file_name;
    int64_t file_size;
    std::string file_sha256;
    std::string created_at;
    int download_count;
};

struct DownloadLogRecord {
    int id;
    int version_id;
    std::string device_id;
    std::string ip_address;
    std::string downloaded_at;
};

struct PageResult {
    int total;
    int page;
    int page_size;
};

struct VersionPage {
    PageResult paging;
    std::vector<VersionRecord> items;
};

struct DownloadLogPage {
    PageResult paging;
    std::vector<DownloadLogRecord> items;
};

/* Forward declaration for sqlite3 handle */
struct sqlite3;

class Database {
public:
    Database();
    ~Database();

    /* Non-copyable */
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open(const std::string &db_path, std::string &err_msg);
    void close();

    /* Version CRUD */
    bool insert_version(const std::string &version, const std::string &description,
                        const std::string &file_name, int64_t file_size,
                        const std::string &file_sha256, int &out_id,
                        std::string &err_msg);

    bool get_version_by_name(const std::string &version, VersionRecord &out,
                             std::string &err_msg);

    bool get_all_versions(std::vector<VersionRecord> &out, std::string &err_msg);

    bool get_versions_paged(int page, int page_size, VersionPage &out,
                            std::string &err_msg);

    bool update_version_description(const std::string &version,
                                    const std::string &description,
                                    std::string &err_msg);

    bool delete_version(const std::string &version, std::string &err_msg);

    bool delete_versions_batch(const std::vector<std::string> &versions,
                               std::string &err_msg);

    /* Download logging */
    bool log_download(int version_id, const std::string &device_id,
                      const std::string &ip_address, std::string &err_msg);

    bool increment_download_count(int version_id, std::string &err_msg);

    bool get_download_logs(int version_id, int page, int page_size,
                           DownloadLogPage &out, std::string &err_msg);

private:
    sqlite3 *db_;
    bool create_tables(std::string &err_msg);
    bool version_exists(const std::string &version);
    int get_download_count_from_summary(int version_id);
};

#endif /* AG_DATABASE_H */
