#include "vision/yolo26_pose.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>

namespace so {
namespace {

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

struct ScaleTensors {
    const TensorView* box{nullptr};
    const TensorView* cls{nullptr};
    const TensorView* kpt{nullptr};
    int grid{0};
    int stride{0};
};

// One anchor's score, kept with enough context to decode it later. The head is
// one2one, so this is a plain top-K selection rather than NMS.
struct Candidate {
    float score;
    const ScaleTensors* scale;
    int cell;   // row-major index into grid*grid
};

}  // namespace

Yolo26PoseDecoder::Yolo26PoseDecoder(int input_size, float conf, float kp_conf, int topk)
    : input_size_(input_size), conf_(conf), kp_conf_(kp_conf), topk_(topk) {}

std::vector<PoseDetection> Yolo26PoseDecoder::decode(
    const std::vector<TensorView>& outputs) const {
    if (outputs.size() != 9)
        throw std::runtime_error("yolo26 pose expects 9 output tensors, got " +
                                 std::to_string(outputs.size()));

    // Group by channel count, never by position: DX-RT's tensor order is not
    // the scale order and is not stable across models.
    std::vector<const TensorView*> boxes, clss, kpts;
    for (const auto& t : outputs) {
        if (t.ndim() != 4)
            throw std::runtime_error("yolo26 pose: expected NCHW tensors");
        switch (t.dim(1)) {
            case 4:  boxes.push_back(&t); break;
            case 1:  clss.push_back(&t);  break;
            case 51: kpts.push_back(&t);  break;
            default:
                throw std::runtime_error(
                    "yolo26 pose: unexpected channel count " + std::to_string(t.dim(1)) +
                    " (expected 4 = box, 1 = class, 51 = 17 keypoints x 3)");
        }
    }
    if (boxes.size() != 3 || clss.size() != 3 || kpts.size() != 3)
        throw std::runtime_error("yolo26 pose: expected 3 box + 3 class + 3 keypoint maps");

    // Largest grid first: 80 (stride 8), 40 (16), 20 (32).
    auto by_grid_desc = [](const TensorView* a, const TensorView* b) {
        return a->dim(2) > b->dim(2);
    };
    std::sort(boxes.begin(), boxes.end(), by_grid_desc);
    std::sort(clss.begin(), clss.end(), by_grid_desc);
    std::sort(kpts.begin(), kpts.end(), by_grid_desc);

    std::array<ScaleTensors, 3> scales{};
    for (int s = 0; s < 3; ++s) {
        const int grid = static_cast<int>(boxes[s]->dim(2));
        if (clss[s]->dim(2) != grid || kpts[s]->dim(2) != grid)
            throw std::runtime_error("yolo26 pose: box/class/keypoint grids disagree");
        scales[s] = ScaleTensors{boxes[s], clss[s], kpts[s], grid, input_size_ / grid};
    }

    // ── select ──────────────────────────────────────────────────────────────
    std::vector<Candidate> cands;
    for (const auto& sc : scales) {
        const int cells = sc.grid * sc.grid;
        const float* cls = sc.cls->data;
        for (int i = 0; i < cells; ++i) {
            const float s = sigmoid(cls[i]);
            if (s >= conf_) cands.push_back(Candidate{s, &sc, i});
        }
    }
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (static_cast<int>(cands.size()) > topk_)
        cands.resize(static_cast<std::size_t>(topk_));

    // ── decode ──────────────────────────────────────────────────────────────
    std::vector<PoseDetection> out;
    out.reserve(cands.size());
    for (const auto& c : cands) {
        const ScaleTensors& sc = *c.scale;
        const int grid = sc.grid;
        const int cells = grid * grid;
        const float stride = static_cast<float>(sc.stride);

        const int row = c.cell / grid;
        const int col = c.cell % grid;
        const float gx = static_cast<float>(col) + 0.5f;
        const float gy = static_cast<float>(row) + 0.5f;

        // ltrb are stored plane-by-plane: plane p occupies [p*cells, (p+1)*cells)
        const float* b = sc.box->data;
        const float l = b[0 * cells + c.cell];
        const float t = b[1 * cells + c.cell];
        const float r = b[2 * cells + c.cell];
        const float d = b[3 * cells + c.cell];

        PoseDetection det;
        det.score = c.score;
        det.box = {(gx - l) * stride, (gy - t) * stride,
                   (gx + r) * stride, (gy + d) * stride};

        const float* k = sc.kpt->data;
        for (int j = 0; j < kNumKeypoints; ++j) {
            const float kx = k[(j * 3 + 0) * cells + c.cell];
            const float ky = k[(j * 3 + 1) * cells + c.cell];
            const float kc = k[(j * 3 + 2) * cells + c.cell];
            det.kps[static_cast<std::size_t>(j)] =
                Keypoint{(kx + gx) * stride, (ky + gy) * stride, sigmoid(kc)};
        }
        out.push_back(det);
    }
    return out;
}

}  // namespace so
