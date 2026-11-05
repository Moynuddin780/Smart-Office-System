#include "doctest/doctest.h"

#include "domain/attendance_rollup.hpp"

using so::AttendanceStatus;
using so::Direction;
using so::OfficeHours;
using so::Punch;

namespace {

std::int64_t at(int h, int m) { return h * 3600 + m * 60; }

Punch in_(int h, int m) { return Punch{Direction::In, at(h, m)}; }
Punch out_(int h, int m) { return Punch{Direction::Out, at(h, m)}; }

OfficeHours nine_to_six() {
    OfficeHours o;
    o.start = so::TimeOfDay::from_hm(9, 0);
    o.end = so::TimeOfDay::from_hm(18, 0);
    o.grace_minutes = 15;
    o.early_leave_grace_minutes = 15;
    return o;
}

}  // namespace

TEST_CASE("a normal day with a lunch break") {
    const auto r = so::rollup({in_(9, 2), out_(12, 30), in_(13, 15), out_(18, 5)},
                              nine_to_six());
    CHECK(r.has_activity);
    CHECK(r.first_in == at(9, 2));
    CHECK(r.last_out == at(18, 5));
    CHECK(r.gross_seconds == at(9, 3));
    CHECK(r.inside_seconds == at(3, 28) + at(4, 50));
    CHECK(r.break_seconds == at(0, 45));
    CHECK(r.entries == 2);
    CHECK(r.status == AttendanceStatus::Present);
    CHECK_FALSE(r.assumed_out);
}

TEST_CASE("arriving inside the grace window is not late") {
    CHECK(so::rollup({in_(9, 14), out_(18, 0)}, nine_to_six()).status ==
          AttendanceStatus::Present);
}

TEST_CASE("arriving past the grace window is late") {
    const auto r = so::rollup({in_(9, 16), out_(18, 0)}, nine_to_six());
    CHECK(r.status == AttendanceStatus::Late);
}

TEST_CASE("leaving early is flagged, and lateness takes precedence over it") {
    CHECK(so::rollup({in_(9, 0), out_(16, 0)}, nine_to_six()).status ==
          AttendanceStatus::EarlyLeave);
    // Late AND early is reported as late: the arrival is the fact a supervisor
    // acts on, and reporting one status means picking the more significant.
    CHECK(so::rollup({in_(11, 0), out_(16, 0)}, nine_to_six()).status ==
          AttendanceStatus::Late);
}

TEST_CASE("no punches at all is absent, not a zero-length day") {
    const auto r = so::rollup({}, nine_to_six());
    CHECK(r.status == AttendanceStatus::Absent);
    CHECK_FALSE(r.has_activity);
    CHECK(r.gross_seconds == 0);
    CHECK(r.entries == 0);
}

TEST_CASE("an unmatched trailing entry is clamped to the office close and FLAGGED") {
    const auto r = so::rollup({in_(9, 0)}, nine_to_six());
    CHECK(r.counted_until == at(18, 0));   // how far time inside was counted
    CHECK(r.last_out == 0);                // they were never recorded leaving
    CHECK(r.assumed_out);
    CHECK(r.inside_seconds == at(9, 0));
    // The flag is the point: the close is inferred, and a timesheet must be
    // able to say so rather than presenting it as an observed departure. The
    // departure itself is simply absent — reporting the inferred close AS a
    // departure is what put an arrival time in the "last out" column.
}

TEST_CASE("someone still inside past the office close keeps their real last punch") {
    const auto r = so::rollup({in_(9, 0), out_(13, 0), in_(14, 0)}, nine_to_six());
    CHECK(r.assumed_out);
    CHECK(r.counted_until == at(18, 0));   // counted to the office close
    CHECK(r.last_out == at(13, 0));        // but they last LEFT at one o'clock
    CHECK(r.inside_seconds == at(4, 0) + at(4, 0));
}

TEST_CASE("duplicate consecutive punches collapse instead of inflating the count") {
    const auto r = so::rollup({in_(9, 0), in_(9, 1), out_(18, 0), out_(18, 2)},
                              nine_to_six());
    CHECK(r.first_in == at(9, 0));      // the FIRST of an in run
    CHECK(r.last_out == at(18, 2));     // the LAST of an out run
    CHECK(r.inside_seconds == at(9, 2));
    CHECK(r.entries == 1);
    CHECK_FALSE(r.assumed_out);
}

