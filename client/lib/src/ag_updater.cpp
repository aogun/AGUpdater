#include "ag_updater.h"
#include "http_client.h"
#include "auth.h"
#include "version_util.h"
#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>

#include <openssl/sha.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <libgen.h>
#endif

#ifndef AG_UPDATER_NAME
#define AG_UPDATER_NAME "ag-updater"
#endif

static void safe_strncpy(char *dst, const char *src, size_t dst_size)
{
    if (dst_size == 0) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static std::string get_temp_dir()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, buf);
    if (len > 0 && len < MAX_PATH) {
        return std::string(buf, len);
    }
    return std::string(".");
#else
    const char *tmp = getenv("TMPDIR");
    if (tmp && tmp[0]) return std::string(tmp);
    return std::string("/tmp");
#endif
}

static std::string get_exe_dir()
{
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::string(".");
    char *sep = strrchr(path, '\\');
    if (sep) *sep = '\0';
    return std::string(path);
#else
    char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return std::string(".");
    path[len] = '\0';
    char *sep = strrchr(path, '/');
    if (sep) *sep = '\0';
    return std::string(path);
#endif
}

/* ---- ag_check_update ---- */

ag_error_t ag_check_update(
    const char *app_name,
    const char *current_version,
    ag_check_callback callback,
    void *user_data)
{
    if (!current_version || !callback) {
        return AG_ERR_INTERNAL;
    }
    if (!ag_semver_validate(std::string(current_version))) {
        return AG_ERR_INTERNAL;
    }

    /* Capture parameters for thread */
    std::string ver_str(current_version);
    std::string app_str(app_name ? app_name : "");

    std::thread t([ver_str, app_str, callback, user_data]() {
        std::string path = "/api/v1/client/updates?current_version=" + ver_str;
        HttpResult res = http_get(path);

        if (!res.ok) {
            callback(AG_ERR_NETWORK, NULL, 0, user_data);
            return;
        }

        if (res.status_code == 403) {
            callback(AG_ERR_AUTH, NULL, 0, user_data);
            return;
        }

        /* Parse JSON response */
        cJSON *root = cJSON_Parse(res.body.c_str());
        if (!root) {
            callback(AG_ERR_INTERNAL, NULL, 0, user_data);
            return;
        }

        const cJSON *j_code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (!cJSON_IsNumber(j_code) || j_code->valueint != 0) {
            cJSON_Delete(root);
            callback(AG_ERR_INTERNAL, NULL, 0, user_data);
            return;
        }

        const cJSON *j_data = cJSON_GetObjectItemCaseSensitive(root, "data");
        const cJSON *j_has_update = cJSON_GetObjectItemCaseSensitive(j_data, "has_update");
        const cJSON *j_updates = cJSON_GetObjectItemCaseSensitive(j_data, "updates");

        if (!cJSON_IsBool(j_has_update) || !cJSON_IsArray(j_updates)) {
            cJSON_Delete(root);
            callback(AG_ERR_INTERNAL, NULL, 0, user_data);
            return;
        }

        if (!cJSON_IsTrue(j_has_update) || cJSON_GetArraySize(j_updates) == 0) {
            cJSON_Delete(root);
            callback(AG_ERR_NO_UPDATE, NULL, 0, user_data);
            return;
        }

        /* Build version info array */
        int count = cJSON_GetArraySize(j_updates);
        std::vector<ag_version_info_t> infos(count);

        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem(j_updates, i);
            memset(&infos[i], 0, sizeof(ag_version_info_t));

            const cJSON *jv = cJSON_GetObjectItemCaseSensitive(item, "version");
            if (cJSON_IsString(jv)) {
                safe_strncpy(infos[i].version, jv->valuestring,
                            sizeof(infos[i].version));
            }

            const cJSON *jd = cJSON_GetObjectItemCaseSensitive(item, "description");
            if (cJSON_IsString(jd)) {
                safe_strncpy(infos[i].description, jd->valuestring,
                            sizeof(infos[i].description));
            }

            const cJSON *jfs = cJSON_GetObjectItemCaseSensitive(item, "file_size");
            if (cJSON_IsNumber(jfs)) {
                infos[i].file_size = static_cast<int64_t>(jfs->valuedouble);
            }

            const cJSON *jsha = cJSON_GetObjectItemCaseSensitive(item, "file_sha256");
            if (cJSON_IsString(jsha)) {
                safe_strncpy(infos[i].file_sha256, jsha->valuestring,
                            sizeof(infos[i].file_sha256));
            }

            const cJSON *jca = cJSON_GetObjectItemCaseSensitive(item, "created_at");
            if (cJSON_IsString(jca)) {
                safe_strncpy(infos[i].created_at, jca->valuestring,
                            sizeof(infos[i].created_at));
            }

            /* Build download URL */
            std::string dl_url = "/api/v1/client/download/" +
                                  std::string(infos[i].version);
            safe_strncpy(infos[i].download_url, dl_url.c_str(),
                        sizeof(infos[i].download_url));
        }

        cJSON_Delete(root);
        callback(AG_OK, &infos[0], count, user_data);
    });
    t.detach();

    return AG_OK;
}

