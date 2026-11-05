#include "vision/arcface.hpp"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <numeric>
#include <stdexcept>

namespace so {

const std::array<cv::Point2f, kNumLandmarks> kArcFaceRef = {{
    {38.2946f, 51.6963f},   // left eye
    {73.5318f, 51.5014f},   // right eye
    {56.0252f, 71.7366f},   // nose
    {41.5493f, 92.3655f},   // left mouth corner
    {70.7299f, 92.2041f},   // right mouth corner
}};

cv::Mat umeyama(const std::vector<cv::Point2f>& src, const std::vector<cv::Point2f>& dst) {
    if (src.size() != dst.size() || src.empty())
        throw std::invalid_argument("umeyama: point sets must be non-empty and equal in size");

    const int n = static_cast<int>(src.size());
    cv::Point2d src_mean(0, 0), dst_mean(0, 0);
    for (int i = 0; i < n; ++i) {
        src_mean += cv::Point2d(src[static_cast<std::size_t>(i)]);
        dst_mean += cv::Point2d(dst[static_cast<std::size_t>(i)]);
    }
    src_mean *= 1.0 / n;
    dst_mean *= 1.0 / n;

    cv::Mat src_c(n, 2, CV_64F), dst_c(n, 2, CV_64F);
    for (int i = 0; i < n; ++i) {
        src_c.at<double>(i, 0) = src[static_cast<std::size_t>(i)].x - src_mean.x;
        src_c.at<double>(i, 1) = src[static_cast<std::size_t>(i)].y - src_mean.y;
        dst_c.at<double>(i, 0) = dst[static_cast<std::size_t>(i)].x - dst_mean.x;
        dst_c.at<double>(i, 1) = dst[static_cast<std::size_t>(i)].y - dst_mean.y;
    }

    const cv::Mat cov = (dst_c.t() * src_c) / static_cast<double>(n);

    cv::Mat w, u, vt;
    cv::SVD::compute(cov, w, u, vt, cv::SVD::FULL_UV);

    // Guard against a reflection: without this a mirrored landmark set produces
    // a flipped "alignment" that still looks like a face and embeds to nonsense.
    cv::Mat d = cv::Mat::eye(2, 2, CV_64F);
    if (cv::determinant(u) * cv::determinant(vt) < 0) d.at<double>(1, 1) = -1.0;

    const cv::Mat R = u * d * vt;

    double var_src = 0.0;
    for (int i = 0; i < n; ++i)
        var_src += src_c.at<double>(i, 0) * src_c.at<double>(i, 0) +
                   src_c.at<double>(i, 1) * src_c.at<double>(i, 1);
    var_src /= n;

    double scale = 1.0;
    if (var_src >= 1e-12) {
        double trace = 0.0;
        for (int i = 0; i < 2; ++i) trace += w.at<double>(i) * d.at<double>(i, i);
        scale = trace / var_src;
    }

    cv::Mat M = cv::Mat::zeros(2, 3, CV_64F);
    const cv::Mat sR = scale * R;
    sR.copyTo(M(cv::Rect(0, 0, 2, 2)));
    M.at<double>(0, 2) = dst_mean.x - (sR.at<double>(0, 0) * src_mean.x +
                                       sR.at<double>(0, 1) * src_mean.y);
    M.at<double>(1, 2) = dst_mean.y - (sR.at<double>(1, 0) * src_mean.x +
                                       sR.at<double>(1, 1) * src_mean.y);
    return M;
}

cv::Mat align_face(const cv::Mat& image, const std::array<Landmark, kNumLandmarks>& lm,
                   int size) {
    std::vector<cv::Point2f> src;
    src.reserve(kNumLandmarks);
    for (const auto& p : lm) src.emplace_back(p.x, p.y);

    const std::vector<cv::Point2f> dst(kArcFaceRef.begin(), kArcFaceRef.end());
    const cv::Mat M = umeyama(src, dst);

    cv::Mat out;
    cv::warpAffine(image, out, M, cv::Size(size, size), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return out;
}

void l2_normalize(std::vector<float>& v) {
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    const double n = std::sqrt(sum);
    if (n <= 1e-8) return;
    for (float& x : v) x = static_cast<float>(x / n);
}

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size())
        throw std::invalid_argument("cosine: embeddings of different length (" +
                                    std::to_string(a.size()) + " vs " +
                                    std::to_string(b.size()) + ")");
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) s += static_cast<double>(a[i]) * b[i];
    return static_cast<float>(s);
}

ArcFace::ArcFace(EnginePool& pool) : pool_(pool) {
    output_.resize(static_cast<std::size_t>(pool_.output_size()));
}

std::vector<float> ArcFace::embed(const cv::Mat& aligned_bgr) {
    if (aligned_bgr.rows != kAlignedSize || aligned_bgr.cols != kAlignedSize ||
        aligned_bgr.type() != CV_8UC3)
        throw std::invalid_argument("ArcFace::embed expects a 112x112 CV_8UC3 crop");

    // The model's input tensor is RGB uint8 NHWC; normalisation is folded into
    // the compiled model, exactly as for the detectors.
    cv::Mat rgb;
    cv::cvtColor(aligned_bgr, rgb, cv::COLOR_BGR2RGB);
    if (!rgb.isContinuous()) rgb = rgb.clone();

    const dxrt::TensorPtrs out = pool_.run_sync(rgb.data, output_.data());
    if (out.empty()) throw std::runtime_error("ArcFace: model returned no output tensor");

    const float* p = static_cast<const float*>(out.front()->data());
    const std::size_t n = out.front()->size_in_bytes() / sizeof(float);
    if (n != kEmbeddingDim)
        throw std::runtime_error("ArcFace: expected a " + std::to_string(kEmbeddingDim) +
                                 "-D embedding, got " + std::to_string(n));

    std::vector<float> v(p, p + n);
    l2_normalize(v);
    return v;
}

}  // namespace so
