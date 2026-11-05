#pragma once

#include <chrono>
#include <cstdint>

#include <opencv2/core.hpp>

namespace so {

/**
 * The letterbox transform between a camera frame and the model's square input.
 *
 * Every model here takes a 640x640 (or 112x112) canvas, while cameras deliver
 * 16:9 or 4:3. Resizing without preserving aspect would distort a person into a
 * shape the model was never trained on, so the frame is scaled to fit and
 * padded. Detections come back in canvas coordinates and have to be mapped
 * home, which is what this struct is for — keeping the forward and inverse
 * together means they cannot drift apart.
 */
struct LetterboxMap {
    float scale{1.0f};   ///< source pixels -> canvas pixels
    int dx{0};           ///< left padding, in canvas pixels
    int dy{0};           ///< top padding, in canvas pixels

    cv::Point2f to_canvas(float x, float y) const {
        return {x * scale + static_cast<float>(dx), y * scale + static_cast<float>(dy)};
    }
    cv::Point2f to_source(float x, float y) const {
        return {(x - static_cast<float>(dx)) / scale, (y - static_cast<float>(dy)) / scale};
    }
};

/// A decoded camera frame plus the identity it was captured under.
struct Frame {
    cv::Mat image;                  ///< BGR, source resolution
    std::int64_t seq{0};
    std::chrono::system_clock::time_point ts{};
};

}  // namespace so
