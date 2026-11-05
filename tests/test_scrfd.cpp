#include "doctest/doctest.h"

#include "fixture_loader.hpp"
#include "vision/scrfd.hpp"
#include "vision/yolo26_pose.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace {
const std::string kManifest = "tests/fixtures/scrfd_raw.manifest";
const std::string kExpected = "tests/fixtures/scrfd_expected.csv";
const std::string kPoseRef  = "tests/fixtures/scrfd_pose_expected.csv";
}

TEST_CASE("the native NPU head decodes to the same faces the reference produced") {
    so_test::FixtureTensors raw(kManifest);
    const auto expect = so_test::load_expected_faces(kExpected);
    REQUIRE(expect.size() >= 3);

    so::ScrfdDecoder dec(640, 0.5f, 0.4f);
    const auto got = dec.decode(raw.views());

    // The two paths see the tensors in DIFFERENT orders (the reference reads
    // ONNX Runtime's canonical order; this decoder groups the native NPU output
    // by channel count), so agreeing here is a real check on the grouping, not
    // just arithmetic agreeing with itself.
    REQUIRE(got.size() == expect.size());

    for (std::size_t i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        CHECK(got[i].score == doctest::Approx(expect[i].score).epsilon(1e-4));
        for (int k = 0; k < 4; ++k) {
            CAPTURE(k);
            CHECK(got[i].box[static_cast<std::size_t>(k)] ==
                  doctest::Approx(expect[i].box[k]).epsilon(1e-3));
        }
        for (int k = 0; k < so::kNumLandmarks; ++k) {
            CAPTURE(k);
            CHECK(got[i].lm[static_cast<std::size_t>(k)].x ==
                  doctest::Approx(expect[i].lm[k][0]).epsilon(1e-3));
            CHECK(got[i].lm[static_cast<std::size_t>(k)].y ==
                  doctest::Approx(expect[i].lm[k][1]).epsilon(1e-3));
        }
    }
}

// Independent geometric check: the pose model, a completely different network,
// puts a nose keypoint on the same people. SCRFD's own nose LANDMARK must land
// on it. This is what settled the anchor placement — 96 paired samples across
// the clip corpus gave a median error of 0.2364 eye-widths at cell offset 0.0
// against 0.9737 at 0.5.
TEST_CASE("SCRFD's nose landmark agrees with the pose model's nose keypoint") {
    so_test::FixtureTensors raw(kManifest);
    so::ScrfdDecoder dec(640, 0.5f, 0.4f);
    const auto faces = dec.decode(raw.views());
    const auto poses = so_test::load_expected(kPoseRef);
    REQUIRE(!faces.empty());
    REQUIRE(!poses.empty());

    int compared = 0;
    for (const auto& p : poses) {
        const float nose_x = p.kps[so::NOSE][0];
        const float nose_y = p.kps[so::NOSE][1];
        const float nose_c = p.kps[so::NOSE][2];
        const float le_c = p.kps[so::LEFT_EYE][2];
        const float re_c = p.kps[so::RIGHT_EYE][2];
        if (nose_c < 0.6f || le_c < 0.6f || re_c < 0.6f) continue;

        const float eye_d = std::hypot(p.kps[so::LEFT_EYE][0] - p.kps[so::RIGHT_EYE][0],
                                       p.kps[so::LEFT_EYE][1] - p.kps[so::RIGHT_EYE][1]);
        if (eye_d < 4.0f) continue;

        float best = 1e9f;
        for (const auto& f : faces) {
            const auto& n = f.lm[so::LM_NOSE];
            best = std::min(best, std::hypot(n.x - nose_x, n.y - nose_y));
        }
        ++compared;
        CAPTURE(compared);
        // Half a stride at stride 32 is 16 px — roughly one eye-width on these
        // faces — so this bound separates the two anchor conventions cleanly.
        CHECK(best / eye_d < 0.6f);
    }
    REQUIRE(compared >= 1);
}

TEST_CASE("the anchor sits at the cell origin, as measured") {
    CHECK(so::ScrfdDecoder::kCellOffset == doctest::Approx(0.0f));
}

TEST_CASE("landmarks fall inside their own face box") {
    so_test::FixtureTensors raw(kManifest);
    so::ScrfdDecoder dec(640, 0.5f, 0.4f);
    for (const auto& f : dec.decode(raw.views())) {
        const float w = f.box[2] - f.box[0];
        const float h = f.box[3] - f.box[1];
        REQUIRE(w > 0.0f);
        REQUIRE(h > 0.0f);
        for (const auto& l : f.lm) {
            CHECK(l.x >= f.box[0] - 0.25f * w);
            CHECK(l.x <= f.box[2] + 0.25f * w);
            CHECK(l.y >= f.box[1] - 0.25f * h);
            CHECK(l.y <= f.box[3] + 0.25f * h);
        }
    }
}

TEST_CASE("the eyes are above the mouth and left is left of right") {
    so_test::FixtureTensors raw(kManifest);
    so::ScrfdDecoder dec(640, 0.5f, 0.4f);
    const auto faces = dec.decode(raw.views());
    REQUIRE(!faces.empty());
    for (const auto& f : faces) {
        const float eye_y = (f.lm[so::LM_LEFT_EYE].y + f.lm[so::LM_RIGHT_EYE].y) / 2.f;
        const float mouth_y = (f.lm[so::LM_LEFT_MOUTH].y + f.lm[so::LM_RIGHT_MOUTH].y) / 2.f;
        CHECK(eye_y < mouth_y);
        CHECK(f.lm[so::LM_LEFT_EYE].x < f.lm[so::LM_RIGHT_EYE].x);
    }
}

TEST_CASE("NMS collapses the overlapping proposals a face generates") {
    so_test::FixtureTensors raw(kManifest);
    so::ScrfdDecoder loose(640, 0.5f, 0.99f);   // effectively no suppression
    so::ScrfdDecoder normal(640, 0.5f, 0.4f);
    CHECK(loose.decode(raw.views()).size() > normal.decode(raw.views()).size());
}

TEST_CASE("an ORT-shaped tensor reaching the native decoder is rejected, not guessed") {
    so_test::FixtureTensors raw(kManifest);
    auto views = raw.views();
    views[0].shape = {1, 12800, 1, 1};          // the (1,A,1) ORT score layout
    so::ScrfdDecoder dec;
    CHECK_THROWS_AS(dec.decode(views), std::runtime_error);
}
