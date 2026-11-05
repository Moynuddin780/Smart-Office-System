#include "core/host_cpu.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace so {
namespace {

struct Sample { unsigned long long total{0}, idle{0}; };

std::mutex g_mtx;
std::map<int, Sample> g_prev;

int max_mhz(int cpu) {
    std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                    "/cpufreq/cpuinfo_max_freq");
    long khz = 0;
    if (f >> khz) return static_cast<int>(khz / 1000);
    return 0;
}

}  // namespace

HostCpu host_cpu() {
    std::lock_guard<std::mutex> lk(g_mtx);
    HostCpu out;

    // Inside a container this is the HOST's file, which is what we want: the
    // cameras compete with everything on the board, not only with each other.
    std::ifstream f("/proc/stat");
    if (!f) return out;

    std::string line;
    std::map<int, Sample> now;
    while (std::getline(f, line)) {
        if (line.compare(0, 3, "cpu") != 0 || line.size() < 4 ||
            !std::isdigit(static_cast<unsigned char>(line[3])))
            continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        const int idx = std::atoi(tag.c_str() + 3);
        Sample s;
        unsigned long long v = 0;
        for (int i = 0; ss >> v; ++i) {
            s.total += v;
            if (i == 3 || i == 4) s.idle += v;   // idle + iowait
        }
        now[idx] = s;
    }

    int fast = 0;
    for (const auto& [idx, s] : now) fast = std::max(fast, max_mhz(idx));

    double sum = 0, lsum = 0, bsum = 0;
    int lcount = 0, bcount = 0;
    for (const auto& [idx, s] : now) {
        CpuCore c;
        c.index = idx;
        c.max_mhz = max_mhz(idx);
        // "Big" relative to this board rather than against a fixed number, so
        // the same code is honest on a part with a different clock.
        c.big = fast > 0 && c.max_mhz >= fast;

        const auto prev = g_prev.find(idx);
        if (prev != g_prev.end() && s.total > prev->second.total) {
            const double dt = static_cast<double>(s.total - prev->second.total);
            const double di = static_cast<double>(s.idle - prev->second.idle);
            c.busy = 100.0 * (dt - di) / dt;
        }
        sum += c.busy;
        (c.big ? bsum : lsum) += c.busy;
        (c.big ? bcount : lcount) += 1;
        out.hottest = std::max(out.hottest, c.busy);
        out.cores.push_back(c);
    }
    g_prev = now;

    if (!out.cores.empty()) out.board = sum / static_cast<double>(out.cores.size());
    if (lcount) out.little = lsum / lcount;
    if (bcount) out.big = bsum / bcount;
    return out;
}

}  // namespace so
