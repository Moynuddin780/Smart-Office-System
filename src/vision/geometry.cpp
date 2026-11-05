#include "vision/geometry.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace so {
namespace {

// Orientation of the triplet (a, b, c): >0 counter-clockwise, <0 clockwise.
float cross_z(const cv::Point2f& a, const cv::Point2f& b, const cv::Point2f& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool on_segment(const cv::Point2f& a, const cv::Point2f& b, const cv::Point2f& p) {
    return std::min(a.x, b.x) - 1e-6f <= p.x && p.x <= std::max(a.x, b.x) + 1e-6f &&
           std::min(a.y, b.y) - 1e-6f <= p.y && p.y <= std::max(a.y, b.y) + 1e-6f;
}

bool segments_intersect(const cv::Point2f& p1, const cv::Point2f& p2,
                        const cv::Point2f& q1, const cv::Point2f& q2) {
    const float d1 = cross_z(q1, q2, p1);
    const float d2 = cross_z(q1, q2, p2);
    const float d3 = cross_z(p1, p2, q1);
    const float d4 = cross_z(p1, p2, q2);

    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;

    // Collinear touching cases.
    if (std::abs(d1) < 1e-9f && on_segment(q1, q2, p1)) return true;
    if (std::abs(d2) < 1e-9f && on_segment(q1, q2, p2)) return true;
    if (std::abs(d3) < 1e-9f && on_segment(p1, p2, q1)) return true;
    if (std::abs(d4) < 1e-9f && on_segment(p1, p2, q2)) return true;
    return false;
}

}  // namespace

cv::Point2f foot_point(const Box& box) {
    return {(box[0] + box[2]) * 0.5f, box[3]};
}

cv::Point2f box_centre(const Box& box) {
    return {(box[0] + box[2]) * 0.5f, (box[1] + box[3]) * 0.5f};
}

float box_area(const Box& box) {
    return std::max(0.0f, box[2] - box[0]) * std::max(0.0f, box[3] - box[1]);
}

float box_iou(const Box& a, const Box& b) {
    const float x1 = std::max(a[0], b[0]);
    const float y1 = std::max(a[1], b[1]);
    const float x2 = std::min(a[2], b[2]);
    const float y2 = std::min(a[3], b[3]);
    const float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    if (inter <= 0.0f) return 0.0f;
    const float uni = box_area(a) + box_area(b) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

bool box_contains(const Box& outer, const Box& inner, float tolerance) {
    const float ia = box_area(inner);
    if (ia <= 0.0f) return false;
    const float x1 = std::max(outer[0], inner[0]);
    const float y1 = std::max(outer[1], inner[1]);
    const float x2 = std::min(outer[2], inner[2]);
    const float y2 = std::min(outer[3], inner[3]);
    const float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    return (inter / ia) >= (1.0f - tolerance);
}

bool point_in_polygon(const cv::Point2f& p, const Polygon& poly) {
    if (poly.size() < 3) return false;
    // cv::pointPolygonTest returns 0 exactly on an edge; treat that as inside so
    // a person standing precisely on a zone boundary is counted rather than
    // flickering in and out as the box jitters by a pixel.
    return cv::pointPolygonTest(poly, p, false) >= 0;
}

float GateLine::side(const cv::Point2f& p) const {
    return cross_z(a, b, p);
}

float GateLine::length() const {
    return std::hypot(b.x - a.x, b.y - a.y);
}

Cross crossing(const GateLine& line, const cv::Point2f& p0, const cv::Point2f& p1) {
    if (line.length() < 1e-6f) return Cross::None;   // a degenerate line

    const float s0 = line.side(p0);
    const float s1 = line.side(p1);
    if (s0 == 0.0f && s1 == 0.0f) return Cross::None;   // moving along the line
    if ((s0 <= 0.0f && s1 <= 0.0f) || (s0 >= 0.0f && s1 >= 0.0f)) return Cross::None;

    // A sign change alone is not enough: it also holds for someone walking past
    // the line's infinite extension, metres away from the doorway.
    if (!segments_intersect(p0, p1, line.a, line.b)) return Cross::None;

    return s0 < 0.0f ? Cross::AtoB : Cross::BtoA;
}

double box_polygon_overlap(const Box& box, const Polygon& poly) {
    if (poly.size() < 3) return 0.0;
    const float w = box[2] - box[0], h = box[3] - box[1];
    if (w <= 0 || h <= 0) return 0.0;

    // Sampled rather than computed exactly. A true polygon-rectangle
    // intersection is more code and more ways to be subtly wrong, and the
    // answer feeds a threshold, not a measurement: whether a person covers 31%
    // or 34% of a desk changes nothing. A 12x12 grid resolves about 1%, which
    // is far finer than the decision needs.
    constexpr int kGrid = 12;
    int inside = 0;
    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            const cv::Point2f p(box[0] + w * (gx + 0.5f) / kGrid,
                                box[1] + h * (gy + 0.5f) / kGrid);
            if (point_in_polygon(p, poly)) ++inside;
        }
    }
    return static_cast<double>(inside) / (kGrid * kGrid);
}

}  // namespace so