TEST_CASE("a leading exit with no entry is ignored rather than becoming first_in") {
    const auto r = so::rollup({out_(9, 5), in_(9, 10), out_(18, 0)}, nine_to_six());
    CHECK(r.first_in == at(9, 10));
    CHECK(r.inside_seconds == at(8, 50));
    CHECK(r.entries == 1);
}

TEST_CASE("punches out of order are sorted, not trusted") {
    const auto r = so::rollup({out_(18, 0), in_(9, 0), in_(13, 0), out_(12, 0)},
                              nine_to_six());
    CHECK(r.first_in == at(9, 0));
    CHECK(r.last_out == at(18, 0));
    CHECK(r.inside_seconds == at(3, 0) + at(5, 0));
    CHECK(r.entries == 2);
}

TEST_CASE("break time is whatever the day was not spent inside") {
    const auto r = so::rollup({in_(9, 0), out_(10, 0), in_(11, 0), out_(12, 0),
                               in_(14, 0), out_(18, 0)},
                              nine_to_six());
    CHECK(r.gross_seconds == at(9, 0));
    CHECK(r.inside_seconds == at(1, 0) + at(1, 0) + at(4, 0));
    CHECK(r.break_seconds == at(3, 0));
    CHECK(r.entries == 3);
}

TEST_CASE("inside time can never exceed the span it sits in") {
    // A guard against any future pairing bug producing a nonsense timesheet.
    const auto r = so::rollup({in_(9, 0), out_(9, 30), in_(9, 20), out_(10, 0)},
                              nine_to_six());
    CHECK(r.inside_seconds <= r.gross_seconds);
    CHECK(r.break_seconds >= 0);
}

TEST_CASE("a single-punch day where the entry is after office close") {
    const auto r = so::rollup({in_(19, 0)}, nine_to_six());
    CHECK(r.assumed_out);
    CHECK(r.counted_until == at(19, 0));   // never earlier than the punch itself
    CHECK(r.last_out == 0);                // and no departure to report
    CHECK(r.gross_seconds == 0);
    CHECK(r.status == AttendanceStatus::Late);
}

TEST_CASE("a day with a break: in, out through the exit zone, in again") {
    // The whole feature, end to end in arithmetic. The punches are what the
    // two modules produce on a normal day:
    //   09:00  attendance   recognised at the front door        -> in
    //   09:01  attendance   still in view, debounced sighting   -> in
    //   13:00  out_count    walked out through the exit zone    -> out
    //   14:00  attendance   recognised at the front door again  -> in
    //   18:00  out_count    left for the day                    -> out
    //
    // Runs of the same direction collapse, so the repeated sighting at 09:01
    // does not become a second entry.
    const auto at = [](int h, int m, so::Direction d) {
        return so::Punch{d, h * 3600 + m * 60};
    };
    so::OfficeHours hours;
    const auto day = so::rollup({at(9, 0, so::Direction::In),
                                 at(9, 1, so::Direction::In),
                                 at(13, 0, so::Direction::Out),
                                 at(14, 0, so::Direction::In),
                                 at(18, 0, so::Direction::Out)},
                                hours);

    REQUIRE(day.has_activity);
    CHECK(day.first_in == 9 * 3600);        // their attendance for the day
    CHECK(day.last_out == 18 * 3600);
    CHECK(day.gross_seconds == 9 * 3600);   // 09:00 -> 18:00
    CHECK(day.inside_seconds == 8 * 3600);  // 4 h + 4 h actually in the building
    CHECK(day.break_seconds == 3600);       // the hour they were out
    CHECK(day.entries == 2);                // arrived, left, came back
    CHECK_FALSE(day.assumed_out);           // a real departure, not an inferred one
}

TEST_CASE("without any out, the break is zero and the departure is inferred") {
    // What every day looked like before out_count existed: attendance emits
    // nothing but `in`, so there was never a second punch to subtract from.
    // This is the behaviour the feature replaces, kept as a test so the
    // difference is visible rather than remembered.
    const auto at = [](int h, so::Direction d) { return so::Punch{d, h * 3600}; };
    so::OfficeHours hours;
    const auto day = so::rollup({at(9, so::Direction::In), at(13, so::Direction::In),
                                 at(16, so::Direction::In)},
                                hours);

    CHECK(day.break_seconds == 0);
    CHECK(day.entries == 1);
    CHECK(day.assumed_out);
}

