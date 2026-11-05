#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace so {

enum class Role { SuperAdmin, Admin, User };

const char* to_string(Role r);
std::optional<Role> role_from_string(const std::string& s);

struct Claims {
    std::int64_t user_id{0};
    std::int64_t tenant_id{0};      ///< 0 for a SuperAdmin, who is not in a tenant
    Role role{Role::User};
    std::int64_t employee_id{0};    ///< links a User to their own attendance
    std::int64_t expires_at{0};     ///< unix seconds
};

/**
 * Password hashing — PBKDF2-HMAC-SHA512, 210 000 iterations (OWASP 2023).
 *
 * The stored form carries its own parameters:
 *
 *     $pbkdf2-sha512$<iterations>$<base64 salt>$<base64 hash>
 *
 * so the cost can be raised, or the algorithm replaced with Argon2id, without
 * invalidating a single existing password: `verify_password` reads what it is
 * given rather than assuming today's settings.
 *
 * Chosen over bcrypt deliberately. bcrypt silently truncates at 72 bytes, which
 * turns a long passphrase into a shorter secret without telling anybody, and it
 * would have meant a dependency that could not be built or tested on this host.
 * PBKDF2 via OpenSSL is available everywhere this will run.
 */
std::string hash_password(const std::string& password);
bool verify_password(const std::string& password, const std::string& stored);

/// True when `stored` was produced with weaker parameters than today's, so the
/// caller can transparently re-hash on next successful login.
bool needs_rehash(const std::string& stored);

/**
 * JWT, HS256.
 *
 * `verify_token` returns an optional and has NO branch that invents a user.
 * The sibling project shipped a `get_current_user` that returned a synthetic
 * admin whenever no token was present, and because every route shared that one
 * dependency, every route in that API answered an anonymous caller with real
 * data while looking protected. There is no such path here: no token, no
 * claims, no exceptions.
 */
std::string issue_token(const Claims& c, const std::string& secret,
                        int lifetime_seconds = 3600);
std::optional<Claims> verify_token(const std::string& token, const std::string& secret,
                                   std::int64_t now_unix = 0);

/// base64url helpers, exposed for tests.
std::string b64url_encode(const std::string& raw);
std::string b64url_decode(const std::string& enc);

}  // namespace so
