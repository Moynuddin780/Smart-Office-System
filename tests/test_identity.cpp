#include "doctest/doctest.h"

#include "identity/face_identifier.hpp"
#include "identity/gallery.hpp"
#include "vision/arcface.hpp"

#include <cmath>
#include <random>

using so::EnrolledProfile;
using so::Gallery;

namespace {

std::vector<float> unit_at(int dim, int axis, float noise = 0.0f, unsigned seed = 1) {
    std::vector<float> v(static_cast<std::size_t>(dim), 0.0f);
    v[static_cast<std::size_t>(axis)] = 1.0f;
    if (noise > 0.0f) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> d(0.0f, noise);
        for (auto& x : v) x += d(rng);
    }
    so::l2_normalize(v);
    return v;
}

/// A unit vector `w` of the way from `a` towards `b` — a face that resembles one
/// gallery entry strongly but not perfectly, which is what a competing claim on
/// the same identity actually looks like.
std::vector<float> mix(const std::vector<float>& a, const std::vector<float>& b, float w) {
    std::vector<float> v(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) v[i] = w * a[i] + (1.0f - w) * b[i];
    so::l2_normalize(v);
    return v;
}

EnrolledProfile prof(std::int64_t id, const char* name, std::vector<float> e) {
    EnrolledProfile p;
    p.employee_id = id;
    p.name = name;
    p.embedding = std::move(e);
    return p;
}

std::array<so::Landmark, so::kNumLandmarks> frontal_landmarks(float yaw_shift = 0.0f) {
    // Eyes 40 px apart, level; nose centred (or shifted to fake a turn); mouth
    // below. Coordinates chosen so one eye-width is 40 px.
    std::array<so::Landmark, so::kNumLandmarks> lm{};
    lm[so::LM_LEFT_EYE]    = {100.0f, 100.0f};
    lm[so::LM_RIGHT_EYE]   = {140.0f, 100.0f};
    lm[so::LM_NOSE]        = {120.0f + yaw_shift * 40.0f, 120.0f};
    lm[so::LM_LEFT_MOUTH]  = {108.0f, 140.0f};
    lm[so::LM_RIGHT_MOUTH] = {132.0f, 140.0f};
    return lm;
}

}  // namespace

// ── gallery ────────────────────────────────────────────────────────────────

