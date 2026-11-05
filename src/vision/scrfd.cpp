#include "vision/scrfd.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>

namespace so {
namespace {

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

constexpr int kStrides[3] = {8, 16, 32};
constexpr int kAnchorsPerCell = 2;

struct ScaleTensors {
    const TensorView* score{nullptr};
    const TensorView* bbox{nullptr};
    const TensorView* kps{nullptr};
    int grid{0};
    int stride{0};
};

struct Candidate {
    float score;
    const ScaleTensors* scale;
    int cell;
    int anchor;
};

// The legacy "+1" area convention, kept deliberately: the Python reference this
// decoder is asserted against uses it, and matching it exactly is what makes
// the parity test meaningful. The difference is sub-pixel on real faces.
float iou(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float x1 = std::max(a[0], b[0]);
    const float y1 = std::max(a[1], b[1]);
    const float x2 = std::min(a[2], b[2]);
    const float y2 = std::min(a[3], b[3]);
    const float inter = std::max(0.0f, x2 - x1 + 1.0f) * std::max(0.0f, y2 - y1 + 1.0f);
    if (inter <= 0.0f) return 0.0f;
    const float aa = (a[2] - a[0] + 1.0f) * (a[3] - a[1] + 1.0f);
    const float bb = (b[2] - b[0] + 1.0f) * (b[3] - b[1] + 1.0f);
    return inter / (aa + bb - inter);
}

}  // namespace

ScrfdDecoder::ScrfdDecoder(int input_size, float conf, float nms_iou)
    : input_size_(input_size), conf_(conf), nms_iou_(nms_iou) {}

std::vector<FaceDetection> ScrfdDecoder::decode(
    const std::vector<TensorView>& outputs) const {
    if (outputs.size() != 9)
        throw std::runtime_error("SCRFD expects 9 output tensors (3 strides x "
                                 "[score, bbox, landmarks]), got " +
                                 std::to_string(outputs.size()));

    std::vector<const TensorView*> scores, boxes, kpts;
    for (const auto& t : outputs) {
        if (t.ndim() != 4)
            throw std::runtime_error("SCRFD: expected NHWC tensors [1,S,S,C]");
        switch (t.dim(3)) {
            case 2:  scores.push_back(&t); break;
            case 8:  boxes.push_back(&t);  break;
            case 20: kpts.push_back(&t);   break;
            default:
                throw std::runtime_error(
                    "SCRFD: unexpected channel count " + std::to_string(t.dim(3)) +
                    " (expected 2 = score, 8 = bbox, 20 = landmarks; "
                    "did an ONNX-Runtime-shaped tensor reach the native decoder?)");
        }
    }
    if (scores.size() != 3 || boxes.size() != 3 || kpts.size() != 3)
        throw std::runtime_error("SCRFD: expected 3 score + 3 bbox + 3 landmark maps");

    auto by_grid_desc = [](const TensorView* a, const TensorView* b) {
        return a->dim(1) > b->dim(1);
    };
    std::sort(scores.begin(), scores.end(), by_grid_desc);
    std::sort(boxes.begin(), boxes.end(), by_grid_desc);
    std::sort(kpts.begin(), kpts.end(), by_grid_desc);

    std::array<ScaleTensors, 3> scales{};
    for (int s = 0; s < 3; ++s) {
        const int grid = static_cast<int>(scores[s]->dim(1));
        if (boxes[s]->dim(1) != grid || kpts[s]->dim(1) != grid)
            throw std::runtime_error("SCRFD: score/bbox/landmark grids disagree");
        const int stride = input_size_ / grid;
        if (stride != kStrides[s])
            throw std::runtime_error("SCRFD: unexpected stride " + std::to_string(stride));
        scales[static_cast<std::size_t>(s)] =
            ScaleTensors{scores[s], boxes[s], kpts[s], grid, stride};
    }

    // ── select ──────────────────────────────────────────────────────────────
    // The score is a raw logit on the native path and already activated on the
    // ORT path, so decide from the data rather than from which path we think
    // produced it.
    std::vector<Candidate> cands;
    for (const auto& sc : scales) {
        const int cells = sc.grid * sc.grid;
        const float* s = sc.score->data;
        const std::size_t n = static_cast<std::size_t>(cells) * kAnchorsPerCell;
        const auto mm = std::minmax_element(s, s + n);
        const bool needs_sigmoid = (*mm.first < 0.0f) || (*mm.second > 1.0f);

        for (int c = 0; c < cells; ++c) {
            for (int a = 0; a < kAnchorsPerCell; ++a) {
                const float raw = s[static_cast<std::size_t>(c) * kAnchorsPerCell + a];
                const float v = needs_sigmoid ? sigmoid(raw) : raw;
                if (v > conf_) cands.push_back(Candidate{v, &sc, c, a});
            }
        }
    }
    if (cands.empty()) return {};

    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    // ── decode ──────────────────────────────────────────────────────────────
    std::vector<FaceDetection> dets;
    dets.reserve(cands.size());
    for (const auto& c : cands) {
        const ScaleTensors& sc = *c.scale;
        const float stride = static_cast<float>(sc.stride);
        const int row = c.cell / sc.grid;
        const int col = c.cell % sc.grid;

        // Anchor at the cell ORIGIN — see the header for the measurement that
        // settled this against the cell-centre convention.
        const float cx = (static_cast<float>(col) + ScrfdDecoder::kCellOffset) * stride;
        const float cy = (static_cast<float>(row) + ScrfdDecoder::kCellOffset) * stride;

        const float* b = sc.bbox->data +
                         (static_cast<std::size_t>(c.cell) * kAnchorsPerCell + c.anchor) * 4;
        const float* k = sc.kps->data +
                         (static_cast<std::size_t>(c.cell) * kAnchorsPerCell + c.anchor) * 10;

        FaceDetection d;
        d.score = c.score;
        d.box = {cx - b[0] * stride, cy - b[1] * stride,
                 cx + b[2] * stride, cy + b[3] * stride};
        for (int i = 0; i < kNumLandmarks; ++i)
            d.lm[static_cast<std::size_t>(i)] =
                Landmark{cx + k[i * 2] * stride, cy + k[i * 2 + 1] * stride};
        dets.push_back(d);
    }

    // ── NMS (SCRFD is not NMS-free) ─────────────────────────────────────────
    std::vector<FaceDetection> keep;
    std::vector<bool> dead(dets.size(), false);
    for (std::size_t i = 0; i < dets.size(); ++i) {
        if (dead[i]) continue;
        keep.push_back(dets[i]);
        for (std::size_t j = i + 1; j < dets.size(); ++j)
            if (!dead[j] && iou(dets[i].box, dets[j].box) > nms_iou_) dead[j] = true;
    }
    return keep;
}

}  // namespace so
