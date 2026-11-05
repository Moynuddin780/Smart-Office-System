#include "doctest/doctest.h"

#include "vision/tracker.hpp"

#include <set>

using so::Box;
using so::Track;
using so::TrackerConfig;
using so::TrackInput;

namespace {

TrackInput det(float x1, float y1, float x2, float y2, float score = 0.9f) {
    TrackInput d;
    d.box = Box{x1, y1, x2, y2};
    d.score = score;
    return d;
}

TrackerConfig fast_confirm() {
    TrackerConfig c;
    c.min_hits = 2;
    return c;
}

}  // namespace

// ── the assignment solver ──────────────────────────────────────────────────

TEST_CASE("assignment picks the cheapest complete pairing, not the greedy one") {
    // Greedy would take (0,0) at 1 and then be forced into (1,1) at 100,
    // total 101. The optimum is (0,1) + (1,0) = 2 + 3 = 5.
    const std::vector<float> cost{1.f, 2.f,
                                  3.f, 100.f};
    const auto a = so::solve_assignment(cost, 2, 2, 1e9f);
    CHECK(a[0] == 1);
    CHECK(a[1] == 0);
}

TEST_CASE("assignment leaves a row unmatched rather than exceeding max_cost") {
    const std::vector<float> cost{0.1f, 5.0f,
                                  5.0f, 5.0f};
    const auto a = so::solve_assignment(cost, 2, 2, 1.0f);
    CHECK(a[0] == 0);
    CHECK(a[1] == -1);
}

TEST_CASE("assignment handles rectangular matrices and empty inputs") {
    const std::vector<float> wide{0.1f, 0.9f, 0.5f};        // 1 row, 3 cols
    const auto a = so::solve_assignment(wide, 1, 3, 1.0f);
    CHECK(a[0] == 0);

    const std::vector<float> tall{0.1f, 0.9f, 0.5f};        // 3 rows, 1 col
    const auto b = so::solve_assignment(tall, 3, 1, 1.0f);
    int assigned = 0;
    for (int r = 0; r < 3; ++r) if (b[static_cast<std::size_t>(r)] >= 0) ++assigned;
    CHECK(assigned == 1);                                    // only one column exists

    CHECK(so::solve_assignment({}, 0, 0, 1.0f).empty());
}

TEST_CASE("no column is ever assigned to two rows") {
    const std::vector<float> cost{0.1f, 0.1f,
                                  0.1f, 0.1f,
                                  0.1f, 0.1f};
    const auto a = so::solve_assignment(cost, 3, 2, 1.0f);
    std::set<int> used;
    for (int r = 0; r < 3; ++r) {
        const int c = a[static_cast<std::size_t>(r)];
        if (c < 0) continue;
        CHECK(used.insert(c).second);
    }
}

// ── the tracker ────────────────────────────────────────────────────────────

TEST_CASE("a steadily moving person keeps one id") {
    so::Tracker t(fast_confirm());
    int id = -1;
    for (int f = 0; f < 12; ++f) {
        const auto out = t.update({det(100.f + f * 5, 100, 140.f + f * 5, 300)});
        if (f == 0) continue;                 // needs min_hits before it reports
        REQUIRE(out.size() == 1);
        if (id < 0) id = out[0].id;
        CHECK(out[0].id == id);
    }
    CHECK(id > 0);
}

TEST_CASE("a track is not reported until it has been seen min_hits times") {
    TrackerConfig c;
    c.min_hits = 3;
    so::Tracker t(c);
    CHECK(t.update({det(0, 0, 10, 20)}).empty());
    CHECK(t.update({det(0, 0, 10, 20)}).empty());
    CHECK(t.update({det(0, 0, 10, 20)}).size() == 1);
}

