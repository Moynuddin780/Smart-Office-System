#pragma once

#include "modules/module.hpp"

#include <chrono>
#include <map>

namespace so {

struct AttendanceModuleConfig {
    /// One event per employee per this window. A person standing in a doorway
    /// otherwise produces one event per frame.
    std::chrono::milliseconds debounce{8000};
    /// The same, for cameras with no gate line, where the event is a sighting
    /// rather than a crossing.
    ///
    /// Much longer, because the two mean different things. A crossing is an act
    /// and is over in a second, so 8 s only suppresses duplicates of one event.
    /// A sighting lasts as long as the person stays in view — somebody at their
    /// desk is recognised continuously, and at 8 s that is 450 rows an hour per
    /// person saying nothing new. A minute keeps "still here" current at a cost
    /// the database does not notice.
    std::chrono::milliseconds sighting_debounce{60000};
};

/**
 * Face-recognition attendance.
 *
 * BEING RECOGNISED IS ENOUGH. One camera watches the front door; anybody it
 * names has arrived, and that is what a timesheet needs.
 *
 * There is no line to cross. Direction used to come from one drawn across the
 * doorway — A->B an entry, B->A an exit — and it cost more than it gave: until
 * somebody drew it the camera recognised people all day and recorded nothing,
 * which is the worst failure a feature can have, because it looks configured.
 * A day with no exit punch is closed by the roll-up's assumed_out, which
 * already existed for the person who leaves past a camera that never saw them.
 *
 * An identity is still required: an anonymous person is traffic, not
 * attendance, since there is nobody to put on the timesheet.
 */
class AttendanceModule : public IModule {
public:
    explicit AttendanceModule(AttendanceModuleConfig cfg = {});
    const char* id() const override { return "attendance"; }
    void on_frame(const FrameContext& ctx, EventSink& sink) override;

private:
    struct TrackPath {
        std::chrono::steady_clock::time_point last_seen{};
        std::int64_t employee_id{0};
        std::string employee_name;
        float match_score{0};
    };
    AttendanceModuleConfig cfg_;
    std::map<int, TrackPath> paths_;
    std::map<std::int64_t, std::chrono::steady_clock::time_point> last_event_;
};

}  // namespace so
