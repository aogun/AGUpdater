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

ag_error_t ag_apply_update(
    const char *zip_path
);

#ifdef __cplusplus
}
#endif

#endif /* AG_UPDATER_H */
