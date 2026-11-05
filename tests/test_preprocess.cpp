#include "doctest/doctest.h"

#include "core/preprocess.hpp"

#include <opencv2/imgproc.hpp>

TEST_CASE("letterbox fits a 16:9 frame into a square canvas and pads the rest") {
    cv::Mat src(1080, 1920, CV_8UC3, cv::Scalar(10, 20, 30));
    so::LetterboxMap m;
    const cv::Mat dst = so::letterbox(src, 640, m);

    CHECK(dst.cols == 640);
    CHECK(dst.rows == 640);
    CHECK(dst.type() == CV_8UC3);
    CHECK(m.scale == doctest::Approx(640.0f / 1920.0f));
    CHECK(m.dx == 0);
    CHECK(m.dy == 140);           // (640 - 360) / 2

    // The padding is 114 on every channel — the value the YOLO family trains
    // with. A different pad shifts the background statistics the model expects.
    const cv::Vec3b top = dst.at<cv::Vec3b>(5, 320);
    CHECK(static_cast<int>(top[0]) == 114);
    CHECK(static_cast<int>(top[1]) == 114);
    CHECK(static_cast<int>(top[2]) == 114);
}

TEST_CASE("the canvas is RGB, not BGR") {
    // A pure-blue BGR source must come out with blue in the LAST channel.
    cv::Mat src(100, 100, CV_8UC3, cv::Scalar(255, 0, 0));
    so::LetterboxMap m;
    const cv::Mat dst = so::letterbox(src, 64, m);
    const cv::Vec3b px = dst.at<cv::Vec3b>(32, 32);
    CHECK(static_cast<int>(px[0]) == 0);
    CHECK(static_cast<int>(px[2]) == 255);
}

TEST_CASE("the forward and inverse maps agree") {
    cv::Mat src(720, 1280, CV_8UC3, cv::Scalar::all(0));
    so::LetterboxMap m;
    so::letterbox(src, 640, m);

    const cv::Point2f c = m.to_canvas(640.f, 360.f);
    CHECK(c.x == doctest::Approx(320.f));
    CHECK(c.y == doctest::Approx(320.f));

    const cv::Point2f s = m.to_source(c.x, c.y);
    CHECK(s.x == doctest::Approx(640.f).epsilon(0.001));
    CHECK(s.y == doctest::Approx(360.f).epsilon(0.001));
}

TEST_CASE("a square source needs no padding at all") {
    cv::Mat src(500, 500, CV_8UC3, cv::Scalar::all(7));
    so::LetterboxMap m;
    so::letterbox(src, 640, m);
    CHECK(m.dx == 0);
    CHECK(m.dy == 0);
    CHECK(m.scale == doctest::Approx(640.0f / 500.0f));
}

TEST_CASE("boxes map home and are clamped to the frame") {
    cv::Mat src(1080, 1920, CV_8UC3, cv::Scalar::all(0));
    so::LetterboxMap m;
    so::letterbox(src, 640, m);

    float box[4] = {0.f, 0.f, 640.f, 640.f};   // the whole canvas
    so::box_to_source(m, 1920, 1080, box);
    CHECK(box[0] == doctest::Approx(0.f));
    CHECK(box[1] == doctest::Approx(0.f));     // clamped up from -420
    CHECK(box[2] == doctest::Approx(1920.f));
    CHECK(box[3] == doctest::Approx(1080.f));  // clamped down from 1500
}

TEST_CASE("letterbox rejects an empty frame instead of producing garbage") {
    cv::Mat empty;
    so::LetterboxMap m;
    CHECK_THROWS_AS(so::letterbox(empty, 640, m), std::invalid_argument);
}
