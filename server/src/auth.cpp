#include "auth.h"
#include "cJSON.h"
#include "log.h"

#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

/* ---------- Utility functions ---------- */

std::string sha256_hex(const std::string &input)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(input.c_str()),
           input.size(), hash);

    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return std::string(hex, SHA256_DIGEST_LENGTH * 2);
}

std::string hmac_sha256_hex(const std::string &key, const std::string &msg)
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
    return std::string(hex, result_len * 2);
}

std::string random_hex(int bytes)
{
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(&buf[0], bytes) != 1) {
        LOG_ERROR("RAND_bytes failed to generate %d random bytes", bytes);
        return std::string();
    }

    std::string hex;
    hex.reserve(bytes * 2);
    char tmp[3];
    for (int i = 0; i < bytes; ++i) {
        snprintf(tmp, sizeof(tmp), "%02x", buf[i]);
        hex.append(tmp, 2);
    }
    return hex;
}

/* ---------- AdminAuth ---------- */

AdminAuth::AdminAuth() {}

void AdminAuth::set_password_hash(const std::string &password_hash)
{
    password_hash_ = password_hash;
    LOG_DEBUG("Admin password hash configured");
}

void AdminAuth::cleanup_expired_nonces()
{
    time_t now = time(NULL);
    int removed = 0;
    std::map<std::string, NonceEntry>::iterator it = nonces_.begin();
    while (it != nonces_.end()) {
        if (it->second.used || difftime(now, it->second.created_at) > 60.0) {
            nonces_.erase(it++);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        LOG_TRACE("Cleaned up %d expired/used nonces, %zu remaining",
                  removed, nonces_.size());
    }
}

void AdminAuth::cleanup_expired_tokens()
{
    time_t now = time(NULL);
    int removed = 0;
    std::map<std::string, TokenEntry>::iterator it = tokens_.begin();
    while (it != tokens_.end()) {
        if (difftime(now, it->second.expires_at) > 0.0) {
            tokens_.erase(it++);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        LOG_TRACE("Cleaned up %d expired tokens, %zu remaining",
                  removed, tokens_.size());
    }
}

std::string AdminAuth::generate_nonce()
{
    cleanup_expired_nonces();

    std::string nonce = random_hex(8); /* 16 hex chars */
    NonceEntry entry;
    entry.created_at = time(NULL);
    entry.used = false;
    nonces_[nonce] = entry;
    LOG_DEBUG("Generated nonce: %s (active nonces: %zu)", nonce.c_str(), nonces_.size());
    return nonce;
}

std::string AdminAuth::verify_login(const std::string &username,
                                    const std::string &nonce,
                                    const std::string &sign,
                                    const std::string &expected_username)
{
    LOG_TRACE("verify_login() enter, username=%s", username.c_str());

    /* Check username */
    if (username != expected_username) {
        LOG_WARN("Login failed: invalid username '%s'", username.c_str());
        return std::string();
    }

    /* Check nonce exists and is unused */
    std::map<std::string, NonceEntry>::iterator it = nonces_.find(nonce);
    if (it == nonces_.end()) {
        LOG_WARN("Login failed: nonce not found");
        return std::string();
    }
    if (it->second.used) {
        LOG_WARN("Login failed: nonce already used");
        nonces_.erase(it);
        return std::string();
    }

    /* Check nonce not expired (60 seconds) */
    time_t now = time(NULL);
    if (difftime(now, it->second.created_at) > 60.0) {
        LOG_WARN("Login failed: nonce expired");
        nonces_.erase(it);
        return std::string();
    }

    /* Mark nonce as used immediately */
    it->second.used = true;

    /* Compute expected sign: HMAC-SHA256(key=password_hash, msg=nonce) */
    std::string expected_sign = hmac_sha256_hex(password_hash_, nonce);
    if (sign.size() != expected_sign.size() ||
        CRYPTO_memcmp(sign.c_str(), expected_sign.c_str(), sign.size()) != 0) {
        LOG_WARN("Login failed: signature mismatch for user '%s'", username.c_str());
        return std::string();
    }

    /* Generate session token */
    cleanup_expired_tokens();
    std::string token = random_hex(16); /* 32 hex chars */
    TokenEntry te;
    te.expires_at = now + 24 * 3600; /* 24 hours */
    tokens_[token] = te;
    LOG_INFO("Admin login successful for user '%s' (active tokens: %zu)",
             username.c_str(), tokens_.size());
    return token;
}

bool AdminAuth::verify_token(const std::string &token)
{
    LOG_TRACE("verify_token() enter");

    if (token.empty()) {
        LOG_DEBUG("Token verification failed: empty token");
        return false;
    }

    std::map<std::string, TokenEntry>::iterator it = tokens_.find(token);
    if (it == tokens_.end()) {
        LOG_DEBUG("Token verification failed: token not found");
        return false;
    }

    time_t now = time(NULL);
    if (difftime(now, it->second.expires_at) > 0.0) {
        LOG_DEBUG("Token verification failed: token expired");
        tokens_.erase(it);
        return false;
    }

    LOG_TRACE("Token verification successful");
    return true;
}

/* ---------- ClientAuth ---------- */

ClientAuth::ClientAuth() {}

void ClientAuth::set_secret(const std::string &secret)
{
    secret_ = secret;
    LOG_DEBUG("Client auth secret configured");
}

bool ClientAuth::verify_xauth(const std::string &xauth_json,
                               std::string &device_id,
                               std::string &err_msg)
{
    LOG_TRACE("verify_xauth() enter");

    if (xauth_json.empty()) {
        err_msg = "missing X-Auth header";
        LOG_DEBUG("X-Auth verification failed: missing header");
        return false;
    }

    cJSON *root = cJSON_Parse(xauth_json.c_str());
    if (root == NULL) {
        err_msg = "invalid X-Auth JSON";
        LOG_WARN("X-Auth verification failed: invalid JSON");
        return false;
    }

    /* Extract fields */
    const cJSON *j_device_id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *j_timestamp = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    const cJSON *j_nonce = cJSON_GetObjectItemCaseSensitive(root, "nonce");
    const cJSON *j_sign = cJSON_GetObjectItemCaseSensitive(root, "sign");

    if (!cJSON_IsString(j_device_id) || !cJSON_IsNumber(j_timestamp) ||
        !cJSON_IsString(j_nonce) || !cJSON_IsString(j_sign)) {
        cJSON_Delete(root);
        err_msg = "invalid X-Auth fields";
        LOG_WARN("X-Auth verification failed: invalid fields");
        return false;
    }

    std::string dev_id = j_device_id->valuestring;
    long long timestamp = static_cast<long long>(j_timestamp->valuedouble);
    std::string nonce = j_nonce->valuestring;
    std::string sign = j_sign->valuestring;
    cJSON_Delete(root);

    /* 1. Timestamp check: within +-10 minutes */
    time_t now = time(NULL);
    long long diff = static_cast<long long>(now) - timestamp;
    if (diff < 0) diff = -diff;
    if (diff > TIMESTAMP_TOLERANCE) {
        err_msg = "timestamp out of range";
        LOG_WARN("X-Auth verification failed: timestamp out of range (diff=%lld) for device '%s'",
                 diff, dev_id.c_str());
        return false;
    }

    /* 2. Nonce uniqueness check */
    for (size_t i = 0; i < recent_nonces_.size(); ++i) {
        if (recent_nonces_[i] == nonce) {
            err_msg = "duplicate nonce";
            LOG_WARN("X-Auth verification failed: duplicate nonce for device '%s'",
                     dev_id.c_str());
            return false;
        }
    }

    /* Add nonce to recent list, evict oldest if over limit */
    recent_nonces_.push_back(nonce);
    if (static_cast<int>(recent_nonces_.size()) > MAX_RECENT_NONCES) {
        recent_nonces_.erase(recent_nonces_.begin());
    }

    /* 3. HMAC signature check */
    /* msg = device_id + timestamp_str + nonce */
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%lld", timestamp);
    std::string msg = dev_id + std::string(ts_buf) + nonce;
    std::string expected_sign = hmac_sha256_hex(secret_, msg);

    if (sign.size() != expected_sign.size() ||
        CRYPTO_memcmp(sign.c_str(), expected_sign.c_str(), sign.size()) != 0) {
        err_msg = "signature mismatch";
        LOG_WARN("X-Auth verification failed: HMAC signature mismatch for device '%s'",
                 dev_id.c_str());
        return false;
    }

    device_id = dev_id;
    LOG_DEBUG("X-Auth verification successful for device '%s'", dev_id.c_str());
    return true;
}
