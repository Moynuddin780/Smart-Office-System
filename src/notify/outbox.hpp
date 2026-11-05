#pragma once

#include "db/repos.hpp"
#include "notify/channels.hpp"
#include "notify/snapshot.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace so::notify {

/**
 * Reliable alert delivery.
 *
 * Nothing in the vision path sends anything. A module emits an event, the event
 * is written, and a row is queued here. This worker drains the queue.
 *
 * Two reasons, both of which have bitten real deployments:
 *
 *  - A slow mail server must not be able to stall a frame thread. An alert
 *    system that drops the NEXT hour of alerts because one SMTP handshake hung
 *    is worse than one that misses a single message.
 *  - A failed send must not lose the alert. It is a row with an attempt count,
 *    so it can be retried and, if it keeps failing, SEEN to be failing.
 */
/// Tuning for OutboxWorker. At namespace scope rather than nested, because a
/// nested type's default member initialisers cannot be used in a default
/// argument of its own enclosing class.
struct OutboxConfig {
    std::chrono::milliseconds poll{2000};
    int max_attempts{3};
    int batch{20};
};

class OutboxWorker {
public:
    OutboxWorker(db::Repos& repos, SnapshotStore& snapshots, OutboxConfig cfg = {});
    ~OutboxWorker();

    void start();
    void stop();

    /// Drain once, synchronously. Returns how many rows it attempted. Exposed so
    /// a test does not have to wait on the poll interval.
    int drain_once();

    std::int64_t sent() const { return sent_.load(); }
    std::int64_t failed() const { return failed_.load(); }

private:
    void loop();

    db::Repos& repos_;
    SnapshotStore& snapshots_;
    OutboxConfig cfg_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::int64_t> sent_{0}, failed_{0};
};

}  // namespace so::notify