TEST_CASE("attendance on the exit camera does not corrupt the break") {
    // THE RULE THIS PROTECTS: attendance counts from EVERY camera, indoor or
    // out. An employee the door camera misses must still be recorded by
    // whichever camera does see them, or a missed glance becomes a missed day.
    //
    // That rule and out-count meet on one camera, and the worry was that they
    // would fight: the exit camera recognises the person walking towards the
    // door and records an `in`, moments before out-count records the `out`.
    //
    // They do not fight, and the reason is ordering. Punches are read back
    // ORDER BY ts; out-count timestamps its `out` at the moment the person was
    // LAST SEEN, and attendance can only fire while they are still visible. So
    // the `in` always lands at or before the `out`, runs of one direction
    // collapse, and the pair survives intact.
    const auto at = [](int h, int m, so::Direction d) {
        return so::Punch{d, h * 3600 + m * 60};
    };
    so::OfficeHours hours;
    const auto day = so::rollup({at(9, 0, so::Direction::In),    // VARITO, arrives
                                 at(12, 59, so::Direction::In),  // EXIT camera sees them
                                 at(13, 0, so::Direction::Out),  // out-count, as they go
                                 at(14, 0, so::Direction::In),   // VARITO, comes back
                                 at(18, 0, so::Direction::Out)}, // leaves for the day
                                hours);

    CHECK(day.first_in == 9 * 3600);          // still their arrival, not 12:59
    CHECK(day.break_seconds == 3600);         // the hour out, unchanged
    CHECK(day.inside_seconds == 8 * 3600);
    CHECK(day.entries == 2);

    // And the case the rule exists for: the door camera missed them entirely
    // and only the exit camera ever saw them. Without attendance on that
    // camera this day is Absent; with it, it is a real day.
    const auto rescued = so::rollup({at(9, 5, so::Direction::In),
                                     at(18, 0, so::Direction::Out)}, hours);
    CHECK(rescued.has_activity);
    CHECK(rescued.first_in == 9 * 3600 + 5 * 60);
}

TEST_CASE("a day still in progress is not credited with hours that have not happened") {
    // AT 14:00, SOMEBODY WHO ARRIVED AT 09:00 HAS WORKED FIVE HOURS, NOT NINE.
    //
    // The unmatched-`in` branch closed the day at the office end whatever the
    // time was, so at two in the afternoon the board showed a full day already
    // worked. Everyone present read as 8h+ hours before the afternoon was out,
    // which makes the whole column worthless: a number that reaches its maximum
    // by lunchtime cannot tell a long day from a short one.
    const auto at = [](int h, int m, so::Direction d) {
        return so::Punch{d, h * 3600 + m * 60};
    };
    so::OfficeHours hours;                       // 09:00 - 18:00
    const std::int64_t two_pm = 14 * 3600;

    const auto today = so::rollup({at(9, 0, so::Direction::In)}, hours, two_pm);
    CHECK(today.counted_until == two_pm);        // as far as the clock has got
    CHECK(today.last_out == 0);                  // no departure happened
    CHECK(today.inside_seconds == 5 * 3600);     // five hours, not nine
    CHECK(today.break_seconds == 0);
    CHECK(today.assumed_out);                    // still a guess, still labelled

    // A FINISHED DAY IS UNCHANGED. Yesterday's row must still close at the
    // office end, or every historical day would shrink to whatever time the
    // report happened to be run.
    const auto yesterday = so::rollup({at(9, 0, so::Direction::In)}, hours, 24 * 3600);
    CHECK(yesterday.counted_until == 18 * 3600);
    CHECK(yesterday.inside_seconds == 9 * 3600);

    // Past the office end on the same day, the end still wins: hours after
    // closing are not credited just because somebody was still on camera.
    const auto evening = so::rollup({at(9, 0, so::Direction::In)}, hours, 20 * 3600);
    CHECK(evening.counted_until == 18 * 3600);

    // A REAL departure is never overridden by the clock.
    const auto left = so::rollup({at(9, 0, so::Direction::In), at(13, 0, so::Direction::Out)},
                                 hours, two_pm);
    CHECK(left.last_out == 13 * 3600);
    CHECK(left.counted_until == 13 * 3600);
    CHECK(left.inside_seconds == 4 * 3600);
    CHECK_FALSE(left.assumed_out);

    // Somebody who arrived after the cut-off is not given negative time.
    const auto just_arrived = so::rollup({at(13, 55, so::Direction::In)}, hours, two_pm);
    CHECK(just_arrived.inside_seconds == 5 * 60);
}

