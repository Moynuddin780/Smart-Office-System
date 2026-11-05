#pragma once

#include "modules/module.hpp"

#include <chrono>
#include <map>

namespace so {

struct PerimeterConfig {
    /// A person must be inside for this long before an alarm. Without it a
    /// tracker jitter of one pixel across the boundary raises an intrusion.
    std::chrono::milliseconds confirm{1500};
    std::chrono::milliseconds zone_cooldown{60000};
};

/**
 * Restricted-area intrusion.
 *
 * Membership is decided on the FOOT POINT, never on box overlap: an overlap
 * test counts a tall person walking past the zone as being inside it, because
 * their box covers ground they are not standing on.
 */
class PerimeterModule : public IModule {
public:
    explicit PerimeterModule(PerimeterConfig cfg = {});
    const char* id() const override { return "perimeter"; }
    void on_frame(const FrameContext& ctx, EventSink& sink) override;

private:
    struct Presence {
        std::chrono::steady_clock::time_point since{};
        bool alerted{false};
    };
    PerimeterConfig cfg_;
    std::map<std::pair<std::int64_t, int>, Presence> inside_;   ///< (zone, track)
    std::map<std::int64_t, std::chrono::steady_clock::time_point> zone_cooldown_;
};

}  // namespace so
