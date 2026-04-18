#include "ag_updater.h"
#include "http_client.h"
#include "auth.h"
#include "version_util.h"
#include "log.h"
#include "cJSON.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>

#include <openssl/evp.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/stat.h>
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

static std::string get_system_temp_dir()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, buf);
    if (len > 0 && len < MAX_PATH) {
        return std::string(buf, len);   /* includes trailing backslash */
    }
    return std::string(".\\");
#else
    const char *tmp = getenv("TMPDIR");
    if (tmp && tmp[0]) return std::string(tmp) + "/";
    return std::string("/tmp/");
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

static std::string md5_hex(const std::string &input)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    unsigned char h[EVP_MAX_MD_SIZE];
    unsigned int hlen = 0;
    EVP_DigestFinal_ex(ctx, h, &hlen);
    EVP_MD_CTX_free(ctx);
    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    for (unsigned int i = 0; i < hlen; ++i) {
        snprintf(hex + i * 2, 3, "%02x", h[i]);
    }
    return std::string(hex, hlen * 2);
}

/* Normalize exe dir path for MD5 hashing: lowercase + unified separators +
 * no trailing separator. Same install dir in different cases must hash the
 * same so cleanup targets the correct subdir. */
static std::string normalize_path_for_hash(const std::string &dir)
{
    std::string s = dir;
#ifdef _WIN32
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = (char)tolower((unsigned char)s[i]);
        if (s[i] == '/') s[i] = '\\';
    }
    while (s.size() > 1 &&
           (s.back() == '\\' || s.back() == '/')) s.pop_back();
#else
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#endif
    return s;
}

/* Returns %TEMP%\ag-updater-<md5>\  (trailing separator included).
 * The directory is created if missing. Empty string on failure. */
static std::string get_app_temp_dir()
{
    std::string exe_dir = get_exe_dir();
    std::string hash = md5_hex(normalize_path_for_hash(exe_dir));
    std::string dir = get_system_temp_dir() + "ag-updater-" + hash;

#ifdef _WIN32
    if (!CreateDirectoryA(dir.c_str(), NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        LOG_ERROR("get_app_temp_dir: CreateDirectoryA(%s) failed (error=%lu)",
                  dir.c_str(), (unsigned long)GetLastError());
        return std::string();
    }
    return dir + "\\";
#else
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        LOG_ERROR("get_app_temp_dir: mkdir(%s) failed: %s",
                  dir.c_str(), strerror(errno));
        return std::string();
    }
    return dir + "/";
#endif
}

#ifdef _WIN32
/* Best-effort removal of .old-<tick> rename backups left in dir.
 * Files still locked are skipped and left for a future sweep. */