TEST_CASE("a gap too short to be a break is not counted as one") {
    // MEASURED, NOT IMAGINED. On 26 October the first real exit this system
    // ever recorded ran: out at 17:43:05 on the door camera, and twenty-two
    // seconds later the lobby camera saw the same person and recorded an `in`.
    // He had not left for the day; he had stepped through a door and was picked
    // up again on the other side.
    //
    // Twenty-two seconds is not a break, and a timesheet that calls it one is
    // reporting camera coverage rather than behaviour. Below the threshold the
    // gap is handed back to time-inside, which is where the person actually
    // was.
    so::OfficeHours hours;
    hours.min_break_seconds = 120;              // two minutes

    const auto s = [](int h, int m, int sec, so::Direction d) {
        return so::Punch{d, h * 3600 + m * 60 + sec};
    };

    const auto blip = so::rollup({s(9, 0, 0, so::Direction::In),
                                  s(17, 43, 5, so::Direction::Out),
                                  s(17, 43, 27, so::Direction::In),
                                  s(18, 0, 0, so::Direction::Out)},
                                 hours, 24 * 3600);
    CHECK(blip.break_seconds == 0);             // the 22 s is not a break
    CHECK(blip.inside_seconds == 9 * 3600);     // he was here the whole time
    CHECK(blip.entries == 1);                   // and he arrived once, not twice

    // A REAL break is untouched.
    const auto lunch = so::rollup({s(9, 0, 0, so::Direction::In),
                                   s(13, 0, 0, so::Direction::Out),
                                   s(14, 0, 0, so::Direction::In),
                                   s(18, 0, 0, so::Direction::Out)},
                                  hours, 24 * 3600);
    CHECK(lunch.break_seconds == 3600);
    CHECK(lunch.entries == 2);

    // Several blips do not add up into a break either, and the last one does
    // not strand the day on an unmatched `out`.
    const auto many = so::rollup({s(9, 0, 0, so::Direction::In),
                                  s(11, 0, 0, so::Direction::Out),
                                  s(11, 0, 30, so::Direction::In),
                                  s(15, 0, 0, so::Direction::Out),
                                  s(15, 1, 0, so::Direction::In),
                                  s(18, 0, 0, so::Direction::Out)},
                                 hours, 24 * 3600);
    CHECK(many.break_seconds == 0);
    CHECK(many.inside_seconds == 9 * 3600);

    // Zero disables it, which is what every existing day was computed with.
    so::OfficeHours off;
    off.min_break_seconds = 0;
    const auto raw = so::rollup({s(9, 0, 0, so::Direction::In),
                                 s(17, 43, 5, so::Direction::Out),
                                 s(17, 43, 27, so::Direction::In),
                                 s(18, 0, 0, so::Direction::Out)},
                                off, 24 * 3600);
    CHECK(raw.break_seconds == 22);
}

TEST_CASE("last out is when they last left, never when the report was run") {
    // MEASURED ON 27 OCTOBER. Iftekhar left at 10:00 and came back at 10:15.
    // His break read 15 minutes, which was right — and his LAST OUT read 10:15,
    // which is the moment he came back IN.
    //
    // The cause is one field doing two jobs. `last_out` was both "when they
    // last left" and "how far to count time inside", and for somebody currently
    // in the building the second is the present moment. So the column confidently
    // reported a departure that had not happened, timed to an arrival.
    //
    // They are separate questions and now have separate answers: the last `out`
    // punch is a fact and stays empty when there is none, while the counting
    // span is a decision about an unfinished day.
    const auto at = [](int h, int m, so::Direction d) {
        return so::Punch{d, h * 3600 + m * 60};
    };
    so::OfficeHours hours;
    hours.min_break_seconds = 0;
    const std::int64_t half_ten = 10 * 3600 + 30 * 60;      // the clock, 10:30

    const auto d = so::rollup({at(9, 59, so::Direction::In),
                               at(10, 0, so::Direction::Out),
                               at(10, 15, so::Direction::In)},
                              hours, half_ten);

    CHECK(d.last_out == 10 * 3600);              // 10:00 — when he actually left
    CHECK(d.break_seconds == 15 * 60);           // unchanged, still right
    CHECK(d.inside_seconds == 16 * 60);          // 1 min before, 15 min since
    CHECK(d.gross_seconds == 31 * 60);           // 09:59 -> 10:30
    CHECK(d.assumed_out);                        // still inside; no final exit yet

    // NEVER LEFT AT ALL: there is no last out to report, and inventing one is
    // what the old code did.
    const auto never = so::rollup({at(9, 0, so::Direction::In)}, hours, half_ten);
    CHECK(never.last_out == 0);
    CHECK(never.assumed_out);
    CHECK(never.inside_seconds == 90 * 60);      // 09:00 -> 10:30, counted to now

    // A FINISHED DAY still reports the real departure, and is not assumed.
    const auto done = so::rollup({at(9, 0, so::Direction::In),
                                  at(18, 0, so::Direction::Out)},
                                 hours, 24 * 3600);
    CHECK(done.last_out == 18 * 3600);
    CHECK_FALSE(done.assumed_out);
}

