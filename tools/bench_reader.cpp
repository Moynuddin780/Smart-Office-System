// bench_reader — where does the per-frame CPU actually go?
//
// The design assumes decode dominates and that a sub-stream is an order of
// magnitude cheaper. This measures it on THIS board rather than inheriting the
// number, and separates the decoder from the colour conversion and the copy,
// because those are three different fixes.

#include "core/camera_reader.hpp"
#include "core/log.hpp"
#include "core/preprocess.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;

static void run(const std::string& url, int seconds, const char* label) {
    so::CameraReaderConfig cfg;
    cfg.url = url;
    so::CameraReader r(cfg);
    if (!r.start()) {
        std::printf("  %-18s FAILED to start\n", label);
        return;
    }

    so::Frame f;
    // Wait for the first frame so connection time is not counted as decode time.
    for (int i = 0; i < 40 && !r.read(f, std::chrono::milliseconds(500)); ++i) {}

    const auto t0 = Clock::now();
    const auto base = r.frames_decoded();
    int n = 0;
    double letterbox_ms = 0;
    const auto deadline = t0 + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        if (!r.read(f, std::chrono::milliseconds(200))) continue;
        ++n;
        const auto l0 = Clock::now();
        so::LetterboxMap m;
        volatile auto canvas = so::letterbox(f.image, 640, m);
        (void)canvas;
        letterbox_ms += std::chrono::duration<double, std::milli>(Clock::now() - l0).count();
    }
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto decoded = r.frames_decoded() - base;

    std::printf("  %-18s %4dx%-4d  decoded %4lld in %.1fs = %5.1f fps  "
                "| consumed %3d  | letterbox %.2f ms/frame  | %s\n",
                label, r.width(), r.height(),
                static_cast<long long>(decoded), secs,
                static_cast<double>(decoded) / secs, n,
                n ? letterbox_ms / n : 0.0,
                so::to_string(r.decode_path()));
    r.stop();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: bench_reader <clip-or-rtsp> [more...]\n");
        return 2;
    }
    std::printf("bench_reader — decode throughput per source\n");
    for (int i = 1; i < argc; ++i) run(argv[i], 6, argv[i] + (strlen(argv[i]) > 34 ? strlen(argv[i]) - 34 : 0));
    return 0;
}
