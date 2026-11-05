#pragma once

#include "modules/module.hpp"

#include <chrono>
#include <map>
#include <set>

namespace so {

struct UnauthorisedConfig {
    /// An unmatched face must persist this long before it is called
    /// unauthorised — one bad frame is not a stranger.
    std::chrono::milliseconds unknown_grace{6000};
    std::chrono::milliseconds cooldown{120000};
    /// Pre-filter ONLY: below this the face stage is skipped to save NPU work.
    /// It does not decide the alert; see the class comment.
    float min_person_px{220.0f};
    /// Restrict to zones of these types; empty means the whole frame.
    bool restricted_zones_only{false};
};

/**
 * Somebody in the building who is not an enrolled employee.
 *
 * GATED ON EVIDENCE, NOT ON A PROXY FOR IT
 * ----------------------------------------
 *   face found, matched the gallery   -> known employee, no alert
 *   face found, matched NOBODY        -> unauthorised, alert with a photo
 *   no face ever found on this track  -> unidentified, logged, NO alert
 *
 * Only the middle case raises an alarm, and it is the only one that can be
 * defended: we saw this person's face and it is not in the gallery. Alerting on
 * the third case would be claiming something about somebody the camera never
 * got a look at — and with every camera on a sub-stream that is the COMMON
 * case, so it would be a permanent stream of false alarms, which is how an
 * alert system teaches its operator to ignore it.
 *
 * A restricted area where ANY person is an intruder regardless of identity is
 * the perimeter module's job, and it needs no face at all.
 */
class UnauthorisedModule : public IModule {
public:
    explicit UnauthorisedModule(UnauthorisedConfig cfg = {});
    const char* id() const override { return "unauthorised"; }
    void on_frame(const FrameContext& ctx, EventSink& sink) override;

    /// Tracks seen but never resolvable — reported as `unidentified`, never as
    /// unauthorised. Exposed so the dashboard can show the distinction.
    std::size_t unidentified_count() const { return unidentified_.size(); }

private:
    struct Seen {
        std::chrono::steady_clock::time_point first_unmatched{};
        std::chrono::steady_clock::time_point last_seen{};
        bool alerted{false};
    };
    UnauthorisedConfig cfg_;
    std::map<int, Seen> unmatched_;
    std::set<int> unidentified_;
    std::map<int, std::chrono::steady_clock::time_point> cooldown_;
};

}  // namespace so
