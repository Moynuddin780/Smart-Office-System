#include "doctest/doctest.h"

#include "device.hpp"

#include "fixture_loader.hpp"
#include "vision/arcface.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::vector<float> load_f32(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<float> v(bytes / sizeof(float));
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(bytes));
    return v;
}

std::vector<float> load_csv_row(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::string line;
    std::getline(f, line);
    std::vector<float> v;
    std::istringstream ls(line);
    std::string cell;
    while (std::getline(ls, cell, ',')) v.push_back(std::stof(cell));
    return v;
}

std::array<so::Landmark, so::kNumLandmarks> load_landmarks(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::string line;
    std::getline(f, line);
    std::vector<float> v;
    std::istringstream ls(line);
    std::string cell;
    while (std::getline(ls, cell, ',')) v.push_back(std::stof(cell));
    if (v.size() != 10) throw std::runtime_error("expected 10 landmark values");
    std::array<so::Landmark, so::kNumLandmarks> lm{};
    for (int i = 0; i < so::kNumLandmarks; ++i)
        lm[static_cast<std::size_t>(i)] =
            so::Landmark{v[static_cast<std::size_t>(i * 2)],
                         v[static_cast<std::size_t>(i * 2 + 1)]};
    return lm;
}

}  // namespace

// ── umeyama: pure maths, no device ─────────────────────────────────────────

