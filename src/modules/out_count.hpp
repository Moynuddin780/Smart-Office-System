#pragma once

#include "modules/module.hpp"

#include <chrono>
#include <map>

namespace so {

struct OutCountConfig {
    /// How long a person must be inside the exit zone before leaving counts.
    ///
    /// Without it, a tracker that jitters one pixel across the boundary for a
    /// single frame and then loses the track books somebody out of the
    /// building. A person actually walking out is in the doorway for about a
    /// second.
    std::chrono::milliseconds confirm{900};

    /// How long the track must be GONE before the exit is believed.
    ///
    /// This is the whole mechanism: somebody who walks into the exit zone and
    /// out of frame has left; somebody who walks into it and back into the room
    /// has not. The only thing that separates those two is whether the track
    /// comes back, so the decision has to wait. Too short and a person briefly
    /// hidden behind the door frame is booked out; too long and the exit is
    /// recorded minutes after it happened.
    /// 2500 ms was too short. A person standing at the reception desk has
    /// their track dropped and re-opened within a second or two, and at 2.5 s
    /// the old candidate had already timed out and booked them out of the
    /// building — confirmed on 27 October.
    ///
    /// Eight seconds costs NOTHING in accuracy: the exit is timestamped at the
    /// moment they were last seen, not when this timer expires, so a real
    /// departure is recorded with the same time either way. It costs only how
    /// soon the row appears, and nobody is watching for it.
    std::chrono::milliseconds gone{8000};

    /// How long a person may stay in view AFTER leaving the zone before it is
    /// read as a change of mind rather than a departure.
    ///
    /// The zone is drawn on the floor in FRONT of the doors, not across them,
    /// so somebody leaving steps out of it on the door side and is visible
    /// there for a moment before going through. Dropping them the instant they
    /// left the zone lost exactly those people. Somebody who turned back, by
    /// contrast, goes on being seen in the room for far longer than it takes to
    /// walk through a door.
    std::chrono::milliseconds linger{3000};

    /// How close a surviving track has to be to the vanished one before it is
    /// taken for the same person, as a fraction of that person's own width.
    ///
    /// Track churn is the main source of false exits here: somebody standing
    /// still has their track dropped and immediately re-opened under a new id,
    /// and the old candidate then times out with nothing to contradict it.
    /// Confirmed on 27 October — an employee booked out twelve seconds after
    /// arriving, while standing at the reception desk.
    ///
    /// Someone who really walked out leaves empty floor behind them, so the
    /// test is simply whether anybody is still standing there.
    float same_spot = 1.0f;

    /// One exit per employee per this window, so a group leaving together
    /// through a busy doorway cannot book one person out twice.
    std::chrono::milliseconds debounce{60000};

    /// The shortest visit that can end in a believable departure.
    ///
    /// SEEN ON THIS SITE: an employee walked IN through the main entry, was
    /// recorded arriving at 08:40:17, and two seconds later the same doorway
    /// recorded them leaving. A second case was twelve seconds. Nobody enters a
    /// building and leaves it again in two seconds, so whatever the zone and
    /// the tracker believed, those were not departures — an arrival whose track
    /// was lost a moment later looks exactly like one.
    ///
    /// This is a floor under the whole decision, independent of the geometry:
    /// the zone can be drawn in the wrong place, the tracker can drop an id,
    /// and this still refuses to turn an arrival into a departure. It costs a
    /// genuine in-and-straight-back-out within the window, which is rare and
    /// far cheaper than a false exit that silently ends somebody's working day.
    std::chrono::seconds min_stay{30};
};

/**
 * Out-count: records an employee leaving, so a day has an `out` to pair with.
 *
 * WHY THIS EXISTS. Attendance records somebody the moment their face is
 * recognised, and every one of those is an `in`. The day roll-up already knows
 * how to turn alternating in/out punches into time inside and time on break —
 * it has always known — but with nothing in the system ever emitting an `out`,
 * every break came out as zero and every day was closed by `assumed_out`.
 * This is the missing half, not a new calculation.
 *
 * HOW LEAVING IS DECIDED. A drawn exit zone, then absence:
 *
 *   1. the person is inside the zone for `confirm`
 *   2. their track then disappears for `gone`
 *   3. an `out` is recorded, timed to when they were last seen
 *
 * NOT a line crossed in a direction. A line needs the operator to know which
 * side is outdoors, it reverses meaning silently when a camera is remounted,
 * and it was removed from attendance for exactly those reasons. Absence after
 * presence needs no such knowledge: whichever way the door faces, somebody who
 * stood in the doorway and then stopped existing has gone through it.
 *
 * ONLY FOR SOMEBODY ALREADY IN. An `out` for a person with no `in` today is not
 * a half-day, it is a corrupt record: the roll-up would pair it with nothing
 * and the timesheet would show a departure from a building they never entered.
 * The presence roster is what makes the first `in` of the day the start of
 * everything, which is the rule the feature was asked for.
 */
class OutCountModule : public IModule {
public:
    explicit OutCountModule(OutCountConfig cfg = {});
    const char* id() const override { return "out_count"; }
    void on_frame(const FrameContext& ctx, EventSink& sink) override;

private:
    struct Candidate {
        std::chrono::steady_clock::time_point inside_since{};
        std::chrono::steady_clock::time_point last_seen{};
        TimePoint last_seen_wall{};
        bool confirmed{false};        ///< stayed in the zone long enough
        /// Seen in the room BEFORE reaching the doorway.
        ///
        /// One door serves both directions, so this is what tells an arrival
        /// from a departure. Somebody leaving crossed the room first, so their
        /// track exists outside the zone before it enters it; somebody arriving
        /// appears already in the doorway, and their track is born inside it.
        /// Without this, an arrival whose track is lost at the threshold — a
        /// second person stepping in front of them is enough — is recorded as
        /// having left the instant they came in.
        bool came_from_room{false};
        /// When they stepped out of the zone, or zero while still inside. The
        /// gap between the far edge of the zone and the door lives here.
        std::chrono::steady_clock::time_point left_zone{};
        /// Who the evidence most resembled, even when it named nobody. This is
        /// what turns an unnamed exit from "somebody left" into a question a
        /// person can answer.
        std::int64_t guess_employee_id{0};
        float guess_score{0}, guess_margin{0}, guess_weight{0};
        cv::Mat crop;             ///< the best face seen of this track
        float crop_px{0};         ///< how big it was, so the best one is kept

        std::int64_t employee_id{0};
        std::string employee_name;
        float match_score{0};
        std::int64_t zone_id{0};
        cv::Mat last_frame;           ///< for the snapshot, since the person is gone by then
        Box last_box{};
    };

    /// A name this camera read, and the track that was wearing it.
    ///
    /// Separate from Candidate because it is learned somewhere else entirely —
    /// out in the room where the face is big enough — and has to survive until
    /// that person reaches the door.
    struct Named {
        std::int64_t employee_id{0};
        std::string employee_name;
        float match_score{0};
        std::chrono::steady_clock::time_point last_seen{};
    };

    OutCountConfig cfg_;
    std::map<int, Candidate> seen_;   ///< by track id
    /// Track ids that have been observed OUTSIDE every exit zone. Kept for all
    /// tracks, not just candidates, because the fact is established before the
    /// person ever reaches the door.
    std::map<int, std::chrono::steady_clock::time_point> in_room_;
    std::map<int, Named> named_;      ///< by track id
    std::map<std::int64_t, std::chrono::steady_clock::time_point> last_out_;
};

}  // namespace so
