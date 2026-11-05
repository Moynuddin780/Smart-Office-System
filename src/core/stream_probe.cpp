#include "core/stream_probe.hpp"

#include "core/camera_reader.hpp"
#include "core/log.hpp"

#include <chrono>

namespace so {

StreamFacts probe_stream(const std::string& url, int timeout_seconds) {
    StreamFacts out;
    if (url.empty()) {
        out.error = "no URL given";
        return out;
    }

    CameraReaderConfig rc;
    rc.url = url;
    // UNCAPPED ON PURPOSE. The question being answered is "what does this
    // camera send", and a cap would answer "what did we ask it to send".
    rc.max_width = 0;
    rc.max_height = 0;
    rc.reconnect_min_ms = 500;

    CameraReader r(rc);
    if (!r.start()) {
        out.error = "the reader refused to start";
        return out;
    }

    Frame f;
    bool got = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (!got && std::chrono::steady_clock::now() < deadline)
        got = r.read(f, std::chrono::milliseconds(400));

    if (!got) {
        r.stop();
        // The wording matters more than it looks. These cameras serve only a
        // couple of RTSP sessions at once, and when this appliance already
        // holds one, a probe of the OTHER stream gets silence — which reads
        // exactly like a wrong password and sends somebody off to re-check
        // credentials that were never wrong. That mistake was made here, by
        // the author of this file, before the message said so.
        out.error = "no frame arrived. If this camera is already streaming to "
                    "this system, it may allow only one connection at a time — "
                    "that is the usual cause and nothing is wrong. Otherwise "
                    "check the path, the password, or the network.";
        return out;
    }

    out.ok = true;
    out.width = f.image.cols;
    out.height = f.image.rows;
    out.fps = r.stream_fps();
    out.decode_path = to_string(r.decode_path());
    r.stop();
    return out;
}

}  // namespace so