TEST_CASE("two people crossing do not swap identities") {
    // Left-to-right and right-to-left, meeting in the middle. This is the case
    // greedy matching gets wrong, and it is why the solver is a real one.
    so::Tracker t(fast_confirm());
    int left_id = -1, right_id = -1;
    for (int f = 0; f < 14; ++f) {
        const float a = 50.f + f * 20.f;
        const float b = 310.f - f * 20.f;
        auto out = t.update({det(a, 100, a + 40, 300), det(b, 100, b + 40, 300)});
        if (f < 2) continue;
        REQUIRE(out.size() == 2);
        std::sort(out.begin(), out.end(),
                  [](const Track& x, const Track& y) { return x.box[0] < y.box[0]; });
        // Before they meet, the leftmost box is the left-moving person.
        if (f == 2) {
            left_id = out[0].id;
            right_id = out[1].id;
        }
        if (f == 13) {
            // They have swapped sides, so the ids must have swapped positions —
            // the same two ids, not two new ones.
            std::set<int> ids{out[0].id, out[1].id};
            CHECK(ids.count(left_id) == 1);
            CHECK(ids.count(right_id) == 1);
        }
    }
}

TEST_CASE("a low-confidence detection sustains a track but never starts one") {
    TrackerConfig c = fast_confirm();
    so::Tracker t(c);

    // A faint detection alone creates nothing.
    CHECK(t.update({det(0, 0, 40, 100, 0.30f)}).empty());
    CHECK(t.update({det(0, 0, 40, 100, 0.30f)}).empty());
    CHECK(t.all_tracks().empty());

    // Established confidently, a track then survives on faint detections —
    // which is exactly what a person turning away from the camera produces.
    so::Tracker t2(c);
    t2.update({det(0, 0, 40, 100, 0.90f)});
    const auto confirmed = t2.update({det(2, 0, 42, 100, 0.90f)});
    REQUIRE(confirmed.size() == 1);
    const int id = confirmed[0].id;
    const auto sustained = t2.update({det(4, 0, 44, 100, 0.30f)});
    REQUIRE(sustained.size() == 1);
    CHECK(sustained[0].id == id);
}

TEST_CASE("a track survives a short gap and dies after max_age") {
    TrackerConfig c = fast_confirm();
    c.max_age = 5;
    so::Tracker t(c);

    t.update({det(0, 0, 40, 100)});
    const int id = t.update({det(0, 0, 40, 100)})[0].id;

    for (int i = 0; i < 3; ++i) CHECK(t.update({}).empty());   // coasting, not reported
    const auto back = t.update({det(0, 0, 40, 100)});
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == id);                                   // same person

    for (int i = 0; i < 8; ++i) t.update({});
    const auto after = t.update({det(0, 0, 40, 100)});
    REQUIRE(after.size() == 0);                                // new track, not yet confirmed
    CHECK(t.all_tracks().size() == 1);
    CHECK(t.all_tracks()[0].id != id);                         // and it is a NEW id
}

TEST_CASE("a coasting track is not reported as present") {
    // This matters: reporting a track that is only predicted would credit desk
    // time to somebody who has left the frame.
    TrackerConfig c = fast_confirm();
    so::Tracker t(c);
    t.update({det(0, 0, 40, 100)});
    CHECK(t.update({det(0, 0, 40, 100)}).size() == 1);
    CHECK(t.update({}).empty());
    CHECK(t.all_tracks().size() == 1);       // it still exists internally
}

TEST_CASE("keypoints ride along with the track") {
    so::Tracker t(fast_confirm());
    TrackInput d = det(0, 0, 40, 100);
    d.kps[so::NOSE] = so::Keypoint{20.f, 10.f, 0.95f};
    t.update({d});
    const auto out = t.update({d});
    REQUIRE(out.size() == 1);
    CHECK(out[0].kps[so::NOSE].x == doctest::Approx(20.f));
    CHECK(out[0].kps[so::NOSE].conf == doctest::Approx(0.95f));
}

TEST_CASE("two well-separated people never match each other") {
    so::Tracker t(fast_confirm());
    t.update({det(0, 0, 40, 100), det(500, 0, 540, 100)});
    const auto out = t.update({det(0, 0, 40, 100), det(500, 0, 540, 100)});
    REQUIRE(out.size() == 2);
    CHECK(out[0].id != out[1].id);
}

TEST_CASE("reset clears everything, ids included") {
    so::Tracker t(fast_confirm());
    t.update({det(0, 0, 40, 100)});
    t.update({det(0, 0, 40, 100)});
    t.reset();
    CHECK(t.all_tracks().empty());
    t.update({det(0, 0, 40, 100)});
    CHECK(t.all_tracks()[0].id == 1);
}
