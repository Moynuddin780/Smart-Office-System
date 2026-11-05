#include "notify/channels.hpp"

#include "core/log.hpp"

#include <curl/curl.h>

#include <cctype>
#include <sstream>

namespace so::notify {
namespace {

std::size_t discard(void*, std::size_t size, std::size_t n, void*) { return size * n; }

std::size_t collect(void* data, std::size_t size, std::size_t n, void* user) {
    auto* out = static_cast<std::string*>(user);
    out->append(static_cast<char*>(data), size * n);
    return size * n;
}

struct Curl {
    CURL* h{nullptr};
    Curl() : h(curl_easy_init()) {}
    ~Curl() { if (h) curl_easy_cleanup(h); }
};

}  // namespace

Telegram::Telegram(TelegramConfig cfg) : cfg_(std::move(cfg)) {}

SendResult Telegram::send_message(const std::string& text) const {
    if (!configured()) return {false, "telegram is not configured"};

    Curl c;
    if (!c.h) return {false, "curl init failed"};

    std::string response;
    const std::string url = cfg_.api_base + "/bot" + cfg_.bot_token + "/sendMessage";

    char* chat = curl_easy_escape(c.h, cfg_.chat_id.c_str(), 0);
    // Telegram's hard limit is 4096; staying under it means a long detail string
    // truncates rather than losing the whole alert.
    char* msg = curl_easy_escape(c.h, text.substr(0, 4000).c_str(), 0);
    const std::string body = std::string("chat_id=") + (chat ? chat : "") +
                             "&disable_web_page_preview=true&text=" + (msg ? msg : "");
    if (chat) curl_free(chat);
    if (msg) curl_free(msg);

    curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.h, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c.h, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_seconds));
    curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &response);

    const CURLcode rc = curl_easy_perform(c.h);
    if (rc != CURLE_OK) return {false, curl_easy_strerror(rc)};

    long status = 0;
    curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300)
        return {false, "HTTP " + std::to_string(status) + ": " + response.substr(0, 200)};
    return {true, ""};
}

SendResult Telegram::send_photo(const std::string& image_path,
                                const std::string& caption) const {
    if (!configured()) return {false, "telegram is not configured"};
    if (image_path.empty()) return send_message(caption);

    Curl c;
    if (!c.h) return {false, "curl init failed"};

    std::string response;
    const std::string url = cfg_.api_base + "/bot" + cfg_.bot_token + "/sendPhoto";

    curl_mime* mime = curl_mime_init(c.h);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, cfg_.chat_id.c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "caption");
    // 1024 is Telegram's caption limit; going over rejects the whole message.
    curl_mime_data(part, caption.substr(0, 1000).c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "photo");
    curl_mime_filedata(part, image_path.c_str());

    curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.h, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c.h, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_seconds));
    curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &response);

    const CURLcode rc = curl_easy_perform(c.h);
    long status = 0;
    curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &status);
    curl_mime_free(mime);

    if (rc != CURLE_OK) return {false, curl_easy_strerror(rc)};
    if (status < 200 || status >= 300)
        return {false, "HTTP " + std::to_string(status) + ": " + response.substr(0, 200)};
    return {true, ""};
}

AlertText format_alert(const std::string& module, const std::string& severity,
                       const std::string& title, const std::string& detail,
                       const std::string& camera, const std::string& when) {
    const char* icon = severity == "critical" ? "\xF0\x9F\x94\xB4"       // red circle
                     : severity == "warning"  ? "\xF0\x9F\x9F\xA0"       // orange circle
                                              : "\xF0\x9F\x94\xB5";      // blue circle

    std::string up = severity;
    for (auto& ch : up) ch = static_cast<char>(std::toupper(ch));

    AlertText a;
    // A phone notification is read at a glance, so the module and camera lead.
    a.telegram = std::string(icon) + " " + title + " \xE2\x80\x94 " + up + "\n" +
                 "Camera: " + camera + "\n" + detail + "\n" + when;
    // An inbox entry is read later, so the subject has to survive being skimmed.
    a.subject = "[Smart Office] " + up + " \xE2\x80\x94 " + title + " on " + camera;
    a.email_body = title + "\n" + std::string(title.size(), '=') + "\n\n" +
                   "Severity : " + up + "\n" +
                   "Module   : " + module + "\n" +
                   "Camera   : " + camera + "\n" +
                   "Time     : " + when + "\n" +
                   "Detail   : " + detail + "\n\n" +
                   "-- \nSent by Smart Office. To stop these, turn the channel off "
                   "for this module on the Alerts page.\n";
    return a;
}

}  // namespace so::notify