/* ---- ag_download_update ---- */

ag_error_t ag_download_update(
    const ag_version_info_t *info,
    ag_download_callback callback,
    void *user_data)
{
    if (!info || !callback) {
        return AG_ERR_INTERNAL;
    }

    /* Copy info for thread */
    ag_version_info_t info_copy = *info;

    std::thread t([info_copy, callback, user_data]() {
        /* Build temp file path */
        std::string temp_dir = get_temp_dir();
        std::string file_path = temp_dir + "/" +
                                 std::string(info_copy.version) + ".zip";

        /* Download */
        std::string err_msg;
        std::string dl_path(info_copy.download_url);

        bool ok = http_download(dl_path, file_path, info_copy.file_size,
            [&callback, &user_data](int64_t downloaded, int64_t total) -> bool {
                ag_download_progress_t progress;
                progress.total_bytes = total;
                progress.downloaded_bytes = downloaded;
                progress.percent = (total > 0)
                    ? static_cast<int>((downloaded * 100) / total) : 0;
                callback(AG_OK, &progress, NULL, user_data);
                return true;
            },
            err_msg);

        if (!ok) {
            ag_download_progress_t progress;
            memset(&progress, 0, sizeof(progress));
            callback(AG_ERR_NETWORK, &progress, NULL, user_data);
            return;
        }

        /* Verify SHA256 */
        FILE *f = fopen(file_path.c_str(), "rb");
        if (!f) {
            ag_download_progress_t progress;
            memset(&progress, 0, sizeof(progress));
            callback(AG_ERR_IO, &progress, NULL, user_data);
            return;
        }

        /* Stream file in chunks for SHA256 to avoid large allocations
         * and ftell overflow on files > 2GB */
        SHA256_CTX sha_ctx;
        SHA256_Init(&sha_ctx);
        char sha_buf[65536];
        size_t sha_n;
        while ((sha_n = fread(sha_buf, 1, sizeof(sha_buf), f)) > 0) {
            SHA256_Update(&sha_ctx, sha_buf, sha_n);
        }
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(hash, &sha_ctx);
        fclose(f);

        char hex[SHA256_DIGEST_LENGTH * 2 + 1];
        for (int hi = 0; hi < SHA256_DIGEST_LENGTH; ++hi) {
            snprintf(hex + hi * 2, 3, "%02x", hash[hi]);
        }
        std::string actual_sha(hex, SHA256_DIGEST_LENGTH * 2);

        if (actual_sha != std::string(info_copy.file_sha256)) {
            remove(file_path.c_str());
            ag_download_progress_t progress;
            memset(&progress, 0, sizeof(progress));
            callback(AG_ERR_CHECKSUM, &progress, NULL, user_data);
            return;
        }

        /* Success */
        ag_download_progress_t progress;
        progress.total_bytes = info_copy.file_size;
        progress.downloaded_bytes = info_copy.file_size;
        progress.percent = 100;
        callback(AG_OK, &progress, file_path.c_str(), user_data);
    });
    t.detach();

    return AG_OK;
}

/* ---- ag_apply_update ---- */

ag_error_t ag_apply_update(const char *zip_path)
{
    if (!zip_path || !zip_path[0]) {
        return AG_ERR_INTERNAL;
    }

    /* Find ag-updater executable in same directory as current exe */
    std::string exe_dir = get_exe_dir();

#ifdef _WIN32
    std::string updater_path = exe_dir + "\\" + AG_UPDATER_NAME + ".exe";
#else
    std::string updater_path = exe_dir + "/" + AG_UPDATER_NAME;
#endif

    /* Check updater exists */
#ifdef _WIN32
    if (GetFileAttributesA(updater_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return AG_ERR_NOT_FOUND;
    }

    /* Launch updater process.
     * Use lpApplicationName to specify the executable path directly,
     * and lpCommandLine for arguments only, to prevent command injection. */
    std::string cmd_args = "\"" + updater_path + "\" \"" +
                           std::string(zip_path) + "\"";
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(updater_path.c_str(), &cmd_args[0],
                        NULL, NULL, FALSE,
                        0, NULL, NULL, &si, &pi)) {
        return AG_ERR_IO;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    if (access(updater_path.c_str(), X_OK) != 0) {
        return AG_ERR_NOT_FOUND;
    }

    /* Fork and exec */
    pid_t pid = fork();
    if (pid < 0) {
        return AG_ERR_IO;
    }
    if (pid == 0) {
        execl(updater_path.c_str(), updater_path.c_str(), zip_path, NULL);
        _exit(1);
    }
#endif

    return AG_OK;
}
