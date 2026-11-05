#pragma once

#include <array>
#include <opencv2/core.hpp>
#include <vector>

namespace so {

using Box = std::array<float, 4>;   ///< x1, y1, x2, y2
using Polygon = std::vector<cv::Point2f>;

/**
 * The ground-contact point of a detection: bottom-centre of the box.
 *
 * Zone membership is decided on this rather than on how much of the box
 * overlaps the polygon. Overlap counts somebody merely leaning into frame near
 * a desk as being AT that desk, and counts a tall person walking past a
 * restricted area as being inside it. Where a person's feet are is where the
 * person is.
 */
cv::Point2f foot_point(const Box& box);

/// Centre of a box — used for track paths, where the feet are not the point.
cv::Point2f box_centre(const Box& box);

float box_area(const Box& box);
float box_iou(const Box& a, const Box& b);

/// True if `b` is (almost) entirely inside `a` — how a face is bound to the
/// person whose body contains it.
bool box_contains(const Box& outer, const Box& inner, float tolerance = 0.15f);

/// Winding-number test. A point exactly on an edge or vertex counts as inside.
bool point_in_polygon(const cv::Point2f& p, const Polygon& poly);

/// What fraction of `box` lies inside `poly`, from 0 to 1.
///
/// For a DESK the question is "is this seat taken", and a point test cannot
/// answer it: the foot point of somebody sitting down is under the desk or
/// behind the chair, outside the shape drawn around their working area. Asking
/// how much of the person overlaps the desk is the question that matches.
double box_polygon_overlap(const Box& box, const Polygon& poly);

/// Which way a track crossed a gate line.
enum class Cross { None, AtoB, BtoA };

/**
 * A directed line across a doorway. `a` -> `b` defines the line; the sign of
 * `side()` says which half-plane a point is in, and crossing from negative to
 * positive is `AtoB`.
 */
struct GateLine {
    cv::Point2f a{};
    cv::Point2f b{};

    /// Signed 2-D cross product: negative on one side, positive on the other.
    float side(const cv::Point2f& p) const;

    /// Length of the segment; a degenerate line can never be crossed.
    float length() const;
};

/**
 * Did the step from `p0` to `p1` cross the line?
 *
 * Requires BOTH a sign change and an actual segment-segment intersection.
 * Testing only the sign would fire for somebody walking past the line's
 * infinite extension — well outside the doorway — which is the difference
 * between counting an entry and counting anyone who walks along the wall.
 */
Cross crossing(const GateLine& line, const cv::Point2f& p0, const cv::Point2f& p1);

}  // namespace so
