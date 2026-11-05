#pragma once

#include "modules/module.hpp"

#include <chrono>
#include <map>

namespace so {

struct DeskConfig {
    /// An absence shorter than this does not end a session. Somebody turning to
    /// speak to a colleague, or briefly hidden behind one, has not left their
    /// desk, and ending their session there would shred the day into fragments.
    std::chrono::milliseconds absence_grace{30000};
    /// How often occupancy is committed. The vision layer runs far faster than
    /// this; a per-second tick is what the reports are built from.
    std::chrono::milliseconds tick{1000};
    /// How much of a person must overlap the desk to count as occupying it.
    ///
    /// Not a half: a desk zone is drawn around the working area, and a person
    /// standing at it or leaning back is only partly inside while still plainly
    /// using it. A fifth is enough to mean "this seat is taken" and still
    /// excludes somebody walking past the edge of the shape.
    double min_overlap{0.20};
};

/**
 * Desk-wise on-duty monitoring.
 *
 * A polygon is bound to one employee and persists until an operator edits or
 * resets it. Occupancy is the FOOT POINT of any tracked person inside it — the
 * same rule the perimeter module uses, and for the same reason.
 *
 * WHO is in the zone is deliberately NOT what decides the counting: the
 * operator assigned this desk to this employee, and that assignment is the
 * product's contract. When a face IS recognised and it belongs to somebody
 * else, the tick records that separately so a report can flag it rather than
 * silently crediting the wrong person.
 */
class DeskDutyModule : public IModule {
public:
    explicit DeskDutyModule(DeskConfig cfg = {});
    const char* id() const override { return "desk"; }
    void on_frame(const FrameContext& ctx, EventSink& sink) override;

private:
    struct ZoneState {
        std::chrono::steady_clock::time_point last_tick{};
        std::chrono::steady_clock::time_point absent_since{};
        bool present{false};
    };
    DeskConfig cfg_;
    std::map<std::int64_t, ZoneState> zones_;
};

}  // namespace so
