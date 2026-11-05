#pragma once

#include "modules/module.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace so {

/// Seconds since local midnight — how office hours are configured.
struct TimeOfDay {
    int seconds{0};
    static TimeOfDay from_hm(int h, int m) { return TimeOfDay{h * 3600 + m * 60}; }
};

struct OfficeHours {
    TimeOfDay start{TimeOfDay::from_hm(9, 0)};
    TimeOfDay end{TimeOfDay::from_hm(18, 0)};
    int grace_minutes{15};              ///< arriving within this is not late
    int early_leave_grace_minutes{15};
    /// The shortest gap that counts as a break, in seconds. 0 = count every gap.
    ///
    /// ZERO, by the operator's rule: however long somebody is out and however
    /// many times they go, it all accumulates.
    ///
    /// It was 120 for a day, after one measurement — an exit at 17:43:05 and an
    /// arrival twenty-two seconds later, which is a doorway rather than a
    /// break. That reasoning still holds for twenty-two seconds, but a number
    /// chosen from a single sample and applied without asking also discarded a
    /// genuine one-minute absence. The knob stays; the policy is the
    /// operator's.
    int min_break_seconds{0};
};

enum class AttendanceStatus { Absent, Present, Late, EarlyLeave };

const char* to_string(AttendanceStatus s);

struct Punch {
    Direction direction{Direction::In};
    std::int64_t seconds{0};            ///< seconds since local midnight
};

struct DayRollup {
    bool has_activity{false};
    std::int64_t first_in{0};
    /// When they were last recorded LEAVING. 0 when they never were.
    ///
    /// A fact, not a projection. It used to double as the end of the counted
    /// span, so somebody still in the building had their "last out" reported as
    /// the present moment — measured here as a departure timed to the minute
    /// the person walked back IN. The span is `counted_until`.
    std::int64_t last_out{0};
    /// How far time-inside was counted: the final departure on a closed day, or
    /// the earlier of the office end and now on a day still running.
    std::int64_t counted_until{0};
    std::int64_t gross_seconds{0};      ///< last_out - first_in
    std::int64_t inside_seconds{0};     ///< sum of paired in -> out intervals
    std::int64_t break_seconds{0};      ///< gross - inside
    int entries{0};                     ///< how many times they came in
    AttendanceStatus status{AttendanceStatus::Absent};
    /// The day ended on an unmatched `in`, so `last_out` was clamped to the end
    /// of the office day. The flag exists so a report can say so rather than
    /// presenting an invented departure time as measured fact.
    bool assumed_out{false};
};

/**
 * Turn a day's punches into the numbers a timesheet needs.
 *
 * Pure: no clock, no database, no NPU. Everything about how a working day is
 * counted lives here and is covered by unit tests, because this is the part of
 * the product a customer will argue with.
 *
 * Punches may arrive in any order and may repeat: face recognition debounces
 * per person, but a person lingering in a doorway can still produce two `in`
 * events before any `out`. Runs of the same direction collapse — the FIRST of
 * an `in` run and the LAST of an `out` run — which is what makes
 * `inside_seconds` the time they were actually in the building.
 */
/// `as_of` is how far the clock has got, in seconds since local midnight, and
/// it bounds the ONE number this function has to invent: the departure of
/// somebody who never registered one.
///
/// Without it the unmatched `in` was always closed at the office end, so a day
/// still in progress was credited with hours nobody had worked yet — at two in
/// the afternoon every present employee already showed a full day. Pass the
/// current time for today and anything past the office end (24h) for a day that
/// is over; the office end still caps both, so hours after closing are never
/// credited either.
DayRollup rollup(std::vector<Punch> punches, const OfficeHours& hours,
                 std::int64_t as_of = 24 * 3600);

}  // namespace so
