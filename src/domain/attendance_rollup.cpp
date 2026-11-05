#include "domain/attendance_rollup.hpp"

#include <algorithm>

namespace so {

const char* to_string(AttendanceStatus s) {
    switch (s) {
        case AttendanceStatus::Absent: return "absent";
        case AttendanceStatus::Present: return "present";
        case AttendanceStatus::Late: return "late";
        case AttendanceStatus::EarlyLeave: return "early_leave";
    }
    return "absent";
}

DayRollup rollup(std::vector<Punch> punches, const OfficeHours& hours,
                 std::int64_t as_of) {
    DayRollup r;

    std::stable_sort(punches.begin(), punches.end(),
                     [](const Punch& a, const Punch& b) { return a.seconds < b.seconds; });

    // Collapse runs of the same direction: keep the first `in` of a run and the
    // last `out` of a run. Two `in` events with no `out` between them describe
    // one arrival, and counting them as two would inflate the entry count and
    // corrupt the pairing below.
    std::vector<Punch> clean;
    for (const auto& p : punches) {
        if (!clean.empty() && clean.back().direction == p.direction) {
            if (p.direction == Direction::Out) clean.back() = p;   // keep the last out
            continue;                                              // keep the first in
        }
        clean.push_back(p);
    }

    // DROP GAPS TOO SHORT TO BE BREAKS.
    //
    // Done here, on the pairs, rather than by refusing to record the `out`:
    // the events stay as they were observed, and changing the threshold
    // re-reads every past day under the new rule instead of leaving history
    // computed under the old one.
    //
    // An `out` and the `in` that follows it within the threshold are both
    // removed, which rejoins the two stays either side into one — so the time
    // between them goes back to being time inside, and the day counts one
    // arrival rather than two.
    if (hours.min_break_seconds > 0) {
        std::vector<Punch> kept;
        for (std::size_t i = 0; i < clean.size(); ++i) {
            if (clean[i].direction == Direction::Out && i + 1 < clean.size() &&
                clean[i + 1].direction == Direction::In &&
                clean[i + 1].seconds - clean[i].seconds < hours.min_break_seconds) {
                ++i;              // skip this out AND the in that closes the gap
                continue;
            }
            kept.push_back(clean[i]);
        }
        clean.swap(kept);
    }

    // A leading `out` has no matching arrival — somebody was already inside when
    // the system started, or their entry was missed. It cannot contribute to
    // time inside, and it must not become `first_in`.
    while (!clean.empty() && clean.front().direction == Direction::Out)
        clean.erase(clean.begin());

    if (clean.empty()) {
        r.status = AttendanceStatus::Absent;
        return r;
    }

    r.has_activity = true;
    r.first_in = clean.front().seconds;

    for (std::size_t i = 0; i < clean.size(); ++i) {
        if (clean[i].direction != Direction::In) continue;
        ++r.entries;
        if (i + 1 < clean.size() && clean[i + 1].direction == Direction::Out) {
            r.inside_seconds += clean[i + 1].seconds - clean[i].seconds;
        }
    }

    // The last time they were recorded leaving, whether or not they came back.
    for (const auto& p : clean)
        if (p.direction == Direction::Out) r.last_out = p.seconds;

    if (clean.back().direction == Direction::Out) {
        r.counted_until = clean.back().seconds;
    } else {
        // Still "inside" with no departure recorded. The day is closed at the
        // office end OR at the moment the report is being run, whichever comes
        // first — a day in progress must not be credited with hours that have
        // not happened, and an evening on camera must not be credited past
        // closing. Never earlier than their arrival, so the time cannot go
        // negative for somebody who has only just walked in.
        const std::int64_t cap = std::min<std::int64_t>(hours.end.seconds, as_of);
        r.counted_until = std::max<std::int64_t>(clean.back().seconds, cap);
        r.inside_seconds += r.counted_until - clean.back().seconds;
        r.assumed_out = true;
    }

    r.gross_seconds = std::max<std::int64_t>(0, r.counted_until - r.first_in);
    r.inside_seconds = std::min(r.inside_seconds, r.gross_seconds);
    r.break_seconds = r.gross_seconds - r.inside_seconds;

    const std::int64_t late_after =
        hours.start.seconds + static_cast<std::int64_t>(hours.grace_minutes) * 60;
    const std::int64_t early_before =
        hours.end.seconds - static_cast<std::int64_t>(hours.early_leave_grace_minutes) * 60;

    if (r.first_in > late_after) r.status = AttendanceStatus::Late;
    // Early leave is judged on a REAL departure. Judging it on the counting cap
    // marked everybody still at their desk as having left early, all morning.
    else if (!r.assumed_out && r.last_out > 0 && r.last_out < early_before)
        r.status = AttendanceStatus::EarlyLeave;
    else r.status = AttendanceStatus::Present;

    return r;
}

}  // namespace so
