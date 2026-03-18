#ifndef AG_HTTP_CLIENT_H
#define AG_HTTP_CLIENT_H

#include <string>
#include <functional>

/* HTTP response result */
struct HttpResult {
    int status_code;
    std::string body;
    bool ok; /* true if request succeeded (any HTTP status) */
    std::string error; /* error message if !ok */
};

/* Progress callback: (downloaded_bytes, total_bytes) -> bool (return false to cancel) */
typedef std::function<bool(int64_t, int64_t)> ProgressCallback;

/* Perform GET request with X-Auth header. Returns parsed result. */
HttpResult http_get(const std::string &path);

/* Perform GET request to download a file, writing to disk.
 * Calls progress_cb periodically. Returns true on success. */
bool http_download(const std::string &path,
                   const std::string &dest_file,
                   int64_t expected_size,
                   ProgressCallback progress_cb,
                   std::string &err_msg);

#endif /* AG_HTTP_CLIENT_H */
