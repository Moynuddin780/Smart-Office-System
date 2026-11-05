#pragma once

#include "vision/geometry.hpp"

#include <array>

namespace so {

/**
 * Constant-velocity Kalman filter over a box, in the standard SORT
 * parameterisation: state is (cx, cy, aspect, height, vcx, vcy, v_aspect,
 * v_height).
 *
 * Height rather than area is tracked because a person's box height is the
 * stable dimension — arms and bags change the width frame to frame while the
 * height only changes with distance.
 *
 * Deliberately hand-rolled rather than pulled from a matrix library: the state
 * is 8-dimensional and the matrices are sparse and constant, so the update is a
 * few dozen multiplies. Adding Eigen for this would be a dependency for no
 * measurable gain.
 */
class KalmanBox {
public:
    explicit KalmanBox(const Box& box);

    /// Advance one frame and return the predicted box.
    Box predict();

    /// Fold a measurement in.
    void update(const Box& box);

    /// Current estimate, without advancing.
    Box state_box() const;

    int time_since_update() const { return time_since_update_; }
    int age() const { return age_; }

private:
    static std::array<float, 4> to_state(const Box& b);
    static Box from_state(const std::array<float, 4>& z);

    std::array<float, 8> x_{};      ///< state
    std::array<float, 8> p_{};      ///< diagonal covariance — see the .cpp
    int time_since_update_{0};
    int age_{0};
};

}  // namespace so
