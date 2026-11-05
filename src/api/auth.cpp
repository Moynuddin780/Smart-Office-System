#include "api/auth.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>

namespace so {
namespace {

constexpr int kPbkdf2Iterations = 210000;   // OWASP 2023 for PBKDF2-HMAC-SHA512
constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 64;
const char* kAlgo = "pbkdf2-sha512";

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64url(const unsigned char* data, std::size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                           (i + 1 < n ? static_cast<unsigned>(data[i + 1]) << 8 : 0u) |
                           (i + 2 < n ? static_cast<unsigned>(data[i + 2]) : 0u);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        if (i + 1 < n) out.push_back(kB64[(v >> 6) & 63]);
        if (i + 2 < n) out.push_back(kB64[v & 63]);
    }
    return out;
}

int b64val(char c) {
    const char* p = std::strchr(kB64, c);
    return p ? static_cast<int>(p - kB64) : -1;
}

/// Constant-time comparison. A plain == leaks how many leading bytes matched,
/// which is enough to forge a signature one byte at a time.
bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

std::string hmac_sha256(const std::string& key, const std::string& msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), out, &len);
    return b64url(out, len);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, sep)) out.push_back(cur);
    return out;
}

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

}  // namespace

const char* to_string(Role r) {
    switch (r) {
        case Role::SuperAdmin: return "superadmin";
        case Role::Admin: return "admin";
        case Role::User: return "user";
    }
    return "user";
}

std::optional<Role> role_from_string(const std::string& s) {
    if (s == "superadmin") return Role::SuperAdmin;
    if (s == "admin") return Role::Admin;
    if (s == "user") return Role::User;
    return std::nullopt;
}

std::string b64url_encode(const std::string& raw) {
    return b64url(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

std::string b64url_decode(const std::string& enc) {
    std::string out;
    int buf = 0, bits = 0;
    for (char c : enc) {
        const int v = b64val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::string hash_password(const std::string& password) {
    unsigned char salt[kSaltBytes];
    if (RAND_bytes(salt, kSaltBytes) != 1)
        throw std::runtime_error("no entropy available for a password salt");

    unsigned char out[kHashBytes];
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt, kSaltBytes, kPbkdf2Iterations, EVP_sha512(),
                          kHashBytes, out) != 1)
        throw std::runtime_error("PBKDF2 failed");

    std::ostringstream s;
    s << '$' << kAlgo << '$' << kPbkdf2Iterations << '$'
      << b64url(salt, kSaltBytes) << '$' << b64url(out, kHashBytes);
    return s.str();
}

bool verify_password(const std::string& password, const std::string& stored) {
    // Parameters come from the stored value, not from today's constants, so a
    // cost increase never locks anybody out of their own account.
    const auto parts = split(stored, '$');
    if (parts.size() != 5 || parts[1] != kAlgo) return false;

    int iterations = 0;
    try {
        iterations = std::stoi(parts[2]);
    } catch (const std::exception&) {
        return false;
    }
    if (iterations < 1000 || iterations > 10000000) return false;

    const std::string salt = b64url_decode(parts[3]);
    const std::string expect = parts[4];
    if (salt.empty()) return false;

    unsigned char out[kHashBytes];
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()),
                          static_cast<int>(salt.size()), iterations, EVP_sha512(),
                          kHashBytes, out) != 1)
        return false;

    return ct_equal(expect, b64url(out, kHashBytes));
}

bool needs_rehash(const std::string& stored) {
    const auto parts = split(stored, '$');
    if (parts.size() != 5 || parts[1] != kAlgo) return true;
    try {
        return std::stoi(parts[2]) < kPbkdf2Iterations;
    } catch (const std::exception&) {
        return true;
    }
}

std::string issue_token(const Claims& c, const std::string& secret, int lifetime_seconds) {
    const std::int64_t exp = (c.expires_at > 0) ? c.expires_at
                                                : now_seconds() + lifetime_seconds;
    std::ostringstream payload;
    payload << R"({"uid":)" << c.user_id
            << R"(,"tid":)" << c.tenant_id
            << R"(,"role":")" << to_string(c.role)
            << R"(","eid":)" << c.employee_id
            << R"(,"exp":)" << exp << '}';

    const std::string head = b64url_encode(R"({"alg":"HS256","typ":"JWT"})");
    const std::string body = b64url_encode(payload.str());
    const std::string signing_input = head + "." + body;
    return signing_input + "." + hmac_sha256(secret, signing_input);
}

namespace {

/// Minimal field reader for the fixed payload shape issue_token writes. A full
/// JSON parser is not warranted for a document this code produced itself, and
/// the signature has already been verified before this runs.
bool read_int(const std::string& json, const std::string& key, std::int64_t& out) {
    const std::string needle = "\"" + key + "\":";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    try {
        out = std::stoll(json.substr(pos + needle.size()));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool read_str(const std::string& json, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\":\"";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    const auto start = pos + needle.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos) return false;
    out = json.substr(start, end - start);
    return true;
}

}  // namespace

std::optional<Claims> verify_token(const std::string& token, const std::string& secret,
                                   std::int64_t now_unix) {
    if (token.empty() || secret.empty()) return std::nullopt;

    const auto parts = split(token, '.');
    if (parts.size() != 3) return std::nullopt;

    const std::string signing_input = parts[0] + "." + parts[1];
    if (!ct_equal(parts[2], hmac_sha256(secret, signing_input))) return std::nullopt;

    // Only accept the algorithm we issue. Without this check a token with
    // "alg":"none" — the classic JWT forgery — would be honoured.
    const std::string header = b64url_decode(parts[0]);
    std::string alg;
    if (!read_str(header, "alg", alg) || alg != "HS256") return std::nullopt;

    const std::string payload = b64url_decode(parts[1]);
    Claims c;
    std::string role;
    if (!read_int(payload, "uid", c.user_id)) return std::nullopt;
    if (!read_int(payload, "tid", c.tenant_id)) return std::nullopt;
    if (!read_int(payload, "exp", c.expires_at)) return std::nullopt;
    read_int(payload, "eid", c.employee_id);
    if (!read_str(payload, "role", role)) return std::nullopt;
    const auto r = role_from_string(role);
    if (!r) return std::nullopt;
    c.role = *r;

    const std::int64_t now = now_unix > 0 ? now_unix : now_seconds();
    if (c.expires_at <= now) return std::nullopt;

    return c;
}

}  // namespace so