static int cleanup_old_backups(const std::string &dir)
{
    int removed = 0;
    std::string pattern = dir + "\\*.old-*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string path = dir + "\\" + fd.cFileName;
        if (DeleteFileA(path.c_str())) {
            LOG_INFO("cleanup: removed backup %s", path.c_str());
            ++removed;
        } else {
            LOG_DEBUG("cleanup: backup still in use: %s (error=%lu)",
                      path.c_str(), (unsigned long)GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return removed;
}

/* Recursively delete dir and its contents. Logs per-entry action. */
static void remove_dir_recursive(const std::string &path,
                                  int &files_removed, int &dirs_removed)
{
    std::string pattern = path + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            std::string child = path + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_dir_recursive(child, files_removed, dirs_removed);
            } else {
                if (!DeleteFileA(child.c_str())) {
                    SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (!DeleteFileA(child.c_str())) {
                        LOG_WARN("cleanup: cannot remove file %s (error=%lu)",
                                 child.c_str(),
                                 (unsigned long)GetLastError());
                        continue;
                    }
                }
                LOG_INFO("cleanup: removed file %s", child.c_str());
                ++files_removed;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (RemoveDirectoryA(path.c_str())) {
        LOG_INFO("cleanup: removed dir  %s", path.c_str());
        ++dirs_removed;
    } else {
        LOG_WARN("cleanup: cannot remove dir %s (error=%lu)",
                 path.c_str(), (unsigned long)GetLastError());
    }
}
#endif

/* ---- ag_check_update ---- */

ag_error_t ag_check_update(
    const char *app_name,
    const char *current_version,
    ag_check_callback callback,
    void *user_data)
{
    if (!current_version || !callback) {
        LOG_ERROR("ag_check_update: invalid parameters (current_version=%p, callback=%p)",
                  (const void *)current_version, (const void *)callback);
        return AG_ERR_INTERNAL;
    }
    if (!ag_semver_validate(std::string(current_version))) {
        LOG_ERROR("ag_check_update: invalid semver: %s", current_version);
        return AG_ERR_INTERNAL;
    }

    /* Capture parameters for thread */
    std::string ver_str(current_version);
    std::string app_str(app_name ? app_name : "");
    LOG_DEBUG("ag_check_update: app=%s, current_version=%s", app_str.c_str(), ver_str.c_str());

    std::thread t([ver_str, app_str, callback, user_data]() {
        std::string path = "/api/v1/client/updates?current_version=" + ver_str;
        LOG_DEBUG("ag_check_update: HTTP GET %s", path.c_str());
        HttpResult res = http_get(path);

        if (!res.ok) {
            LOG_ERROR("ag_check_update failed: %s", res.error.c_str());
            callback(AG_ERR_NETWORK, NULL, 0, user_data);
            return;
        }

        if (res.status_code == 403) {
            LOG_ERROR("ag_check_update: authentication failed (HTTP 403)");
            callback(AG_ERR_AUTH, NULL, 0, user_data);
            return;
        }

        LOG_TRACE("ag_check_update: response body length=%zu", res.body.size());

        /* Parse JSON response */
        cJSON *root = cJSON_Parse(res.body.c_str());
        if (!root) {
            LOG_ERROR("ag_check_update: JSON parse failed");
            callback(AG_ERR_INTERNAL, NULL, 0, user_data);
            return;
        }

        const cJSON *j_code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (!cJSON_IsNumber(j_code) || j_code->valueint != 0) {
            LOG_ERROR("ag_check_update: server returned error code %d",
                      cJSON_IsNumber(j_code) ? j_code->valueint : -1);
            cJSON_Delete(root);
            callback(AG_ERR_INTERNAL, NULL, 0, user_data);
            return;
        }

        const cJSON *j_data = cJSON_GetObjectItemCaseSensitive(root, "data");
        const cJSON *j_has_update = cJSON_GetObjectItemCaseSensitive(j_data, "has_update");
        const cJSON *j_updates = cJSON_GetObjectItemCaseSensitive(j_data, "updates");

        if (!cJSON_IsBool(j_has_update) || !cJSON_IsArray(j_updates)) {
            LOG_ERROR("ag_check_update: unexpected JSON structure");
            cJSON_Delete(root);
            callback(AG_ERR_INTERNAL, NULL, 0, user_data);
            return;
        }

        if (!cJSON_IsTrue(j_has_update) || cJSON_GetArraySize(j_updates) == 0) {
            LOG_INFO("ag_check_update: no update available");
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

        LOG_INFO("ag_check_update: found %d update(s)", count);
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
        LOG_ERROR("ag_download_update: invalid parameters (info=%p, callback=%p)",
                  (const void *)info, (const void *)callback);
        return AG_ERR_INTERNAL;
    }

    LOG_INFO("ag_download_update: starting download for version %s", info->version);

    /* Copy info for thread */
    ag_version_info_t info_copy = *info;

    std::thread t([info_copy, callback, user_data]() {
        /* Build temp file path inside the per-install temp dir so different
         * installations do not share download artifacts. */
        std::string temp_dir = get_app_temp_dir();
        if (temp_dir.empty()) temp_dir = get_system_temp_dir();
        std::string file_path = temp_dir +
                                 std::string(info_copy.version) + ".zip";

        /* Download */
        std::string err_msg;
        std::string dl_path(info_copy.download_url);
        LOG_DEBUG("ag_download_update: url=%s, dest=%s", dl_path.c_str(), file_path.c_str());

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
            LOG_ERROR("ag_download_update: download failed: %s", err_msg.c_str());
            ag_download_progress_t progress;
            memset(&progress, 0, sizeof(progress));
            callback(AG_ERR_NETWORK, &progress, NULL, user_data);
            return;
        }

        LOG_INFO("ag_download_update: download complete, verifying SHA256");

        /* Verify SHA256 */
        FILE *f = fopen(file_path.c_str(), "rb");
        if (!f) {
            LOG_ERROR("ag_download_update: cannot open downloaded file: %s", file_path.c_str());
            ag_download_progress_t progress;
            memset(&progress, 0, sizeof(progress));
            callback(AG_ERR_IO, &progress, NULL, user_data);
            return;
        }

        /* Stream file in chunks for SHA256 using EVP API (OpenSSL 3.0+)
         * to avoid large allocations and ftell overflow on files > 2GB */
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
        char sha_buf[65536];
        size_t sha_n;
        while ((sha_n = fread(sha_buf, 1, sizeof(sha_buf), f)) > 0) {
            EVP_DigestUpdate(mdctx, sha_buf, sha_n);
        }
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len = 0;
        EVP_DigestFinal_ex(mdctx, hash, &hash_len);
        EVP_MD_CTX_free(mdctx);
        fclose(f);

        char hex[EVP_MAX_MD_SIZE * 2 + 1];
        for (unsigned int hi = 0; hi < hash_len; ++hi) {
            snprintf(hex + hi * 2, 3, "%02x", hash[hi]);
        }
        std::string actual_sha(hex, hash_len * 2);

        LOG_DEBUG("ag_download_update: SHA256 expected=%s, actual=%s",
                  info_copy.file_sha256, actual_sha.c_str());

        if (actual_sha != std::string(info_copy.file_sha256)) {
            LOG_ERROR("ag_download_update: SHA256 mismatch");
            remove(file_path.c_str());
            ag_download_progress_t progress;
            memset(&progress, 0, sizeof(progress));
            callback(AG_ERR_CHECKSUM, &progress, NULL, user_data);
            return;
        }

        /* Success */
        LOG_INFO("ag_download_update: verified and saved to %s", file_path.c_str());
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

#ifdef _WIN32
/* Stage ag-updater.exe + all *.dll from src_dir into a fresh temp dir.
 * Running updater from the staging dir prevents Windows from locking the
 * DLLs in the target directory, so the update can overwrite them. */
static std::string stage_updater_files(const std::string &src_dir)
{
    /* Put staging under the per-install temp dir so it's included in cleanup
     * and is co-located with the downloaded zip. */
    std::string app_tmp = get_app_temp_dir();
    if (app_tmp.empty()) {
        LOG_ERROR("ag_apply_update: cannot resolve app temp dir");
        return std::string();
    }

    char staging[MAX_PATH];
    snprintf(staging, MAX_PATH, "%sstage-%lu-%lu",
             app_tmp.c_str(), (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    if (!CreateDirectoryA(staging, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        LOG_ERROR("ag_apply_update: cannot create staging dir %s (error=%lu)",
                  staging, (unsigned long)GetLastError());
        return std::string();
    }
    std::string staging_dir(staging);
    LOG_DEBUG("ag_apply_update: staging dir %s", staging_dir.c_str());

    /* Copy ag-updater.exe */
    std::string updater_src = src_dir + "\\" + AG_UPDATER_NAME + ".exe";
    std::string updater_dst = staging_dir + "\\" + AG_UPDATER_NAME + ".exe";
    if (!CopyFileA(updater_src.c_str(), updater_dst.c_str(), FALSE)) {
        LOG_ERROR("ag_apply_update: CopyFileA %s -> %s failed (error=%lu)",
                  updater_src.c_str(), updater_dst.c_str(), GetLastError());
        return std::string();
    }

    /* Copy every *.dll next to the updater so it can load its dependencies
     * from staging instead of from the target directory being overwritten. */
    std::string pattern = src_dir + "\\*.dll";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string dll_src = src_dir + "\\" + fd.cFileName;
            std::string dll_dst = staging_dir + "\\" + fd.cFileName;
            if (!CopyFileA(dll_src.c_str(), dll_dst.c_str(), FALSE)) {
                LOG_WARN("ag_apply_update: copy DLL %s failed (error=%lu)",
                         fd.cFileName, GetLastError());
            } else {
                LOG_TRACE("ag_apply_update: staged %s", fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    return staging_dir;
}

/* Quote argument for Windows command line (simple: wrap in quotes, escape \"). */
static std::string win_quote(const std::string &s)
{
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') out += '\\';
        out += s[i];
    }
    out += "\"";
    return out;
}
#endif

ag_error_t ag_apply_update(const char *zip_path, const char *launch_after)
{
    if (!zip_path || !zip_path[0]) {
        LOG_ERROR("ag_apply_update: invalid zip_path");
        return AG_ERR_INTERNAL;
    }

    LOG_INFO("ag_apply_update: applying update from %s", zip_path);
    if (launch_after && launch_after[0]) {
        LOG_INFO("ag_apply_update: will launch '%s' after update", launch_after);
    }

    std::string exe_dir = get_exe_dir();

#ifdef _WIN32
    std::string original_updater = exe_dir + "\\" + AG_UPDATER_NAME + ".exe";
    if (GetFileAttributesA(original_updater.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_ERROR("ag_apply_update: updater not found: %s", original_updater.c_str());
        return AG_ERR_NOT_FOUND;
    }

    /* Stage updater + DLLs to temp so extraction into exe_dir can overwrite
     * libwinpthread-1.dll etc. without "File locked" errors. */
    std::string staging_dir = stage_updater_files(exe_dir);
    if (staging_dir.empty()) {
        LOG_ERROR("ag_apply_update: failed to stage updater files");
        return AG_ERR_IO;
    }
    std::string staged_updater = staging_dir + "\\" + AG_UPDATER_NAME + ".exe";

    /* Build command line: staged updater extracts into original exe_dir. */
    std::string cmd_args = win_quote(staged_updater) + " " +
                           win_quote(zip_path) + " --target " +
                           win_quote(exe_dir);
    if (launch_after && launch_after[0]) {
        cmd_args += " --launch " + win_quote(launch_after);
    }
    LOG_DEBUG("ag_apply_update: cmd=%s", cmd_args.c_str());

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(staged_updater.c_str(), &cmd_args[0],
                        NULL, NULL, FALSE,
                        0, NULL, staging_dir.c_str(), &si, &pi)) {
        LOG_ERROR("ag_apply_update: CreateProcessA failed (error=%lu)", GetLastError());
        return AG_ERR_IO;
    }

    LOG_INFO("ag_apply_update: staged updater launched (pid=%lu) from %s",
             pi.dwProcessId, staging_dir.c_str());
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return AG_OK;
#else
    std::string updater_path = exe_dir + "/" + AG_UPDATER_NAME;
    if (access(updater_path.c_str(), X_OK) != 0) {
        LOG_ERROR("ag_apply_update: updater not found or not executable: %s", updater_path.c_str());
        return AG_ERR_NOT_FOUND;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("ag_apply_update: fork() failed");
        return AG_ERR_IO;
    }
    if (pid > 0) {
        LOG_INFO("ag_apply_update: updater process launched (pid=%d)", pid);
    }
    if (pid == 0) {
        if (launch_after && launch_after[0]) {
            execl(updater_path.c_str(), updater_path.c_str(),
                  zip_path, "--launch", launch_after, NULL);
        } else {
            execl(updater_path.c_str(), updater_path.c_str(), zip_path, NULL);
        }
        _exit(1);
    }
    return AG_OK;
#endif
}

#ifdef _WIN32
/* Read from pipe, break into lines, invoke log_cb per line.
 * After pipe closes, wait for process, invoke done_cb with exit code. */
static void pipe_reader_thread(HANDLE pipe_read, HANDLE process,
                                ag_update_log_cb_t log_cb,
                                ag_update_done_cb_t done_cb, void *ud)
{
    std::string buffer;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(pipe_read, buf, sizeof(buf), &n, NULL) && n > 0) {
        buffer.append(buf, n);
        size_t start = 0;
        for (size_t i = 0; i < buffer.size(); ++i) {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                if (i > start) {
                    std::string line = buffer.substr(start, i - start);
                    if (log_cb) log_cb(line.c_str(), ud);
                }
                start = i + 1;
            }
        }
        buffer.erase(0, start);
    }
    if (!buffer.empty() && log_cb) log_cb(buffer.c_str(), ud);

    CloseHandle(pipe_read);

    DWORD exit_code = 1;
    WaitForSingleObject(process, INFINITE);
    GetExitCodeProcess(process, &exit_code);
    CloseHandle(process);

    if (done_cb) done_cb((int)exit_code, ud);
}
#endif

ag_error_t ag_apply_update_with_log(const char *zip_path,
                                     const char *launch_after,
                                     ag_update_log_cb_t log_cb,
                                     ag_update_done_cb_t done_cb,
                                     void *user_data)
{
    if (!zip_path || !zip_path[0]) {
        LOG_ERROR("ag_apply_update_with_log: invalid zip_path");
        return AG_ERR_INTERNAL;
    }

#ifdef _WIN32
    std::string exe_dir = get_exe_dir();
    std::string original_updater = exe_dir + "\\" + AG_UPDATER_NAME + ".exe";
    if (GetFileAttributesA(original_updater.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_ERROR("ag_apply_update_with_log: updater not found: %s",
                  original_updater.c_str());
        return AG_ERR_NOT_FOUND;
    }

    std::string staging_dir = stage_updater_files(exe_dir);
    if (staging_dir.empty()) return AG_ERR_IO;
    std::string staged_updater = staging_dir + "\\" + AG_UPDATER_NAME + ".exe";

    /* Inheritable anonymous pipe so child's stdout/stderr flow back here. */
    HANDLE pipe_read = NULL, pipe_write = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
        LOG_ERROR("ag_apply_update_with_log: CreatePipe failed (error=%lu)",
                  GetLastError());
        return AG_ERR_IO;
    }
    /* Parent's read end must NOT be inherited by child. */
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

    std::string cmd_args = win_quote(staged_updater) + " " +
                           win_quote(zip_path) + " --target " +
                           win_quote(exe_dir);
    if (launch_after && launch_after[0]) {
        cmd_args += " --launch " + win_quote(launch_after);
    }
    LOG_DEBUG("ag_apply_update_with_log: cmd=%s", cmd_args.c_str());

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_write;
    si.hStdError  = pipe_write;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(staged_updater.c_str(), &cmd_args[0],
                        NULL, NULL, TRUE,  /* bInheritHandles=TRUE */
                        CREATE_NO_WINDOW, NULL, staging_dir.c_str(),
                        &si, &pi)) {
        LOG_ERROR("ag_apply_update_with_log: CreateProcessA failed (error=%lu)",
                  GetLastError());
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return AG_ERR_IO;
    }

    /* Parent must close its write end so the pipe breaks when child exits. */
    CloseHandle(pipe_write);
    CloseHandle(pi.hThread);

    LOG_INFO("ag_apply_update_with_log: staged updater launched (pid=%lu)",
             pi.dwProcessId);

    std::thread reader(pipe_reader_thread, pipe_read, pi.hProcess,
                       log_cb, done_cb, user_data);
    reader.detach();
    return AG_OK;
#else
    (void)launch_after; (void)log_cb; (void)done_cb; (void)user_data;
    LOG_ERROR("ag_apply_update_with_log: not implemented on this platform");
    return AG_ERR_INTERNAL;
#endif
}

ag_error_t ag_cleanup_temp(void)
{
    std::string dir = get_app_temp_dir();
    if (dir.empty()) return AG_ERR_IO;
    /* Strip trailing separator for cleaner log output. */
    std::string dir_noslash = dir;
    while (!dir_noslash.empty() &&
           (dir_noslash.back() == '\\' || dir_noslash.back() == '/')) {
        dir_noslash.pop_back();
    }
    LOG_INFO("cleanup: scanning %s", dir_noslash.c_str());

    int files_removed = 0, dirs_removed = 0;

#ifdef _WIN32
    std::string pattern = dir_noslash + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_INFO("cleanup: nothing to remove in %s", dir_noslash.c_str());
        return AG_OK;
    }
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string child = dir_noslash + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_dir_recursive(child, files_removed, dirs_removed);
        } else {
            if (!DeleteFileA(child.c_str())) {
                SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileA(child.c_str())) {
                    LOG_WARN("cleanup: cannot remove file %s (error=%lu)",
                             child.c_str(),
                             (unsigned long)GetLastError());
                    continue;
                }
            }
            LOG_INFO("cleanup: removed file %s", child.c_str());
            ++files_removed;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir_noslash.c_str());
    if (!d) return AG_OK;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string child = dir_noslash + "/" + name;
        if (unlink(child.c_str()) == 0) {
            LOG_INFO("cleanup: removed %s", child.c_str());
            ++files_removed;
        }
    }
    closedir(d);
#endif

    LOG_INFO("cleanup: done for %s — removed %d file(s), %d dir(s)",
             dir_noslash.c_str(), files_removed, dirs_removed);

#ifdef _WIN32
    /* Also sweep stale .old-<tick> rename backups next to the caller's exe.
     * These accumulate when a previous run could not delete a still-loaded
     * DLL; by now the prior process has exited and the file is free. */
    std::string exe_dir = get_exe_dir();
    int backup_removed = cleanup_old_backups(exe_dir);
    if (backup_removed > 0) {
        LOG_INFO("cleanup: removed %d stale backup file(s) in %s",
                 backup_removed, exe_dir.c_str());
    }
#endif

    return AG_OK;
}