TEST_CASE("the version bumps on every write so live workers notice") {
    Gallery g;
    const auto v0 = g.version();
    g.set({prof(1, "Aslam", unit_at(512, 0))});
    CHECK(g.version() > v0);
    CHECK(g.count() == 1);

    const auto v1 = g.version();
    g.set({prof(1, "Aslam", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});
    CHECK(g.version() > v1);
    CHECK(g.count() == 2);
}

TEST_CASE("a snapshot taken before a write is unaffected by it") {
    // This is the property that makes holding a snapshot across frames safe.
    Gallery g;
    g.set({prof(1, "A", unit_at(512, 0))});
    const auto before = g.snapshot();
    g.set({prof(1, "A", unit_at(512, 0)), prof(2, "B", unit_at(512, 1))});
    CHECK(before.centred.size() == 1);
    CHECK(g.snapshot().centred.size() == 2);
    CHECK(g.version() > before.version);
}

TEST_CASE("a minority embedding width is dropped, not allowed to poison the gallery") {
    Gallery g;
    const auto r = g.set({prof(1, "A", unit_at(512, 0)),
                          prof(2, "B", unit_at(512, 1)),
                          prof(3, "C", unit_at(128, 0))});
    CHECK(r.kept == 2);
    CHECK(r.dim == 512);
    CHECK(r.skipped == 1);
    REQUIRE(r.skipped_ids.size() == 1);
    CHECK(r.skipped_ids[0] == 3);
    CHECK(g.count() == 2);
}

TEST_CASE("an empty embedding is skipped rather than stored") {
    Gallery g;
    const auto r = g.set({prof(1, "A", unit_at(512, 0)), prof(2, "B", {})});
    CHECK(r.kept == 1);
    CHECK(r.skipped == 1);
}

TEST_CASE("the centroid is a unit vector and the rows are centred by it") {
    Gallery g;
    g.set({prof(1, "A", unit_at(512, 0, 0.05f, 1)),
           prof(2, "B", unit_at(512, 0, 0.05f, 2)),
           prof(3, "C", unit_at(512, 0, 0.05f, 3))});
    const auto s = g.snapshot();

    double n = 0;
    for (float x : s.centroid) n += static_cast<double>(x) * x;
    CHECK(std::sqrt(n) == doctest::Approx(1.0).epsilon(1e-4));

    // Three profiles that are nearly the same direction are almost pure common
    // component; after centring they must spread out, which is the whole point.
    for (const auto& row : s.centred) {
        double rn = 0;
        for (float x : row) rn += static_cast<double>(x) * x;
        CHECK(std::sqrt(rn) == doctest::Approx(1.0).epsilon(1e-4));
    }
    CHECK(so::cosine(s.centred[0], s.centred[1]) < 0.99f);
}

TEST_CASE("a ONE-profile gallery can still match — the centroid is not subtracted") {
    // The failure this guards: with one enrolled person the centroid IS that
    // person, so row - centroid is the zero vector and nobody is ever matched.
    // Found on a running deployment that detected 1033 faces and identified none.
    Gallery g;
    g.set({prof(7, "Only Person", unit_at(512, 0))});
    const auto s = g.snapshot();
    REQUIRE(s.centred.size() == 1);

    double n = 0;
    for (float x : s.centred[0]) n += static_cast<double>(x) * x;
    CHECK(std::sqrt(n) == doctest::Approx(1.0).epsilon(1e-4));   // NOT annihilated

    so::FaceIdentifier id(g);
    so::Identification r;
    for (int i = 0; i < 4; ++i)
        r = id.observe(1, unit_at(512, 0, 0.02f, static_cast<unsigned>(i)), 0.02f, 120.f, 0.9f);
    CHECK(r.employee_id == 7);
}

TEST_CASE("a TWO-profile gallery is also left uncentred") {
    // With two, the centroid is their midpoint: subtracting it pushes them to
    // opposite poles and makes every live face look like one or the other.
    Gallery g;
    g.set({prof(7, "A", unit_at(512, 0)), prof(8, "B", unit_at(512, 1))});
    const auto s = g.snapshot();
    for (float c : s.centroid) CHECK(c == 0.0f);

    so::FaceIdentifier id(g);
    so::Identification r;
    for (int i = 0; i < 4; ++i)
        r = id.observe(1, unit_at(512, 1, 0.02f, static_cast<unsigned>(i)), 0.02f, 120.f, 0.9f);
    CHECK(r.employee_id == 8);
}

TEST_CASE("three or more profiles ARE centred") {
    Gallery g;
    g.set({prof(1, "A", unit_at(512, 0, 0.05f, 1)),
           prof(2, "B", unit_at(512, 0, 0.05f, 2)),
           prof(3, "C", unit_at(512, 0, 0.05f, 3))});
    const auto s = g.snapshot();
    bool nonzero = false;
    for (float c : s.centroid) if (c != 0.0f) { nonzero = true; break; }
    CHECK(nonzero);
}

TEST_CASE("an empty gallery is valid, not a crash") {
    Gallery g;
    const auto r = g.set({});
    CHECK(r.kept == 0);
    CHECK(g.count() == 0);
    CHECK(g.snapshot().centred.empty());
}

// ── head pose ──────────────────────────────────────────────────────────────

TEST_CASE("a symmetric face reads as square on") {
    const auto p = so::face_pose(frontal_landmarks());
    CHECK(std::abs(p.yaw) < 0.02f);
    CHECK(std::abs(p.roll) < 0.5f);
}

TEST_CASE("yaw is measured in eye-widths and signed") {
    CHECK(so::face_pose(frontal_landmarks(+0.30f)).yaw == doctest::Approx(0.30f).epsilon(0.02));
    CHECK(so::face_pose(frontal_landmarks(-0.30f)).yaw == doctest::Approx(-0.30f).epsilon(0.02));
}

TEST_CASE("head roll does not leak into yaw") {
    // Rotate a frontal face by 25 degrees about the eye midpoint. It is tilted,
    // not turned, and must still read as square on — otherwise every tilted head
    // is discarded as a profile view.
    auto lm = frontal_landmarks();
    const float cx = 120.0f, cy = 120.0f;
    const float a = 25.0f * 3.14159265f / 180.0f;
    for (auto& p : lm) {
        const float dx = p.x - cx, dy = p.y - cy;
        p.x = cx + dx * std::cos(a) - dy * std::sin(a);
        p.y = cy + dx * std::sin(a) + dy * std::cos(a);
    }
    const auto pose = so::face_pose(lm);
    CHECK(std::abs(pose.yaw) < 0.05f);
    CHECK(std::abs(pose.roll) == doctest::Approx(25.0f).epsilon(0.1));
}

// ── identification ─────────────────────────────────────────────────────────

TEST_CASE("a turned face may not carry an identity at any threshold") {
    Gallery g;
    g.set({prof(1, "Aslam", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    // Perfect embedding, ample size — but turned past the gate.
    for (int i = 0; i < 20; ++i)
        id.observe(1, unit_at(512, 0), /*yaw*/0.40f, /*px*/150.f, /*det*/0.95f);

    CHECK(id.current(1).employee_id == 0);
    CHECK_FALSE(id.was_judged(1));    // no frame was ever good enough to judge
}

TEST_CASE("a face too small or too weakly detected is not judged either") {
    Gallery g;
    g.set({prof(1, "Aslam", unit_at(512, 0))});
    so::FaceIdentifier id(g);

    // Below min_face_px the landmarks are not trustworthy enough to align on,
    // so no number of frames may produce a name.
    for (int i = 0; i < 20; ++i) id.observe(1, unit_at(512, 0), 0.0f, 18.f, 0.95f);
    CHECK_FALSE(id.was_judged(1));

    for (int i = 0; i < 20; ++i) id.observe(2, unit_at(512, 0), 0.0f, 150.f, 0.40f);
    CHECK_FALSE(id.was_judged(2));
}

TEST_CASE("a small but usable face is admitted on evidence, not refused on size") {
    // The sub-streams these cameras deliver put faces at 25-30 px, and the
    // deployed model names them correctly at that size. A hard size floor threw
    // those away before any comparison and reported it as a 0.00 score, which
    // reads as "no resemblance" rather than "never looked at".
    //
    // What replaces the floor is the weighting that was always there: a 30 px
    // face carries 30/96 of a frame's weight, so it takes several square-on
    // looks to reach min_weight where one close look would do.
    Gallery g;
    g.set({prof(1, "Aslam", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    // One glance at 30 px is NOT enough — evidence has not accumulated yet.
    id.observe(1, unit_at(512, 0), 0.0f, 30.f, 0.95f);
    CHECK(id.current(1).employee_id == 0);
    CHECK(id.was_judged(1));           // but it WAS weighed, unlike before

    // Keep looking, and the same face earns its name.
    for (int i = 0; i < 6; ++i) id.observe(1, unit_at(512, 0), 0.0f, 30.f, 0.95f);
    CHECK(id.current(1).employee_id == 1);
}

TEST_CASE("one person cannot be named on two tracks at once") {
    // Straight from the live wall: two different people in one frame, both
    // labelled "Moynuddin", one at 0.73 and one at 0.76. Each track scored
    // itself in isolation, so nothing noticed. Whoever reads that screen cannot
    // tell which label is real, and attendance would happily take both.
    Gallery g;
    g.set({prof(1, "Moynuddin", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    // Track 1 settles on Moynuddin from a good look.
    for (int i = 0; i < 3; ++i) id.observe(1, unit_at(512, 0), 0.0f, 150.f, 0.95f);
    CHECK(id.current(1).employee_id == 1);

    // Track 2 looks like Moynuddin too, but less convincingly. It must NOT be
    // given the name — and it must still report its score, so the picture can
    // say "? 0.xx" rather than claiming the face was never weighed.
    const auto nearly = mix(unit_at(512, 0), unit_at(512, 2), 0.82f);
    for (int i = 0; i < 3; ++i) id.observe(2, nearly, 0.0f, 150.f, 0.95f);
    CHECK(id.current(2).employee_id == 0);
    CHECK(id.current(2).score > 0.0f);
    CHECK(id.current(1).employee_id == 1);     // the stronger claim is untouched
}

TEST_CASE("the stronger claim takes the name and the weaker one is released") {
    Gallery g;
    g.set({prof(1, "Moynuddin", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    // A so-so look settles track 1 on Moynuddin first.
    const auto soso = mix(unit_at(512, 0), unit_at(512, 2), 0.80f);
    for (int i = 0; i < 3; ++i) id.observe(1, soso, 0.0f, 150.f, 0.95f);
    CHECK(id.current(1).employee_id == 1);

    // Track 2 then produces a perfect match. Arriving second must not lose it
    // the name: the evidence decides, not the order.
    for (int i = 0; i < 3; ++i) id.observe(2, unit_at(512, 0), 0.0f, 150.f, 0.95f);
    CHECK(id.current(2).employee_id == 1);
    CHECK(id.current(1).employee_id == 0);     // released, free to find its own
}

TEST_CASE("a good frontal look names the right person") {
    Gallery g;
    g.set({prof(7, "Aslam", unit_at(512, 0)),
           prof(8, "Rahim", unit_at(512, 1)),
           prof(9, "Karim", unit_at(512, 2))});
    so::FaceIdentifier id(g);

    so::Identification r;
    for (int i = 0; i < 4; ++i)
        r = id.observe(1, unit_at(512, 0, 0.02f, static_cast<unsigned>(i)),
                       0.02f, 120.f, 0.9f);

    CHECK(id.was_judged(1));
    CHECK(r.employee_id == 7);
    CHECK(r.name == "Aslam");
    CHECK(r.settled);
    CHECK(r.margin > 0.06f);
}

TEST_CASE("one good look is not enough on its own") {
    Gallery g;
    g.set({prof(7, "Aslam", unit_at(512, 0)), prof(8, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);
    // A single frame carries weight ~0.8 against a min_weight of 1.2, so it is
    // judged but not yet named — evidence, not a verdict.
    const auto r = id.observe(1, unit_at(512, 0), 0.0f, 120.f, 0.9f);
    CHECK(id.was_judged(1));
    CHECK(r.employee_id == 0);
}

TEST_CASE("a stranger is judged but not named") {
    // This is the distinction the unauthorised-person module rests on: we got a
    // proper look at this face and it matches nobody.
    Gallery g;
    g.set({prof(7, "Aslam", unit_at(512, 0)), prof(8, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    for (int i = 0; i < 6; ++i)
        id.observe(1, unit_at(512, 300), 0.0f, 120.f, 0.9f);

    CHECK(id.was_judged(1));
    CHECK(id.current(1).employee_id == 0);
}

TEST_CASE("a settled name is not stolen by a marginally better challenger") {
    Gallery g;
    g.set({prof(7, "Aslam", unit_at(512, 0)), prof(8, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    for (int i = 0; i < 6; ++i) id.observe(1, unit_at(512, 0), 0.0f, 120.f, 0.9f);
    REQUIRE(id.current(1).employee_id == 7);

    // A handful of ambiguous frames must not rename the track.
    for (int i = 0; i < 3; ++i)
        id.observe(1, unit_at(512, 1, 0.9f, static_cast<unsigned>(i)), 0.0f, 120.f, 0.9f);
    CHECK(id.current(1).employee_id == 7);
}

TEST_CASE("an empty gallery names nobody but still records that we looked") {
    Gallery g;
    so::FaceIdentifier id(g);
    for (int i = 0; i < 6; ++i) id.observe(1, unit_at(512, 0), 0.0f, 120.f, 0.9f);
    CHECK(id.was_judged(1));
    CHECK(id.current(1).employee_id == 0);
}

TEST_CASE("tracks are pruned once they go stale") {
    Gallery g;
    g.set({prof(7, "Aslam", unit_at(512, 0))});
    so::IdentifierConfig cfg;
    cfg.track_ttl = std::chrono::seconds(2);
    so::FaceIdentifier id(g, cfg);

    const auto t0 = std::chrono::steady_clock::now();
    id.observe(1, unit_at(512, 0), 0.0f, 120.f, 0.9f, t0);
    CHECK(id.tracked() == 1);
    id.prune(t0 + std::chrono::seconds(1));
    CHECK(id.tracked() == 1);
    id.prune(t0 + std::chrono::seconds(5));
    CHECK(id.tracked() == 0);
}

TEST_CASE("a face the detector accepted is weighed, not silently discarded") {
    // The detector admits a face at 0.50 and the live view draws a box around
    // it. The identifier then demanded 0.70, so every detection between the two
    // was shown to the operator and never compared to anybody — a clearly
    // recognisable 34 px face on this site was refused that way, and the only
    // thing the picture said was "faint".
    Gallery g;
    g.set({prof(1, "Aslam", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    // A hesitant but accepted detection, square on and a good size.
    for (int i = 0; i < 3; ++i)
        id.observe(1, unit_at(512, 0), 0.0f, 150.f, /*det*/0.55f);
    CHECK(id.was_judged(1));
    CHECK(id.current(1).employee_id == 1);

    // Still refused below the detector's own bar — nothing there to weigh.
    for (int i = 0; i < 20; ++i)
        id.observe(2, unit_at(512, 0), 0.0f, 150.f, /*det*/0.40f);
    CHECK_FALSE(id.was_judged(2));
}

TEST_CASE("confidence counts for less rather than counting for nothing") {
    // The hesitant face needs more looks than the confident one, which is the
    // point: it is admitted, but it has to earn the name.
    Gallery g;
    g.set({prof(1, "Aslam", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});

    so::FaceIdentifier sure(g), unsure(g);
    sure.observe(1, unit_at(512, 0), 0.0f, 150.f, 0.99f);
    sure.observe(1, unit_at(512, 0), 0.0f, 150.f, 0.99f);
    unsure.observe(1, unit_at(512, 0), 0.0f, 150.f, 0.52f);
    unsure.observe(1, unit_at(512, 0), 0.0f, 150.f, 0.52f);

    CHECK(sure.current(1).weight > unsure.current(1).weight);
}

TEST_CASE("a box that changes person loses the name instead of lending it") {
    // REGRESSION TEST FOR A NAME LANDING ON A STRANGER.
    //
    // Identity is bound to the TRACK, which is right while the track follows
    // one person. When two people cross, the tracker can hand the box to the
    // other one, and the name goes with it — observed on this site: a label
    // moved onto a passer-by, and their frames then pooled into the template of
    // somebody they are not, corrupting it for everyone afterwards.
    //
    // The faces are what give it away. Within one person frames agree at about
    // 0.6 and up; different people here average 0.18 and never pass 0.44.
    Gallery g;
    g.set({prof(1, "Noushin", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    for (int i = 0; i < 6; ++i) id.observe(1, unit_at(512, 0), 0.0f, 120.f, 0.9f);
    REQUIRE(id.current(1).employee_id == 1);

    // One odd frame is noise — a blur, a half-turn — and must change nothing.
    id.observe(1, unit_at(512, 300), 0.0f, 120.f, 0.9f);
    CHECK(id.current(1).employee_id == 1);

    // Somebody else now occupies the box. Sustained disagreement, so the name
    // is dropped rather than lent out.
    for (int i = 0; i < 4; ++i) id.observe(1, unit_at(512, 300), 0.0f, 120.f, 0.9f);
    CHECK(id.current(1).employee_id == 0);
}

TEST_CASE("evidence from somebody who left too soon is reported, not thrown away") {
    // A PERSON WALKING OUT GETS ONE OR TWO LOOKS, NOT FIVE.
    //
    // rescore() returns immediately when the pooled weight is under the bar, so
    // for exactly the people this system most needs to name — the ones leaving
    // — no comparison was ever made. The frames were captured, embedded, pooled
    // and then dropped on the floor, and the exit was recorded as "unknown"
    // with nothing to show for it.
    //
    // best_guess() asks the question anyway and reports what the evidence says,
    // WITH its score, margin and weight so the caller can see how thin it is.
    // It is not an identification and must never be written as one; it is the
    // difference between "we do not know" and "we did not look".
    Gallery g;
    g.set({prof(1, "Alice", unit_at(512, 0)), prof(2, "Bob", unit_at(512, 1))});

    so::IdentifierConfig cfg;
    cfg.min_weight = 1.2f;              // more than one glance
    so::FaceIdentifier id(g, cfg);

    // ONE small, quick look at Alice — below the weight bar on purpose.
    const auto v = unit_at(512, 0);
    const auto settled = id.observe(7, v, 0.0f, 30.0f, 0.9f);
    CHECK(settled.employee_id == 0);            // correctly refuses to name her
    CHECK(settled.weight < cfg.min_weight);

    // The verdict stays empty — that part is right and must not change.
    CHECK(id.current(7).employee_id == 0);

    // But the evidence is there to be read.
    const auto guess = id.best_guess(7);
    CHECK(guess.employee_id == 1);              // it did look like Alice
    CHECK(guess.name == "Alice");
    CHECK(guess.score > 0.5f);
    CHECK(guess.weight == doctest::Approx(settled.weight));
    CHECK_FALSE(guess.settled);                 // never presented as settled

    // A track nobody ever saw a face on has nothing to guess from.
    CHECK(id.best_guess(999).employee_id == 0);
}

TEST_CASE("a track that has been seen once but cannot yet be named keeps its claim on the face budget") {
    // WHY THIS EXISTS
    //
    // The camera can crop only so many faces per cycle, and it ordered people
    // by was_judged() — "has this track ever produced a usable frame". That is
    // true after ONE frame, while naming needs min_weight of evidence: about
    // five square-on looks at a small face. So three people walking in together
    // each got one look, all three then counted as "seen", and from the next
    // cycle the budget went to whoever was nearest. The person at the back was
    // never looked at again and never named — which is exactly the report from
    // the floor: two or three enter together, one is missed.
    Gallery g;
    g.set({prof(1, "Aslam", unit_at(512, 0)), prof(2, "Rahim", unit_at(512, 1))});
    so::FaceIdentifier id(g);

    CHECK(id.needs_more_looks(7));          // never seen at all

    id.observe(7, unit_at(512, 0), 0.0f, 30.f, 0.95f);
    CHECK(id.was_judged(7));                // one usable frame — "seen"
    CHECK(id.current(7).employee_id == 0);  // ...but still nameless
    CHECK(id.needs_more_looks(7));          // so it must NOT lose its priority

    for (int i = 0; i < 6; ++i) id.observe(7, unit_at(512, 0), 0.0f, 30.f, 0.95f);
    CHECK(id.current(7).employee_id == 1);  // named
    CHECK_FALSE(id.needs_more_looks(7));    // and now it stops asking
}

TEST_CASE("a face nobody recognises stops asking for the budget once it has had its share") {
    // The bound that keeps the rule above from backfiring: somebody who is not
    // enrolled can never be named, so "still nameless" alone would let them
    // hold the budget for as long as they stood in frame, starving the people
    // the system CAN identify.
    Gallery g;
    g.set({prof(1, "Aslam", unit_at(512, 0))});
    so::FaceIdentifier id(g);

    const auto stranger = unit_at(512, 300);
    for (int i = 0; i < 12; ++i) id.observe(9, stranger, 0.0f, 96.f, 0.95f);
    CHECK(id.current(9).employee_id == 0);  // never named, correctly
    CHECK_FALSE(id.needs_more_looks(9));    // but no longer hogging the budget
}
