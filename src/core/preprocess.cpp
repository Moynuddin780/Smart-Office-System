#include "core/preprocess.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace so {

cv::Mat letterbox(const cv::Mat& src, int size, LetterboxMap& map) {
    if (src.empty()) throw std::invalid_argument("letterbox: empty source frame");
    if (size <= 0) throw std::invalid_argument("letterbox: size must be positive");

    const int w = src.cols;
    const int h = src.rows;
    map.scale = std::min(static_cast<float>(size) / static_cast<float>(w),
                         static_cast<float>(size) / static_cast<float>(h));

    const int nw = static_cast<int>(std::lround(w * map.scale));
    const int nh = static_cast<int>(std::lround(h * map.scale));
    map.dx = (size - nw) / 2;
    map.dy = (size - nh) / 2;

    cv::Mat canvas(size, size, CV_8UC3, cv::Scalar(114, 114, 114));
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
    resized.copyTo(canvas(cv::Rect(map.dx, map.dy, nw, nh)));

    // The model's input tensor is RGB; frames from OpenCV are BGR.
    cv::Mat rgb;
    cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);
    return rgb;
}

void box_to_source(const LetterboxMap& map, int src_w, int src_h, float* xyxy) {
    const cv::Point2f a = map.to_source(xyxy[0], xyxy[1]);
    const cv::Point2f b = map.to_source(xyxy[2], xyxy[3]);
    xyxy[0] = std::clamp(a.x, 0.0f, static_cast<float>(src_w));
    xyxy[1] = std::clamp(a.y, 0.0f, static_cast<float>(src_h));
    xyxy[2] = std::clamp(b.x, 0.0f, static_cast<float>(src_w));
    xyxy[3] = std::clamp(b.y, 0.0f, static_cast<float>(src_h));
}

}  // namespace so
