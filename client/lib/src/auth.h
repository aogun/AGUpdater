#ifndef AG_CLIENT_AUTH_H
#define AG_CLIENT_AUTH_H

#include <string>

/* Generate X-Auth JSON header value for client API requests.
 * Uses AG_SECRET compile-time constant as HMAC key.
 * Returns JSON string: {"device_id":"...","timestamp":...,"nonce":"...","sign":"..."} */
std::string ag_generate_xauth();

/* Get machine/device ID (hostname) */
std::string ag_get_device_id();

/* Compute HMAC-SHA256 hex string */
std::string ag_hmac_sha256_hex(const std::string &key, const std::string &msg);

/* Compute SHA256 of a data buffer, return hex string */
std::string ag_sha256_hex(const char *data, size_t len);

/* Generate random hex string (bytes*2 hex chars) */
std::string ag_random_hex(int bytes);

#endif /* AG_CLIENT_AUTH_H */
