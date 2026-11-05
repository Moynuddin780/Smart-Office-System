#pragma once

#include <dxrt/dxrt_cxx_api.h>

#include <cstdlib>
#include <string>

namespace so_test {

/**
 * Is a real DX-M1 reachable from here?
 *
 * Tests that load an engine are the ones that prove the decoders against
 * hardware, and they cannot run inside a build container: there is no
 * /dev/dxrt0 and no dxrtd socket, so engine creation throws
 * "dxrt service is not running".
 *
 * They SKIP there rather than fail, which is what lets the same suite act as a
 * build gate and as the on-hardware verification. A skip says "not checked
 * here"; a failure would say "this is broken", and shipping a build that
 * reports the second when it means the first trains everyone to ignore it.
 */
inline bool npu_available() {
    static const bool ok = [] {
        try {
            dxrt::InferenceOption opt;
            opt.devices = {0};
            opt.boundOption = dxrt::InferenceOption::NPU_0;
            opt.useORT = false;
            dxrt::InferenceEngine probe("models/yolo26s_pose.dxnn", opt);
            return true;
        } catch (...) {
            return false;
        }
    }();
    return ok;
}

/**
 * Is there a usable video decoder here?
 *
 * The camera-reader tests decode real video, which needs GStreamer's PLUGINS
 * and the gst-launch tool, not just the development headers. A build container
 * has the headers and neither of the others, so these tests skip there for the
 * same reason the NPU ones do: the reader is verified on the board, where the
 * decode performance it exists to deliver actually matters.
 */
inline bool decoder_available() {
    static const bool ok = [] {
        // decodebin is what the reader actually instantiates now; the concrete
        // decoder is whatever the stream's caps call for. Checking for it
        // instead of a codec-specific element is also the difference between
        // "can decode" and "can decode H.264", which is the distinction that
        // the hardcoded-avdec_h264 bug turned on.
        return std::system("gst-inspect-1.0 decodebin >/dev/null 2>&1") == 0;
    }();
    return ok;
}

}  // namespace so_test

/// Put at the top of a test case that needs a video decoder.
#define SO_REQUIRE_DECODER()                                                 \
    do {                                                                     \
        if (!::so_test::decoder_available()) {                               \
            MESSAGE("skipped: no GStreamer decoder available here");         \
            return;                                                          \
        }                                                                    \
    } while (0)

/// Put at the top of a test case that needs the device.
#define SO_REQUIRE_NPU()                                                     \
    do {                                                                     \
        if (!::so_test::npu_available()) {                                   \
            MESSAGE("skipped: no DX-M1 reachable from here");                \
            return;                                                          \
        }                                                                    \
    } while (0)
