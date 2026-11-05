#pragma once

#include "vision/tensor_view.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace so {

/// SCRFD's five landmarks, in the order the model emits them.
enum FaceLm : int {
    LM_LEFT_EYE = 0,
    LM_RIGHT_EYE = 1,
    LM_NOSE = 2,
    LM_LEFT_MOUTH = 3,
    LM_RIGHT_MOUTH = 4,
};
constexpr int kNumLandmarks = 5;

struct Landmark {
    float x{0}, y{0};
};

struct FaceDetection {
    std::array<float, 4> box{};                            ///< x1,y1,x2,y2, CANVAS space
    float score{0};
    std::array<Landmark, kNumLandmarks> lm{};              ///< CANVAS space
};

/**
 * Decoder for `scrfd_10g.dxnn` running natively on the NPU (use_ort=false).
 *
 * THE LAYOUT, MEASURED ON THIS BOARD
 * ----------------------------------
 * Nine tensors, three strides (8/16/32), **two anchors per cell**, and — unlike
 * the pose model, which is NCHW — these are **NHWC**:
 *
 *     [1,S,S, 2]   score, RAW LOGIT on the native path
 *     [1,S,S, 8]   bbox distances, 2 anchors x (l, t, r, b), in stride units
 *     [1,S,S,20]   landmarks, 2 anchors x 5 x (dx, dy), in stride units
 *
 * The same model through ONNX Runtime instead returns (1,A,1) / (1,A,4) /
 * (1,A,10) with A = S*S*2 and the score ALREADY sigmoid'd. Mixing those two up
 * is silent: the numbers are all plausible. Grouping is therefore by channel
 * count, and the score is activated only when its range says it needs to be.
 *
 * Tensor ORDER is not usable either. Probed natively, this model returns
 * bbox80, kps80, score80, score40, bbox40, kps40, bbox20, kps20, score20 —
 * so the common `outputs[i] / outputs[i+3] / outputs[i+6]` indexing (correct
 * for ORT's canonical order) reads the wrong tensor here.
 *
 * ANCHOR PLACEMENT — SETTLED BY MEASUREMENT, NOT CITATION
 * -------------------------------------------------------
 * The anchor sits at the grid cell's ORIGIN, not its centre. Measured on
 * 2026-09-24 over 96 paired samples from the whole clip corpus, comparing
 * SCRFD's own nose LANDMARK against the pose model's nose KEYPOINT on the same
 * frame and normalising by the subject's eye separation:
 *
 *     offset 0.0    median error 0.2364 eye-widths
 *     offset 0.5    median error 0.9737 eye-widths      (4.1x worse)
 *
 * Half a stride is 16 px at stride 32. At the wrong offset the landmarks are
 * about one eye-width out, which corrupts the Umeyama alignment and therefore
 * every ArcFace embedding computed from it — it would not fail loudly, it would
 * just quietly recognise people less well.
 *
 * SCRFD is NOT NMS-free (unlike the YOLO26 head), so NMS runs here.
 */
class ScrfdDecoder {
public:
    static constexpr float kCellOffset = 0.0f;

    ScrfdDecoder(int input_size = 640, float conf = 0.5f, float nms_iou = 0.4f);

    std::vector<FaceDetection> decode(const std::vector<TensorView>& outputs) const;

    float conf() const { return conf_; }
    void set_conf(float c) { conf_ = c; }

private:
    int input_size_;
    float conf_;
    float nms_iou_;
};

}  // namespace so
