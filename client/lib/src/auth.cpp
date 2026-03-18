#include "auth.h"
#include "log.h"
#include "cJSON.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef AG_SECRET
#define AG_SECRET "default_secret"
#endif

std::string ag_get_device_id()
{
#ifdef _WIN32
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        std::string id(buf, size);
        LOG_DEBUG("ag_get_device_id: %s", id.c_str());
        return id;
    }
    LOG_DEBUG("ag_get_device_id: fallback to UNKNOWN");
    return std::string("UNKNOWN");
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        LOG_DEBUG("ag_get_device_id: %s", buf);
        return std::string(buf);
    }
    LOG_DEBUG("ag_get_device_id: fallback to UNKNOWN");
    return std::string("UNKNOWN");
#endif
}

std::string ag_hmac_sha256_hex(const std::string &key, const std::string &msg)
{
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;

    HMAC(EVP_sha256(),
         key.c_str(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(msg.c_str()),
         msg.size(),
         result, &result_len);

    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    for (unsigned int i = 0; i < result_len; ++i) {
        snprintf(hex + i * 2, 3, "%02x", result[i]);
    }
    LOG_TRACE("ag_hmac_sha256_hex: msg_len=%zu, result_len=%u", msg.size(), result_len);
    return std::string(hex, result_len * 2);
}

std::string ag_sha256_hex(const char *data, size_t len)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data), len, hash);
    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return std::string(hex, SHA256_DIGEST_LENGTH * 2);
}

std::string ag_random_hex(int bytes)
{
    std::vector<unsigned char> buf(bytes);
    RAND_bytes(&buf[0], bytes);
    std::string hex;
    hex.reserve(bytes * 2);
    char tmp[3];
    for (int i = 0; i < bytes; ++i) {
        snprintf(tmp, sizeof(tmp), "%02x", buf[i]);
        hex.append(tmp, 2);
    }
    return hex;
}

std::string ag_generate_xauth()
{
    std::string device_id = ag_get_device_id();
    time_t timestamp = time(NULL);
    std::string nonce = ag_random_hex(8); /* 16 hex chars */

    /* Build message: device_id + timestamp_str + nonce */
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%lld", (long long)timestamp);
    std::string msg = device_id + std::string(ts_buf) + nonce;

    /* Compute HMAC-SHA256 sign */
    std::string sign = ag_hmac_sha256_hex(AG_SECRET, msg);

    /* Build JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(timestamp));
    cJSON_AddStringToObject(root, "nonce", nonce.c_str());
    cJSON_AddStringToObject(root, "sign", sign.c_str());

    char *str = cJSON_PrintUnformatted(root);
    std::string result(str);
    free(str);
    cJSON_Delete(root);
    LOG_DEBUG("ag_generate_xauth: device_id=%s, timestamp=%lld, nonce=%s",
              device_id.c_str(), (long long)timestamp, nonce.c_str());
    return result;
}
