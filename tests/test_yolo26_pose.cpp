#include "doctest/doctest.h"

#include "fixture_loader.hpp"
#include "vision/yolo26_pose.hpp"

#include <algorithm>
#include <string>

namespace {
const std::string kManifest = "tests/fixtures/pose_raw.manifest";
const std::string kExpected = "tests/fixtures/pose_expected.csv";
}

TEST_CASE("the raw NPU head decodes to exactly what the ORT path produced") {
    so_test::FixtureTensors raw(kManifest);
    const auto expect = so_test::load_expected(kExpected);
    REQUIRE(expect.size() >= 2);

    so::Yolo26PoseDecoder dec(640, 0.5f, 0.35f);
    auto got = dec.decode(raw.views());

    REQUIRE(got.size() == expect.size());

    for (std::size_t i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        CHECK(got[i].score == doctest::Approx(expect[i].score).epsilon(1e-4));
        for (int k = 0; k < 4; ++k) {
            CAPTURE(k);
            CHECK(got[i].box[static_cast<std::size_t>(k)] ==
                  doctest::Approx(expect[i].box[k]).epsilon(1e-3));
        }
        for (int k = 0; k < so::kNumKeypoints; ++k) {
            CAPTURE(k);
            const auto& g = got[i].kps[static_cast<std::size_t>(k)];
            CHECK(g.x    == doctest::Approx(expect[i].kps[k][0]).epsilon(1e-3));
            CHECK(g.y    == doctest::Approx(expect[i].kps[k][1]).epsilon(1e-3));
            CHECK(g.conf == doctest::Approx(expect[i].kps[k][2]).epsilon(1e-4));
        }
    }
}

// The keypoint convention is not documented anywhere; it was derived by fitting
// candidates against the ORT reference on a real frame (2026-09-23). The mean
// per-keypoint pixel error of each candidate was:
//
//     (2r + (g - 0.5)) * stride     39.756 px
//     (r + g - 0.5)    * stride     22.627 px
//     (2r + g)         * stride     29.870 px
//     (r + g)          * stride      0.000 px   <-- shipped
//
// This guard exists so that a future reader who "remembers" the Ultralytics
// v8-pose convention does not quietly swap in the first one.
TEST_CASE("the keypoint formula is the one that measured zero error") {
    CHECK(std::string(so::Yolo26PoseDecoder::kKeypointFormula) ==
          "(raw + grid_centre) * stride");
}

TEST_CASE("raising the confidence floor drops the weaker detection") {
    so_test::FixtureTensors raw(kManifest);
    const auto expect = so_test::load_expected(kExpected);

    const float second = expect[1].score;
    so::Yolo26PoseDecoder strict(640, second + 0.01f, 0.35f);
    CHECK(strict.decode(raw.views()).size() == 1);
}

TEST_CASE("detections come back sorted by score, strongest first") {
    so_test::FixtureTensors raw(kManifest);
    so::Yolo26PoseDecoder dec(640, 0.3f, 0.35f);
    const auto got = dec.decode(raw.views());
    REQUIRE(got.size() >= 2);
    CHECK(std::is_sorted(got.begin(), got.end(),
                         [](const so::PoseDetection& a, const so::PoseDetection& b) {
                             return a.score > b.score;
                         }));
}

TEST_CASE("a wrong tensor count is rejected rather than silently mis-decoded") {
    so_test::FixtureTensors raw(kManifest);
    auto views = raw.views();
    views.pop_back();
    so::Yolo26PoseDecoder dec;
    CHECK_THROWS_AS(dec.decode(views), std::runtime_error);
}

TEST_CASE("every keypoint of a confident person lands inside that person's box") {
    so_test::FixtureTensors raw(kManifest);
    so::Yolo26PoseDecoder dec(640, 0.5f, 0.35f);
    const auto got = dec.decode(raw.views());
    REQUIRE(!got.empty());

    const auto& d = got.front();
    int inside = 0, counted = 0;
    for (const auto& k : d.kps) {
        if (k.conf < 0.5f) continue;   // only judge keypoints the model believes
        ++counted;
        // A small margin: a wrist can legitimately sit a pixel outside the box.
        if (k.x >= d.box[0] - 4 && k.x <= d.box[2] + 4 &&
            k.y >= d.box[1] - 4 && k.y <= d.box[3] + 4) ++inside;
    }
    REQUIRE(counted >= 5);
    CHECK(inside == counted);
}