TEST_CASE("umeyama maps a point set onto itself with the identity") {
    const std::vector<cv::Point2f> pts(so::kArcFaceRef.begin(), so::kArcFaceRef.end());
    const cv::Mat M = so::umeyama(pts, pts);
    CHECK(M.at<double>(0, 0) == doctest::Approx(1.0).epsilon(1e-9));
    CHECK(M.at<double>(1, 1) == doctest::Approx(1.0).epsilon(1e-9));
    CHECK(M.at<double>(0, 1) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(M.at<double>(1, 0) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(M.at<double>(0, 2) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(M.at<double>(1, 2) == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("umeyama recovers a known scale, rotation and translation") {
    const double s = 2.5, th = 0.37, tx = 11.0, ty = -4.0;
    std::vector<cv::Point2f> src(so::kArcFaceRef.begin(), so::kArcFaceRef.end());
    std::vector<cv::Point2f> dst;
    for (const auto& p : src) {
        const double x = s * (std::cos(th) * p.x - std::sin(th) * p.y) + tx;
        const double y = s * (std::sin(th) * p.x + std::cos(th) * p.y) + ty;
        dst.emplace_back(static_cast<float>(x), static_cast<float>(y));
    }
    const cv::Mat M = so::umeyama(src, dst);

    // Every source point must land on its destination.
    for (std::size_t i = 0; i < src.size(); ++i) {
        const double x = M.at<double>(0, 0) * src[i].x + M.at<double>(0, 1) * src[i].y +
                         M.at<double>(0, 2);
        const double y = M.at<double>(1, 0) * src[i].x + M.at<double>(1, 1) * src[i].y +
                         M.at<double>(1, 2);
        CHECK(x == doctest::Approx(dst[i].x).epsilon(1e-6));
        CHECK(y == doctest::Approx(dst[i].y).epsilon(1e-6));
    }
    // ...and the recovered scale is the one we applied.
    const double recovered = std::hypot(M.at<double>(0, 0), M.at<double>(1, 0));
    CHECK(recovered == doctest::Approx(s).epsilon(1e-6));
}

TEST_CASE("umeyama does not fit a reflection") {
    // Mirror the template in x. A similarity transform cannot reach it, and the
    // determinant of the returned rotation must stay positive rather than the
    // solver quietly flipping the face.
    std::vector<cv::Point2f> src(so::kArcFaceRef.begin(), so::kArcFaceRef.end());
    std::vector<cv::Point2f> dst;
    for (const auto& p : src) dst.emplace_back(112.0f - p.x, p.y);
    const cv::Mat M = so::umeyama(src, dst);
    const double det = M.at<double>(0, 0) * M.at<double>(1, 1) -
                       M.at<double>(0, 1) * M.at<double>(1, 0);
    CHECK(det > 0.0);
}

TEST_CASE("umeyama rejects mismatched point sets") {
    std::vector<cv::Point2f> a{{0, 0}, {1, 1}};
    std::vector<cv::Point2f> b{{0, 0}};
    CHECK_THROWS_AS(so::umeyama(a, b), std::invalid_argument);
}

TEST_CASE("alignment residuals match the reference exactly") {
    const cv::Mat canvas = cv::imread("tests/fixtures/scrfd_canvas.png");
    REQUIRE(!canvas.empty());
    const auto lm = load_landmarks("tests/fixtures/arcface_landmarks.csv");

    const cv::Mat crop = so::align_face(canvas, lm);
    CHECK(crop.cols == 112);
    CHECK(crop.rows == 112);
    CHECK(crop.type() == CV_8UC3);

    // A 4-DOF similarity cannot place five landmarks exactly, and it SHOULD
    // not: the residual is the fit declining to reshape a real head into the
    // frontal template. Its size therefore depends on the subject's pose, so
    // asserting an arbitrary pixel bound would be asserting something about
    // this fixture's face rather than about the code. What must hold is that
    // the C++ fit is the SAME fit as the reference, so the residuals are
    // compared against the ones the generator recorded.
    const auto expected_res = load_csv_row("tests/fixtures/arcface_residuals.csv");
    REQUIRE(expected_res.size() == so::kNumLandmarks);

    std::vector<cv::Point2f> src;
    for (const auto& p : lm) src.emplace_back(p.x, p.y);
    const std::vector<cv::Point2f> dst(so::kArcFaceRef.begin(), so::kArcFaceRef.end());
    const cv::Mat M = so::umeyama(src, dst);

    for (std::size_t i = 0; i < src.size(); ++i) {
        CAPTURE(i);
        const double x = M.at<double>(0, 0) * src[i].x + M.at<double>(0, 1) * src[i].y +
                         M.at<double>(0, 2);
        const double y = M.at<double>(1, 0) * src[i].x + M.at<double>(1, 1) * src[i].y +
                         M.at<double>(1, 2);
        const double residual = std::hypot(x - dst[i].x, y - dst[i].y);
        CHECK(residual == doctest::Approx(expected_res[i]).epsilon(1e-4));
    }
}

TEST_CASE("the C++ alignment reproduces the Python reference crop") {
    const cv::Mat canvas = cv::imread("tests/fixtures/scrfd_canvas.png");
    const cv::Mat expected = cv::imread("tests/fixtures/arcface_aligned.png");
    REQUIRE(!canvas.empty());
    REQUIRE(!expected.empty());

    const auto lm = load_landmarks("tests/fixtures/arcface_landmarks.csv");
    const cv::Mat got = so::align_face(canvas, lm);

    cv::Mat diff;
    cv::absdiff(got, expected, diff);
    const cv::Scalar mean = cv::mean(diff);
    // Both warps are cv::warpAffine with the same matrix, so this is an
    // interpolation-level comparison, not a tolerance for a different transform.
    CHECK(mean[0] < 1.0);
    CHECK(mean[1] < 1.0);
    CHECK(mean[2] < 1.0);
}

TEST_CASE("cosine of a vector with itself is 1, and it rejects length mismatches") {
    std::vector<float> v(so::kEmbeddingDim, 0.0f);
    v[0] = 3.0f;
    v[1] = 4.0f;
    so::l2_normalize(v);
    CHECK(so::cosine(v, v) == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(so::cosine(v, std::vector<float>(so::kEmbeddingDim, 0.0f)) ==
          doctest::Approx(0.0f));
    CHECK_THROWS_AS(so::cosine(v, std::vector<float>(8, 1.0f)), std::invalid_argument);
}

TEST_CASE("l2_normalize leaves a zero vector alone rather than producing NaNs") {
    std::vector<float> z(8, 0.0f);
    so::l2_normalize(z);
    for (float x : z) CHECK(x == 0.0f);
}

// ── device check ───────────────────────────────────────────────────────────

TEST_CASE("the C++ embedding matches the Python reference for the same crop") {
    SO_REQUIRE_NPU();
    const cv::Mat crop = cv::imread("tests/fixtures/arcface_aligned.png");
    REQUIRE(!crop.empty());
    const auto expected = load_f32("tests/fixtures/arcface_expected.f32");
    REQUIRE(expected.size() == so::kEmbeddingDim);

    so::EnginePool pool("models/ArcFace_MobileFaceNet.dxnn", 1);
    so::ArcFace arc(pool);
    const auto got = arc.embed(crop);

    REQUIRE(got.size() == so::kEmbeddingDim);

    // The embedding must be unit length — every downstream cosine assumes it.
    double norm = 0.0;
    for (float x : got) norm += static_cast<double>(x) * x;
    CHECK(std::sqrt(norm) == doctest::Approx(1.0).epsilon(1e-4));

    // The device is deterministic here: two Python embeddings of this same crop
    // agreed at cosine 1.000000, so anything below 0.99 is a real difference in
    // preprocessing, not sampling noise.
    CHECK(so::cosine(got, expected) > 0.99f);
}

TEST_CASE("embed rejects a crop that is not a 112x112 BGR image") {
    SO_REQUIRE_NPU();
    so::EnginePool pool("models/ArcFace_MobileFaceNet.dxnn", 1);
    so::ArcFace arc(pool);
    cv::Mat wrong(64, 64, CV_8UC3, cv::Scalar::all(0));
    CHECK_THROWS_AS(arc.embed(wrong), std::invalid_argument);
}
