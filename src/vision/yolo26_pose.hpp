#pragma once

#include "vision/tensor_view.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace so {

// COCO-17 keypoint order, as this model emits it.
enum Kp : int {
    NOSE = 0,
    LEFT_EYE = 1, RIGHT_EYE = 2,
    LEFT_EAR = 3, RIGHT_EAR = 4,
    LEFT_SHOULDER = 5, RIGHT_SHOULDER = 6,
    LEFT_ELBOW = 7, RIGHT_ELBOW = 8,
    LEFT_WRIST = 9, RIGHT_WRIST = 10,
    LEFT_HIP = 11, RIGHT_HIP = 12,
    LEFT_KNEE = 13, RIGHT_KNEE = 14,
    LEFT_ANKLE = 15, RIGHT_ANKLE = 16,
};
constexpr int kNumKeypoints = 17;

struct Keypoint {
    float x{0}, y{0}, conf{0};
};

struct PoseDetection {
    std::array<float, 4> box{};                        ///< x1,y1,x2,y2 in CANVAS space
    float score{0};
    std::array<Keypoint, kNumKeypoints> kps{};         ///< canvas space
};

/**
 * Decoder for `yolo26s_pose.dxnn` running natively on the NPU (use_ort=false).
 *
 * WHY WE DECODE THIS OURSELVES
 * ----------------------------
 * dx_app ships a `YOLOv26PostProcess`, but it does not fit this model: it
 * selects box tensors on `shape()[1] == 64` (a DFL head) and assumes 80
 * classes. This export is direct 4-channel box regression with a 1-channel
 * class head, so that decoder finds nothing and returns no detections.
 *
 * THE LAYOUT, MEASURED ON THIS BOARD (2026-09-23)
 * -----------------------------------------------
 * Nine tensors, at three scales S in {80, 40, 20}, stride = 640 / S:
 *     [1, 4,S,S]   ltrb box distances from the cell centre, in STRIDE units
 *     [1, 1,S,S]   class logit (1 class: person)
 *     [1,51,S,S]   17 keypoints x (x, y, conf)
 *
 * DX-RT returned them in the order box80, kpt80, kpt40, box40, cls80, cls40,
 * kpt20, box20, cls20 — i.e. neither scale order nor type order. They are
 * therefore grouped by CHANNEL COUNT and sorted by grid size, never by
 * position in the list.
 *
 * THE DECODE, VERIFIED TO 0.000 px AGAINST THE ORT PATH
 * ------------------------------------------------------
 * With gx = col + 0.5, gy = row + 0.5 and a row-major flatten:
 *     x1 = (gx - l) * stride        score   = sigmoid(cls)
 *     y1 = (gy - t) * stride        kp_x    = (raw_kx + gx) * stride
 *     x2 = (gx + r) * stride        kp_y    = (raw_ky + gy) * stride
 *     y2 = (gy + b) * stride        kp_conf = sigmoid(raw_kconf)
 *
 * Three other plausible keypoint formulas were measured and rejected on the
 * same frame: `2r+(g-0.5)` was off by 39.756 px mean, `(r+g-0.5)` by 22.627,
 * `2r+g` by 29.870. The one above was off by 0.000. tests/test_yolo26_pose.cpp
 * keeps that record so the question is not reopened by guesswork.
 *
 * NO NMS. The head is `one2one` — the model emits one prediction per object.
 * Running NMS here would be wrong, not merely redundant.
 */
class Yolo26PoseDecoder {
public:
    static constexpr const char* kKeypointFormula = "(raw + grid_centre) * stride";

    Yolo26PoseDecoder(int input_size = 640, float conf = 0.5f, float kp_conf = 0.35f,
                      int topk = 300);

    std::vector<PoseDetection> decode(const std::vector<TensorView>& outputs) const;

    float conf() const { return conf_; }
    void set_conf(float c) { conf_ = c; }

private:
    int input_size_;
    float conf_;
    float kp_conf_;
    int topk_;
};

}  // namespace so
