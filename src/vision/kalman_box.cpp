#include "vision/kalman_box.hpp"

#include <algorithm>
#include <cmath>

namespace so {
namespace {

// Process and measurement noise, in the SORT convention: position is trusted
// more than velocity, and the aspect ratio is the noisiest measurement because
// arms and carried objects change box width without the person moving.
constexpr float kStdPos = 1.0f / 20.0f;
constexpr float kStdVel = 1.0f / 160.0f;

}  // namespace

std::array<float, 4> KalmanBox::to_state(const Box& b) {
    const float w = std::max(1e-3f, b[2] - b[0]);
    const float h = std::max(1e-3f, b[3] - b[1]);
    return {b[0] + w * 0.5f, b[1] + h * 0.5f, w / h, h};
}

Box KalmanBox::from_state(const std::array<float, 4>& z) {
    const float h = std::max(1e-3f, z[3]);
    const float w = std::max(1e-3f, z[2] * h);
    return {z[0] - w * 0.5f, z[1] - h * 0.5f, z[0] + w * 0.5f, z[1] + h * 0.5f};
}

KalmanBox::KalmanBox(const Box& box) {
    const auto z = to_state(box);
    for (int i = 0; i < 4; ++i) {
        x_[static_cast<std::size_t>(i)] = z[static_cast<std::size_t>(i)];
        x_[static_cast<std::size_t>(i + 4)] = 0.0f;
    }
    // A diagonal covariance is sufficient here: the four measured quantities are
    // close enough to independent that the off-diagonal terms never earned their
    // cost in association quality.
    const float h = z[3];
    p_ = {2 * kStdPos * h, 2 * kStdPos * h, 1e-2f, 2 * kStdPos * h,
          10 * kStdVel * h, 10 * kStdVel * h, 1e-5f, 10 * kStdVel * h};
    for (auto& v : p_) v = v * v;
}

Box KalmanBox::predict() {
    // Constant velocity: position += velocity.
    for (int i = 0; i < 4; ++i)
        x_[static_cast<std::size_t>(i)] += x_[static_cast<std::size_t>(i + 4)];

    const float h = std::max(1e-3f, x_[3]);
    const std::array<float, 8> q = {
        kStdPos * h, kStdPos * h, 1e-2f, kStdPos * h,
        kStdVel * h, kStdVel * h, 1e-5f, kStdVel * h};
    for (std::size_t i = 0; i < 8; ++i) p_[i] += q[i] * q[i];

    ++age_;
    ++time_since_update_;
    return from_state({x_[0], x_[1], x_[2], x_[3]});
}

void KalmanBox::update(const Box& box) {
    const auto z = to_state(box);
    const float h = std::max(1e-3f, x_[3]);
    const std::array<float, 4> r = {kStdPos * h, kStdPos * h, 1e-1f, kStdPos * h};

    for (int i = 0; i < 4; ++i) {
        const auto ip = static_cast<std::size_t>(i);
        const float rr = r[ip] * r[ip];
        const float k = p_[ip] / (p_[ip] + rr);                 // position gain
        const float innov = z[ip] - x_[ip];
        x_[ip] += k * innov;
        p_[ip] *= (1.0f - k);

        // The velocity component is corrected by the same innovation, scaled by
        // its own gain — this is what lets a track keep moving sensibly through
        // a missed frame instead of snapping back.
        const auto iv = static_cast<std::size_t>(i + 4);
        const float kv = p_[iv] / (p_[iv] + rr);
        x_[iv] += kv * innov;
        p_[iv] *= (1.0f - kv);
    }
    time_since_update_ = 0;
}

Box KalmanBox::state_box() const {
    return from_state({x_[0], x_[1], x_[2], x_[3]});
}

}  // namespace so
