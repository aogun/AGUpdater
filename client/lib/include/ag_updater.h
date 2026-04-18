#ifndef AG_UPDATER_H
#define AG_UPDATER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version info */
typedef struct ag_version_info {
    char version[32];
    char description[1024];
    char download_url[512];
    int64_t file_size;
    char file_sha256[65];
    char created_at[32];
} ag_version_info_t;

/* Error codes */
typedef enum ag_error {
    AG_OK = 0,
    AG_ERR_NETWORK = -1,
    AG_ERR_AUTH = -2,
    AG_ERR_NOT_FOUND = -3,
    AG_ERR_CHECKSUM = -4,
    AG_ERR_IO = -5,
    AG_ERR_INTERNAL = -6,
    AG_ERR_NO_UPDATE = -7
} ag_error_t;

/* Download progress */
typedef struct ag_download_progress {
    int64_t total_bytes;
    int64_t downloaded_bytes;
    int percent;
} ag_download_progress_t;

/* Callbacks */
typedef void (*ag_check_callback)(
    ag_error_t error,
    const ag_version_info_t *info,
    int update_count,
    void *user_data
);

typedef void (*ag_download_callback)(
    ag_error_t error,
    const ag_download_progress_t *progress,
    const char *file_path,
    void *user_data
);

/* API functions */
ag_error_t ag_check_update(
    const char *app_name,
    const char *current_version,
    ag_check_callback callback,
    void *user_data
);

ag_error_t ag_download_update(
    const ag_version_info_t *info,
    ag_download_callback callback,
    void *user_data
);

/**
 * @param zip_path       下载的 zip 文件路径
 * @param launch_after   更新完成后要启动的程序名（可为 NULL 表示不启动）
 */
ag_error_t ag_apply_update(
    const char *zip_path,
    const char *launch_after
);

/* Log callback: invoked on a worker thread for each line from ag-updater
 * stdout/stderr (no trailing newline). */
typedef void (*ag_update_log_cb_t)(const char *line, void *user_data);

/* Done callback: invoked on the same worker thread after ag-updater exits.
 * exit_code is the child process exit code (0 = success). */
typedef void (*ag_update_done_cb_t)(int exit_code, void *user_data);

/**
 * Launch ag-updater and stream its stdout/stderr to the given callback.
 * Returns AG_OK immediately once the child is spawned; the worker thread
 * owns the lifetime of the pipe read + child wait + callbacks.
 */
ag_error_t ag_apply_update_with_log(
    const char *zip_path,
    const char *launch_after,
    ag_update_log_cb_t log_cb,
    ag_update_done_cb_t done_cb,
    void *user_data
);

/**
 * Remove leftover downloads and staging dirs for the current installation.
 *
 * Files are kept under a per-install subdirectory of the system temp folder,
 * named `ag-updater-<md5(exe_dir)>`, so different install paths do not share
 * temp space. Applications integrating ag-update-lib should call this at
 * startup (e.g. from main()) to prevent accumulation across updates.
 *
 * Actions and paths are logged at INFO level.
 */
ag_error_t ag_cleanup_temp(void);

#ifdef __cplusplus
}
#endif

#endif /* AG_UPDATER_H */
