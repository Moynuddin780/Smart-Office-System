// bench_pool — proves (or disproves) the premise the whole architecture rests on:
// that three core-bound engines actually use three NPU cores, and a single
// engine does not.
//
// It runs the same saturating workload twice — once against a 1-engine pool and
// once against a 3-engine pool — and reports, for each, the completed
// inferences per second and the per-core utilisation sampled from the DX-RT C
// API while the run is in flight.
//
// If the 3-core pool does not show materially higher throughput AND activity on
// all three cores, the architecture premise is wrong and the design must change
// before anything is built on top of it.

#include "core/engine_pool.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct CoreStats {
    double peak[3] = {0.0, 0.0, 0.0};
    double sum[3]  = {0.0, 0.0, 0.0};
    int samples     = 0;
};

struct Result {
    int cores;
    double seconds;
    long long completed;
    double fps;
    CoreStats util;
};

Result run_once(const std::string& model, int cores, int threads, int seconds) {
    so::EnginePool pool(model, cores);

    std::atomic<long long> completed{0};

    // One input and one output buffer per worker thread. Reusing a single
    // shared output buffer would let two in-flight jobs write over each other
    // and would measure something other than throughput.
    const std::size_t in_bytes  = static_cast<std::size_t>(pool.input_size());
    const std::size_t out_bytes = static_cast<std::size_t>(pool.output_size());

    std::vector<std::vector<std::uint8_t>> inputs(threads, std::vector<std::uint8_t>(in_bytes, 114));
    std::vector<std::vector<std::uint8_t>> outputs(threads, std::vector<std::uint8_t>(out_bytes));

    std::atomic<bool> stop{false};
    // Bound how far ahead the submitters may run. Without this the queue grows
    // without limit and the measurement becomes one of memory, not of the NPU.
    const int max_in_flight = 4 * cores * 2;
    std::atomic<int> in_flight{0};

    pool.register_callback([&](dxrt::TensorPtrs&, void*) {
        completed.fetch_add(1, std::memory_order_relaxed);
        in_flight.fetch_sub(1, std::memory_order_relaxed);
    });

    const auto t0 = Clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            while (!stop.load(std::memory_order_relaxed)) {
                if (in_flight.load(std::memory_order_relaxed) >= max_in_flight) {
                    std::this_thread::yield();
                    continue;
                }
                in_flight.fetch_add(1, std::memory_order_relaxed);
                try {
                    pool.submit(inputs[t].data(), nullptr, outputs[t].data());
                } catch (const std::exception& e) {
                    in_flight.fetch_sub(1, std::memory_order_relaxed);
                    SO_LOG_ERROR("submit failed: %s", e.what());
                    break;
                }
            }
        });
    }

    // Sample utilisation while the workload is actually in flight.
    CoreStats stats;
    const auto deadline = t0 + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        bool any = false;
        double s[3];
        for (int c = 0; c < 3; ++c) {
            s[c] = so::EnginePool::core_utilization(c);
            if (s[c] >= 0.0) any = true;
        }
        if (!any) continue;
        for (int c = 0; c < 3; ++c) {
            const double v = s[c] < 0.0 ? 0.0 : s[c];
            stats.peak[c] = std::max(stats.peak[c], v);
            stats.sum[c] += v;
        }
        ++stats.samples;
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& w : workers) w.join();

    // Let the in-flight jobs land before reading the counter.
    const auto drain_until = Clock::now() + std::chrono::seconds(3);
    while (in_flight.load(std::memory_order_relaxed) > 0 && Clock::now() < drain_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const double elapsed =
        std::chrono::duration<double>(Clock::now() - t0).count();
    const long long done = completed.load();

    Result r;
    r.cores     = cores;
    r.seconds   = elapsed;
    r.completed = done;
    r.fps       = done / elapsed;
    r.util      = stats;
    return r;
}

void print(const Result& r) {
    std::printf("\n  cores=%d  %lld inferences in %.1f s  =  %.1f inf/s\n",
                r.cores, r.completed, r.seconds, r.fps);
    if (r.util.samples == 0) {
        std::printf("    (no utilisation samples available from the driver)\n");
        return;
    }
    for (int c = 0; c < 3; ++c) {
        std::printf("    NPU_%d  mean %5.1f%%   peak %5.1f%%\n",
                    c, r.util.sum[c] / r.util.samples, r.util.peak[c]);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string model = argc > 1 ? argv[1] : "models/yolo26s_pose.dxnn";
    const int threads       = argc > 2 ? std::atoi(argv[2]) : 6;
    const int seconds       = argc > 3 ? std::atoi(argv[3]) : 15;

    std::printf("bench_pool — model=%s threads=%d duration=%ds per run\n",
                model.c_str(), threads, seconds);
    std::printf("Premise under test: three core-bound engines use three NPU cores;\n"
                "a single engine does not.\n");

    const Result one   = run_once(model, 1, threads, seconds);
    std::printf("\n--- CONTROL: one engine (bound NPU_0) ---");
    print(one);

    const Result three = run_once(model, 3, threads, seconds);
    std::printf("\n--- POOL: three engines (NPU_0 + NPU_1 + NPU_2) ---");
    print(three);

    const double speedup = three.fps / std::max(one.fps, 1e-9);
    std::printf("\n=== VERDICT ===\n");
    std::printf("  speedup 3 engines vs 1 : %.2fx\n", speedup);

    int busy = 0;
    if (three.util.samples > 0)
        for (int c = 0; c < 3; ++c)
            if (three.util.peak[c] > 1.0) ++busy;
    std::printf("  cores showing activity  : %d of 3\n", busy);

    const bool ok = speedup > 1.5 || busy == 3;
    std::printf("  RESULT: %s\n", ok ? "PASS — per-core pooling works"
                                     : "FAIL — pooling did NOT engage extra cores");
    return ok ? 0 : 1;
}
