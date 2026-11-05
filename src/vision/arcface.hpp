#pragma once

#include "core/engine_pool.hpp"
#include "vision/scrfd.hpp"

#include <array>
#include <opencv2/core.hpp>
#include <vector>

namespace so {

constexpr int kEmbeddingDim = 512;
constexpr int kAlignedSize = 112;

/// The 5-point template ArcFace was trained against, in its 112x112 frame.
extern const std::array<cv::Point2f, kNumLandmarks> kArcFaceRef;

/**
 * Least-squares similarity transform (rotation + uniform scale + translation).
 *
 * Umeyama 1991 — the closed form behind skimage's SimilarityTransform, and what
 * the ArcFace reference alignment uses. It is deterministic and uses all five
 * landmarks; `cv::estimateAffinePartial2D` would instead run RANSAC over
 * 2-point minimal subsets, giving a slightly different matrix run to run, which
 * on an embedding model means a slightly different vector for the same face.
 *
 * @return 2x3 CV_64F, ready for cv::warpAffine
 */
cv::Mat umeyama(const std::vector<cv::Point2f>& src, const std::vector<cv::Point2f>& dst);

/// Warp a face onto the ArcFace template. `lm` are in `image`'s coordinates.
cv::Mat align_face(const cv::Mat& image, const std::array<Landmark, kNumLandmarks>& lm,
                   int size = kAlignedSize);

/// Cosine similarity. Both vectors are assumed L2-normalised (embed() returns
/// them that way), so this is a plain dot product.
float cosine(const std::vector<float>& a, const std::vector<float>& b);

/// L2-normalise in place; a zero vector is left alone rather than producing NaNs.
void l2_normalize(std::vector<float>& v);

/**
 * ArcFace embedder over a shared per-core engine pool.
 *
 * Takes an ALIGNED 112x112 BGR crop — alignment is a separate step so that the
 * quality gates (size, yaw) can reject a face before any NPU work is spent on
 * it.
 */
class ArcFace {
public:
    explicit ArcFace(EnginePool& pool);

    /// 512-D, L2-normalised. Throws if the crop is not 112x112 BGR.
    std::vector<float> embed(const cv::Mat& aligned_bgr);

private:
    EnginePool& pool_;
    std::vector<std::uint8_t> output_;
};

}  // namespace so
