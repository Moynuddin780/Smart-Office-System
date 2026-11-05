#include "doctest/doctest.h"

#include "core/postprocess_pool.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("every posted job runs exactly once") {
    so::PostprocessPool pool(3, 4096);
    std::atomic<int> n{0};
    for (int i = 0; i < 1000; ++i) CHECK(pool.post([&] { ++n; }));
    pool.drain();
    CHECK(n.load() == 1000);
    CHECK(pool.completed() == 1000);
    CHECK(pool.dropped() == 0);
}

TEST_CASE("the queue is bounded and sheds instead of growing") {
    so::PostprocessPool pool(1, 8);
    std::atomic<int> accepted{0};
    for (int i = 0; i < 200; ++i)
        if (pool.post([] { std::this_thread::sleep_for(2ms); })) ++accepted;

    CHECK(pool.queued() <= 8);
    CHECK(pool.dropped() > 0);
    CHECK(accepted.load() + static_cast<int>(pool.dropped()) == 200);
    pool.stop();
}

TEST_CASE("a job that throws does not take the pool down") {
    so::PostprocessPool pool(2, 64);
    std::atomic<int> after{0};
    CHECK(pool.post([] { throw std::runtime_error("module blew up"); }));
    for (int i = 0; i < 10; ++i) CHECK(pool.post([&] { ++after; }));
    pool.drain();
    CHECK(after.load() == 10);
    CHECK(pool.completed() == 11);   // the thrower counts as completed
}

TEST_CASE("work really is spread across the workers") {
    so::PostprocessPool pool(3, 256);
    std::atomic<int> concurrent{0};
    std::atomic<int> peak{0};
    for (int i = 0; i < 24; ++i) {
        pool.post([&] {
            const int c = ++concurrent;
            int prev = peak.load();
            while (c > prev && !peak.compare_exchange_weak(prev, c)) {}
            std::this_thread::sleep_for(20ms);
            --concurrent;
        });
    }
    pool.drain();
    CHECK(peak.load() > 1);
    CHECK(pool.workers() == 3);
}

TEST_CASE("posting after stop is refused rather than silently lost") {
    so::PostprocessPool pool(1, 8);
    pool.stop();
    CHECK_FALSE(pool.post([] {}));
}

TEST_CASE("drain returns immediately on an idle pool") {
    so::PostprocessPool pool(2, 8);
    pool.drain();
    CHECK(pool.queued() == 0);
}
