#include "notify/outbox.hpp"

#include "core/log.hpp"

#include <algorithm>

namespace so::notify {

OutboxWorker::OutboxWorker(db::Repos& repos, SnapshotStore& snapshots, OutboxConfig cfg)
    : repos_(repos), snapshots_(snapshots), cfg_(cfg) {}

OutboxWorker::~OutboxWorker() { stop(); }

void OutboxWorker::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread(&OutboxWorker::loop, this);
}

void OutboxWorker::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

void OutboxWorker::loop() {
    while (!stop_.load()) {
        try {
            drain_once();
        } catch (const std::exception& e) {
            // The worker must survive anything the database or a relay does.
            SO_LOG_ERROR("[outbox] drain failed: %s", e.what());
        }
        for (int slept = 0; slept < cfg_.poll.count() && !stop_.load(); slept += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int OutboxWorker::drain_once() {
    auto& db = repos_.raw();

    const auto due = db.query(
        "SELECT o.id,o.tenant_id,o.channel,o.attempts,ev.module,ev.severity,ev.title,"
        "ev.detail,COALESCE(c.name,'-'),to_char(ev.ts,'YYYY-MM-DD HH24:MI:SS'),"
        "COALESCE(NULLIF(ev.crop_path,''),ev.snapshot_path),t.slug "
        "FROM notification_outbox o "
        "JOIN events ev ON ev.id=o.event_id "
        "JOIN tenants t ON t.id=o.tenant_id "
        "LEFT JOIN cameras c ON c.id=ev.camera_id "
        "WHERE o.status='pending' AND o.next_try_at <= now() "
        "ORDER BY o.id LIMIT $1",
        {std::to_string(cfg_.batch)});

    int attempted = 0;
    for (int i = 0; i < due.rows(); ++i) {
        const std::int64_t row_id = due.integer(i, 0);
        const std::int64_t tenant_id = due.integer(i, 1);
        const std::string channel = due.text(i, 2);
        const int attempts = static_cast<int>(due.integer(i, 3));
        const std::string image_rel = due.text(i, 10);

        const db::Settings s = repos_.settings(db::TenantId{tenant_id});
        const AlertText text = format_alert(due.text(i, 4), due.text(i, 5), due.text(i, 6),
                                            due.text(i, 7), due.text(i, 8), due.text(i, 9));
        const std::string image = snapshots_.resolve(image_rel);

        SendResult r;
        if (channel == "telegram") {
            TelegramConfig tc;
            tc.bot_token = s.telegram_bot_token;
            tc.chat_id = s.telegram_chat_id;
            // send_photo, not send_message: the requirement is the person's
            // picture. It falls back to text only when there is no image at all.
            r = Telegram(tc).send_photo(image, text.telegram);
        } else {
            EmailConfig ec;
            ec.host = s.smtp_host;
            ec.port = s.smtp_port;
            ec.user = s.smtp_user;
            ec.password = s.smtp_password;
            ec.from = s.smtp_from;
            ec.from_name = s.smtp_from_name;
            ec.to = s.email_to;
            r = Email(ec).send(text.subject, text.email_body, image);
        }
        ++attempted;

        if (r.ok) {
            db.query(
                "UPDATE notification_outbox SET status='sent',attempts=attempts+1,"
                "sent_at=now(),last_error='' WHERE id=$1",
                {std::to_string(row_id)});
            sent_.fetch_add(1);
            continue;
        }

        const int next = attempts + 1;
        if (next >= cfg_.max_attempts) {
            // Give up, but LOUDLY and permanently visible: a silently dropped
            // alert is indistinguishable from nothing having happened.
            db.query(
                "UPDATE notification_outbox SET status='failed',attempts=$2,last_error=$3 "
                "WHERE id=$1",
                {std::to_string(row_id), std::to_string(next), r.error.substr(0, 500)});
            failed_.fetch_add(1);
            SO_LOG_ERROR("[outbox] %s delivery gave up after %d attempts: %s",
                      channel.c_str(), next, r.error.c_str());
        } else {
            // Exponential backoff, so a relay that is down for a minute does not
            // get hammered and a transient failure still clears itself.
            const int delay = 15 * (1 << (next - 1));
            db.query(
                "UPDATE notification_outbox SET attempts=$2,last_error=$3,"
                "next_try_at=now() + ($4||' seconds')::interval WHERE id=$1",
                {std::to_string(row_id), std::to_string(next), r.error.substr(0, 500),
                 std::to_string(delay)});
            SO_LOG_WARN("[outbox] %s attempt %d/%d failed, retrying in %ds: %s",
                     channel.c_str(), next, cfg_.max_attempts, delay, r.error.c_str());
        }
    }
    return attempted;
}

}  // namespace so::notify
