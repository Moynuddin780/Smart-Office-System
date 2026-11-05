#include "doctest/doctest.h"

#include "device.hpp"

#include "core/engine_pool.hpp"

#include <set>
#include <string>
#include <vector>

namespace {
const std::string kPoseModel = "models/yolo26s_pose.dxnn";
}

// ── Pure checks — no device needed ──────────────────────────────────────────

TEST_CASE("core index maps to the per-core bound option, never NPU_ALL") {
    CHECK(so::EnginePool::bound_option_for(0) ==
          static_cast<std::uint32_t>(dxrt::InferenceOption::NPU_0));
    CHECK(so::EnginePool::bound_option_for(1) ==
          static_cast<std::uint32_t>(dxrt::InferenceOption::NPU_1));
    CHECK(so::EnginePool::bound_option_for(2) ==
          static_cast<std::uint32_t>(dxrt::InferenceOption::NPU_2));

    // NPU_ALL is 0 in the DXRT enum and NPU_0 is 1, so a pool can never
    // accidentally produce the option this whole class exists to avoid.
    for (int c = 0; c < 3; ++c)
        CHECK(so::EnginePool::bound_option_for(c) !=
              static_cast<std::uint32_t>(dxrt::InferenceOption::NPU_ALL));
}

TEST_CASE("a core index outside the DX-M1's three cores is rejected") {
    CHECK_THROWS_AS(so::EnginePool::bound_option_for(3), std::invalid_argument);
    CHECK_THROWS_AS(so::EnginePool::bound_option_for(-1), std::invalid_argument);
}

// ── Device checks — these load real engines on the DX-M1 ────────────────────

TEST_CASE("EnginePool loads one engine per core") {
    SO_REQUIRE_NPU();
    so::EnginePool pool(kPoseModel, 3);
    CHECK(pool.size() == 3);
}

TEST_CASE("round-robin visits every core before repeating one") {
    SO_REQUIRE_NPU();
    so::EnginePool pool(kPoseModel, 3);
    std::set<int> seen;
    for (std::size_t i = 0; i < pool.size(); ++i) seen.insert(pool.next_index());
    CHECK(seen.size() == pool.size());

    // ...and then wraps, rather than running off the end.
    const int after_wrap = pool.next_index();
    CHECK(after_wrap >= 0);
    CHECK(after_wrap < static_cast<int>(pool.size()));
}

TEST_CASE("the pool reports the model geometry probed on this board") {
    SO_REQUIRE_NPU();
    so::EnginePool pool(kPoseModel, 1);
    const std::vector<int64_t> expected{1, 640, 640, 3};
    CHECK(pool.input_shape() == expected);
    CHECK(pool.output_size() > 0);

    // yolo26s_pose emits 9 tensors: 3 box + 3 class + 3 keypoint maps.
    CHECK(pool.output_shapes().size() == 9);

    int box = 0, cls = 0, kpt = 0;
    for (const auto& s : pool.output_shapes()) {
        REQUIRE(s.size() == 4);
        if (s[1] == 4) ++box;
        else if (s[1] == 1) ++cls;
        else if (s[1] == 51) ++kpt;
    }
    CHECK(box == 3);
    CHECK(cls == 3);
    CHECK(kpt == 3);   // 17 keypoints x (x, y, conf)
}

TEST_CASE("a pool can be built for a single core so the board can be shared") {
    SO_REQUIRE_NPU();
    so::EnginePool pool(kPoseModel, 1);
    CHECK(pool.size() == 1);
    CHECK(pool.next_index() == 0);
    CHECK(pool.next_index() == 0);
}
