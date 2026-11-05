#include "doctest/doctest.h"

#include "vision/geometry.hpp"

using so::Box;
using so::Cross;
using so::GateLine;
using so::Polygon;

TEST_CASE("the foot point is the bottom centre of the box") {
    const Box b{100.f, 200.f, 140.f, 300.f};
    const auto f = so::foot_point(b);
    CHECK(f.x == doctest::Approx(120.f));
    CHECK(f.y == doctest::Approx(300.f));
}

TEST_CASE("box helpers") {
    const Box a{0.f, 0.f, 10.f, 10.f};
    const Box b{5.f, 5.f, 15.f, 15.f};
    CHECK(so::box_area(a) == doctest::Approx(100.f));
    CHECK(so::box_iou(a, a) == doctest::Approx(1.f));
    CHECK(so::box_iou(a, b) == doctest::Approx(25.f / 175.f));
    CHECK(so::box_iou(a, Box{20.f, 20.f, 30.f, 30.f}) == doctest::Approx(0.f));

    // A degenerate box has no area and cannot overlap anything.
    CHECK(so::box_area(Box{5.f, 5.f, 5.f, 5.f}) == doctest::Approx(0.f));
    CHECK(so::box_iou(a, Box{5.f, 5.f, 5.f, 5.f}) == doctest::Approx(0.f));
}

TEST_CASE("a face is bound to the person box that contains it") {
    const Box person{100.f, 100.f, 200.f, 400.f};
    const Box face{130.f, 110.f, 170.f, 150.f};
    CHECK(so::box_contains(person, face));

    const Box other_face{300.f, 110.f, 340.f, 150.f};
    CHECK_FALSE(so::box_contains(person, other_face));

    // A face straddling the edge is still bound, within tolerance — a person
    // box often clips the top of the head.
    const Box edge_face{180.f, 100.f, 210.f, 130.f};
    CHECK(so::box_contains(person, edge_face, 0.4f));
    CHECK_FALSE(so::box_contains(person, edge_face, 0.05f));
}

TEST_CASE("point in polygon, including the boundary") {
    const Polygon square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    CHECK(so::point_in_polygon({5, 5}, square));
    CHECK_FALSE(so::point_in_polygon({15, 5}, square));
    CHECK_FALSE(so::point_in_polygon({5, -1}, square));

    // On an edge and on a vertex both count as inside, so a person standing on
    // a zone boundary does not flicker as the box jitters.
    CHECK(so::point_in_polygon({0, 5}, square));
    CHECK(so::point_in_polygon({0, 0}, square));
}

TEST_CASE("a concave polygon does not swallow its own notch") {
    // An L shape: the notch in the top-right must read as OUTSIDE.
    const Polygon l{{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}};
    CHECK(so::point_in_polygon({2, 2}, l));
    CHECK(so::point_in_polygon({8, 2}, l));
    CHECK(so::point_in_polygon({2, 8}, l));
    CHECK_FALSE(so::point_in_polygon({8, 8}, l));
}

TEST_CASE("a degenerate polygon is never entered") {
    CHECK_FALSE(so::point_in_polygon({0, 0}, Polygon{}));
    CHECK_FALSE(so::point_in_polygon({0, 0}, Polygon{{0, 0}, {1, 1}}));
}

TEST_CASE("the gate line separates the two sides") {
    const GateLine g{{0, 5}, {10, 5}};   // horizontal, at y = 5
    CHECK(g.side({5, 1}) < 0);
    CHECK(g.side({5, 9}) > 0);
    CHECK(g.side({5, 5}) == doctest::Approx(0.f));
}

TEST_CASE("crossing reports a direction, and only for a real crossing") {
    const GateLine g{{0, 5}, {10, 5}};

    CHECK(so::crossing(g, {5, 1}, {5, 9}) == Cross::AtoB);
    CHECK(so::crossing(g, {5, 9}, {5, 1}) == Cross::BtoA);

    // Same side throughout — approaching the door and turning back.
    CHECK(so::crossing(g, {5, 1}, {5, 2}) == Cross::None);
    CHECK(so::crossing(g, {5, 9}, {5, 8}) == Cross::None);

    // Past the line's infinite extension, nowhere near the doorway. A pure
    // sign test would call this an entry; this is the case that separates
    // counting entries from counting everyone who walks along the wall.
    CHECK(so::crossing(g, {50, 1}, {50, 9}) == Cross::None);
    CHECK(so::crossing(g, {-20, 9}, {-20, 1}) == Cross::None);

    // Diagonal crossings still resolve.
    CHECK(so::crossing(g, {1, 1}, {9, 9}) == Cross::AtoB);
    CHECK(so::crossing(g, {9, 9}, {1, 1}) == Cross::BtoA);
}

TEST_CASE("reversing the line reverses the reported direction") {
    const GateLine g{{0, 5}, {10, 5}};
    const GateLine flipped{{10, 5}, {0, 5}};
    CHECK(so::crossing(g, {5, 1}, {5, 9}) == Cross::AtoB);
    CHECK(so::crossing(flipped, {5, 1}, {5, 9}) == Cross::BtoA);
}

TEST_CASE("a step that only touches the line does not count as a crossing") {
    const GateLine g{{0, 5}, {10, 5}};
    CHECK(so::crossing(g, {5, 1}, {5, 5}) == Cross::None);   // arrives at the line
    CHECK(so::crossing(g, {5, 5}, {5, 9}) == Cross::None);   // leaves from the line
    CHECK(so::crossing(g, {2, 5}, {8, 5}) == Cross::None);   // walks along it
}

TEST_CASE("a degenerate line can never be crossed") {
    const GateLine g{{5, 5}, {5, 5}};
    CHECK(so::crossing(g, {0, 0}, {10, 10}) == Cross::None);
}

TEST_CASE("a vertical gate line works the same way") {
    const GateLine g{{5, 0}, {5, 10}};
    CHECK(so::crossing(g, {1, 5}, {9, 5}) == Cross::BtoA);
    CHECK(so::crossing(g, {9, 5}, {1, 5}) == Cross::AtoB);
    CHECK(so::crossing(g, {1, 50}, {9, 50}) == Cross::None);
}
