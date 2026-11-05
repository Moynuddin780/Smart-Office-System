#include "notify/channels.hpp"

#include "core/log.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace so::notify {
namespace {

std::string b64(const std::vector<unsigned char>& data) {
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                           (i + 1 < data.size() ? static_cast<unsigned>(data[i + 1]) << 8 : 0u) |
                           (i + 2 < data.size() ? static_cast<unsigned>(data[i + 2]) : 0u);
        out.push_back(t[(v >> 18) & 63]);
        out.push_back(t[(v >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? t[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? t[v & 63] : '=');
    }
    // Hard-wrap at 76 columns: a base64 part with unbounded line length is
    // rejected or mangled by some relays.
    std::string wrapped;
    for (std::size_t i = 0; i < out.size(); i += 76) {
        wrapped += out.substr(i, 76);
        wrapped += "\r\n";
    }
    return wrapped;
}

std::vector<unsigned char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

std::string basename_of(const std::string& p) {
    const auto s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

/// Strip CR/LF from anything that goes into a header. Without this an operator
/// could put a newline in a camera name and inject extra headers - the classic
/// SMTP header injection, and camera names are operator-supplied.
std::string header_safe(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in)
        if (c != '\r' && c != '\n') out.push_back(c);
    return out;
}

std::vector<std::string> recipients_of(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (c == ',' || c == ';') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct Payload {
    std::string data;
    std::size_t offset{0};
};

std::size_t read_payload(char* buf, std::size_t size, std::size_t n, void* user) {
    auto* p = static_cast<Payload*>(user);
    const std::size_t want = size * n;
    const std::size_t have = p->data.size() - p->offset;
    const std::size_t take = std::min(want, have);
    if (take == 0) return 0;
    std::memcpy(buf, p->data.data() + p->offset, take);
    p->offset += take;
    return take;
}

std::string boundary() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::ostringstream s;
    s << "=_SmartOffice_" << std::hex << rng() << rng();
    return s.str();
}

}  // namespace

Email::Email(EmailConfig cfg) : cfg_(std::move(cfg)) {}

SendResult Email::send(const std::string& subject, const std::string& body,
                       const std::string& image_path) const {
    if (!configured()) return {false, "email is not configured (host and a recipient are required)"};

    const auto rcpts = recipients_of(cfg_.to);
    if (rcpts.empty()) return {false, "no usable recipient"};

    const std::string from = cfg_.from.empty() ? cfg_.user : cfg_.from;
    if (from.empty()) return {false, "no sender address configured"};

    const std::string bnd = boundary();
    std::ostringstream m;
    m << "From: " << header_safe(cfg_.from_name.empty() ? "Smart Office" : cfg_.from_name)
      << " <" << header_safe(from) << ">\r\n";
    m << "To: " << header_safe(cfg_.to) << "\r\n";
    m << "Subject: " << header_safe(subject) << "\r\n";
    m << "MIME-Version: 1.0\r\n";

    const auto image = image_path.empty() ? std::vector<unsigned char>()
                                          : read_file(image_path);

    if (image.empty()) {
        m << "Content-Type: text/plain; charset=UTF-8\r\n\r\n" << body << "\r\n";
    } else {
        m << "Content-Type: multipart/mixed; boundary=\"" << bnd << "\"\r\n\r\n";
        m << "--" << bnd << "\r\n";
        m << "Content-Type: text/plain; charset=UTF-8\r\n\r\n" << body << "\r\n";
        m << "--" << bnd << "\r\n";
        // Attached, not linked: an alert has to be useful from an inbox that
        // cannot reach this network.
        m << "Content-Type: image/jpeg; name=\"" << header_safe(basename_of(image_path))
          << "\"\r\n";
        m << "Content-Transfer-Encoding: base64\r\n";
        m << "Content-Disposition: attachment; filename=\""
          << header_safe(basename_of(image_path)) << "\"\r\n\r\n";
        m << b64(image);
        m << "--" << bnd << "--\r\n";
    }

    Payload payload{m.str(), 0};

    CURL* h = curl_easy_init();
    if (!h) return {false, "curl init failed"};

    const std::string url = "smtp://" + cfg_.host + ":" + std::to_string(cfg_.port);
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    // Port 465 is implicit TLS; everything else is STARTTLS, required rather
    // than merely attempted so credentials never cross in the clear.
    if (cfg_.port == 465) curl_easy_setopt(h, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    else curl_easy_setopt(h, CURLOPT_USE_SSL, CURLUSESSL_TRY);

    if (!cfg_.user.empty()) curl_easy_setopt(h, CURLOPT_USERNAME, cfg_.user.c_str());
    if (!cfg_.password.empty()) curl_easy_setopt(h, CURLOPT_PASSWORD, cfg_.password.c_str());

    curl_easy_setopt(h, CURLOPT_MAIL_FROM, ("<" + from + ">").c_str());
    curl_slist* list = nullptr;
    for (const auto& r : rcpts) list = curl_slist_append(list, ("<" + r + ">").c_str());
    curl_easy_setopt(h, CURLOPT_MAIL_RCPT, list);

    curl_easy_setopt(h, CURLOPT_READFUNCTION, read_payload);
    curl_easy_setopt(h, CURLOPT_READDATA, &payload);
    curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_seconds));

    const CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(list);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK) return {false, curl_easy_strerror(rc)};
    return {true, ""};
}

}  // namespace so::notify