TEST_CASE("with no minimum, every absence adds to the break") {
    // The operator's rule: no threshold at all. However long somebody is out,
    // and however many times they go, it all accumulates.
    const auto at = [](int h, int m, so::Direction d) {
        return so::Punch{d, h * 3600 + m * 60};
    };
    so::OfficeHours hours;                       // min_break_seconds now 0 by default
    CHECK(hours.min_break_seconds == 0);

    const auto d = so::rollup({at(9, 0, so::Direction::In),
                               at(10, 0, so::Direction::Out),   // 15 min out
                               at(10, 15, so::Direction::In),
                               at(13, 0, so::Direction::Out),   // 45 min out
                               at(13, 45, so::Direction::In),
                               at(16, 0, so::Direction::Out),   // 2 min out
                               at(16, 2, so::Direction::In),
                               at(18, 0, so::Direction::Out)},
                              hours, 24 * 3600);

    CHECK(d.break_seconds == (15 + 45 + 2) * 60);   // all three, added up
    CHECK(d.entries == 4);
    CHECK(d.last_out == 18 * 3600);
    CHECK(d.inside_seconds == 9 * 3600 - (15 + 45 + 2) * 60);
}

TEST_CASE("a false exit at the reception desk is undone by the next sighting") {
    // MEASURED, 27 OCTOBER. The exit zone's far edge falls in front of the
    // reception desk, so somebody standing there leaves the zone and stops
    // being tracked — which from the module's side is indistinguishable from
    // walking out of the door.
    //
    // Mehedi: in 10:18:00, in 10:18:11 on a second camera, out 10:18:12. He had
    // arrived twelve seconds earlier. The false exit then CLOSED his day, so
    // the periodic refresh stopped touching it and he showed no working hours
    // for the rest of the morning — the damage was never really the break
    // column, it was the day being declared over.
    //
    // The signal that it was false is always the same: the person is seen again
    // almost immediately. Rejoining the two stays repairs both the break and
    // the day.
    const auto s = [](int h, int m, int sec, so::Direction d) {
        return so::Punch{d, h * 3600 + m * 60 + sec};
    };
    so::OfficeHours hours;
    hours.min_break_seconds = 60;
    const std::int64_t half_eleven = 11 * 3600 + 30 * 60;

    const auto d = so::rollup({s(10, 18, 0, so::Direction::In),
                               s(10, 18, 12, so::Direction::Out),
                               s(10, 18, 40, so::Direction::In)},
                              hours, half_eleven);

    CHECK(d.break_seconds == 0);          // 28 s is a tracking gap, not a break
    CHECK(d.last_out == 0);               // and no departure to report
    CHECK(d.assumed_out);                 // THE DAY IS OPEN AGAIN
    CHECK(d.entries == 1);                // he arrived once
    CHECK(d.inside_seconds == half_eleven - (10 * 3600 + 18 * 60));

    // A genuine short break is still counted: the threshold is about tracking
    // gaps, and ninety seconds outside is somebody who went somewhere.
    const auto real = so::rollup({s(10, 0, 0, so::Direction::In),
                                  s(10, 30, 0, so::Direction::Out),
                                  s(10, 31, 30, so::Direction::In)},
                                 hours, half_eleven);
    CHECK(real.break_seconds == 90);
    CHECK(real.last_out == 10 * 3600 + 30 * 60);
}
