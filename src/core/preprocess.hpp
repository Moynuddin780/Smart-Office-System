#pragma once

#include "core/frame.hpp"

#include <opencv2/core.hpp>

namespace so {

/**
 * Scale-preserving resize onto a square canvas, padded with 114 (the value the
 * YOLO family trains with).
 *
 * The result is the model's input tensor as-is: uint8, NHWC, **RGB**. There is
 * deliberately no float conversion or mean/std normalisation here — that is
 * folded into the compiled .dxnn, and keeping it there is a large part of why
 * this deployment's CPU stays free for decoding.
 *
 * @param src  BGR source frame
 * @param size canvas edge (640 for the detector and SCRFD, 112 for ArcFace)
 * @param map  filled in with the transform, so detections can be mapped back
 */
cv::Mat letterbox(const cv::Mat& src, int size, LetterboxMap& map);

/// Box-to-source helper: maps a canvas-space x1,y1,x2,y2 back to the frame and
/// clamps it to the frame's bounds.
void box_to_source(const LetterboxMap& map, int src_w, int src_h, float* xyxy);

}  // namespace so
