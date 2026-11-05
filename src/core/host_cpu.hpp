#pragma once

#include <string>
#include <vector>

namespace so {

/// One CPU core, as the kernel reports it.
struct CpuCore {
    int index{0};
    double busy{0};        ///< per cent, since the previous sample
    int max_mhz{0};
    /// RK3588 pairs four slow cores with four fast ones, and the difference is
    /// not cosmetic: a decode thread parked on a 1.8 GHz A55 is a quarter
    /// slower than the same thread on a 2.26 GHz A76. A reading that averages
    /// them hides the one case worth seeing — the board idle on average while
    /// one big core is pinned.
    bool big{false};
};

/// Host CPU use, sampled between calls.
///
/// WHY THE APPLIANCE REPORTS THIS AT ALL: the NPU sits at a fifth of its
/// capacity while cameras fall behind, and the question "is the board out of
/// room, or is the work badly spread" cannot be answered from inside the
/// container without this. It was answered with ssh and /proc/stat by hand
/// several times before it was worth building in.
struct HostCpu {
    std::vector<CpuCore> cores;
    double board{0};       ///< mean across all cores, per cent
    double little{0};
    double big{0};
    double hottest{0};     ///< the busiest single core — what actually limits
};

/// Sample now. The FIRST call establishes a baseline and reports zeroes;
/// every later call covers the period since the one before it.
HostCpu host_cpu();

}  // namespace so
