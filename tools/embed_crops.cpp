/**
 * Embed a directory of aligned 112x112 crops with a given .dxnn, on the NPU.
 *
 * This exists so a candidate recognition model can be judged on the SAME path
 * production uses — EnginePool + ArcFace — rather than through a separate
 * Python binding that might normalise differently and flatter the result.
 *
 *     embed_crops <model.dxnn> <crops_dir> <out.csv>
 *
 * Writes one row per crop: filename,e0,e1,...,e511 (L2-normalised).
 */
#include "core/engine_pool.hpp"
#include "vision/arcface.hpp"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: embed_crops <model.dxnn> <crops_dir> <out.csv>\n");
        return 2;
    }
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(argv[2]))
        if (e.path().extension() == ".png") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::fprintf(stderr, "no .png in %s\n", argv[2]); return 2; }

    so::EnginePool pool(argv[1], 1);     // one core: leave the appliance usable
    so::ArcFace arc(pool);

    std::FILE* out = std::fopen(argv[3], "w");
    if (!out) { std::perror("fopen"); return 2; }

    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& f : files) {
        const cv::Mat crop = cv::imread(f.string());
        if (crop.empty() || crop.cols != 112 || crop.rows != 112) {
            std::fprintf(stderr, "skip %s\n", f.c_str());
            continue;
        }
        const auto v = arc.embed(crop);
        std::fprintf(out, "%s", f.filename().c_str());
        for (float x : v) std::fprintf(out, ",%.6f", x);
        std::fprintf(out, "\n");
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    std::fclose(out);
    std::fprintf(stderr, "%zu crops, %.1f ms/face -> %s\n", files.size(),
                 ms / static_cast<double>(files.size()), argv[3]);
    return 0;
}
