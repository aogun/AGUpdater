#ifndef AG_SERVER_AUTH_H
#define AG_SERVER_AUTH_H

#include <string>
#include <map>
#include <vector>
#include <ctime>

/* Compute SHA256 hex string */
std::string sha256_hex(const std::string &input);

/* Compute HMAC-SHA256 hex string */
std::string hmac_sha256_hex(const std::string &key, const std::string &msg);

/* Generate random hex string of specified byte length (output is 2*bytes chars) */
std::string random_hex(int bytes);

/* Admin authentication manager */
class AdminAuth {
public:
    AdminAuth();

    /* Set the password_hash from config (SHA256 of plaintext password) */
    void set_password_hash(const std::string &password_hash);

    /* Generate a nonce for challenge-response login. Valid for 60 seconds. */
    std::string generate_nonce();

    /* Verify login: check nonce validity, compute HMAC, compare sign.
     * On success, returns a session token. On failure, returns empty string. */
    std::string verify_login(const std::string &username,
                             const std::string &nonce,
                             const std::string &sign,
                             const std::string &expected_username);

    /* Verify a session token. Returns true if valid and not expired. */
    bool verify_token(const std::string &token);

private:
    std::string password_hash_;

    struct NonceEntry {
        time_t created_at;
        bool used;
    };
    std::map<std::string, NonceEntry> nonces_;

    struct TokenEntry {
        time_t expires_at;
    };
    std::map<std::string, TokenEntry> tokens_;

    void cleanup_expired_nonces();
    void cleanup_expired_tokens();
};

/* Client HMAC authentication verifier */
class ClientAuth {
public:
    ClientAuth();

    void set_secret(const std::string &secret);

    /* Verify X-Auth header JSON.
     * Checks timestamp (±10 min), nonce uniqueness, HMAC signature.
     * On success, sets device_id and returns true. */
    bool verify_xauth(const std::string &xauth_json,
                      std::string &device_id,
                      std::string &err_msg);

private:
    std::string secret_;
    std::vector<std::string> recent_nonces_;
    static const int MAX_RECENT_NONCES = 100;
    static const int TIMESTAMP_TOLERANCE = 600; /* 10 minutes in seconds */
};

#endif /* AG_SERVER_AUTH_H */
