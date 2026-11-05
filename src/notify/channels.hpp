#pragma once

#include <string>

namespace so::notify {

struct SendResult {
    bool ok{false};
    std::string error;
};

struct TelegramConfig {
    std::string bot_token;
    std::string chat_id;
    /// Overridable so a test can point at a local mock instead of Telegram.
    std::string api_base{"https://api.telegram.org"};
    int timeout_seconds{15};
};

/**
 * Telegram.
 *
 * `send_photo` is the one that matters: the requirement is the person's
 * picture, so a text-only alert does not satisfy it. `send_message` exists for
 * the case where an event genuinely has no image.
 *
 * Captions are PLAIN TEXT, never Markdown. Alert text carries camera and zone
 * names an operator typed, and a single stray underscore or bracket in one of
 * those makes Telegram reject the entire message with a 400. A missed alert is
 * not worth bold text.
 */
class Telegram {
public:
    explicit Telegram(TelegramConfig cfg);

    SendResult send_message(const std::string& text) const;
    SendResult send_photo(const std::string& image_path, const std::string& caption) const;

    bool configured() const { return !cfg_.bot_token.empty() && !cfg_.chat_id.empty(); }

private:
    TelegramConfig cfg_;
};

struct EmailConfig {
    std::string host;
    int port{587};
    std::string user, password, from, from_name, to;
    int timeout_seconds{25};
};

/**
 * SMTP with an attached image.
 *
 * The MIME body is built here rather than handed to a library so the image is
 * genuinely attached rather than linked: an alert has to be useful in an inbox
 * that cannot reach this network.
 */
class Email {
public:
    explicit Email(EmailConfig cfg);

    SendResult send(const std::string& subject, const std::string& body,
                    const std::string& image_path = "") const;

    bool configured() const { return !cfg_.host.empty() && !cfg_.to.empty(); }

private:
    EmailConfig cfg_;
};

/// Shared formatting so an alert reads the same on both channels.
struct AlertText {
    std::string telegram;
    std::string subject;
    std::string email_body;
};
AlertText format_alert(const std::string& module, const std::string& severity,
                       const std::string& title, const std::string& detail,
                       const std::string& camera, const std::string& when);

}  // namespace so::notify
