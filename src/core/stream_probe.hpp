#pragma once

#include <string>

namespace so {

/// What one RTSP URL turns out to be.
struct StreamFacts {
    bool ok{false};
    int width{0};
    int height{0};
    double fps{0};             ///< what the camera says it sends
    std::string decode_path;   ///< "software" | "rkmpp (hardware)"
    std::string error;         ///< why not, when ok is false
};

/**
 * Open a camera URL, keep the first frame, and report what arrived.
 *
 * WHY THE PRODUCT NEEDS THIS AND NOT JUST A CLI FLAG: every camera here offers
 * a main stream and a sub-stream, and choosing between them is the single
 * decision that decides whether faces are big enough to recognise and whether
 * the board can keep up. The form used to offer "Sub-stream (recommended)"
 * against a URL nobody had opened — a recommendation made in ignorance. Worse,
 * the sizes the database remembered were wrong for ALL FIVE cameras on this
 * site, because nothing re-checked them after the first save.
 *
 * Asking the camera is the only honest way to fill that in, and it takes a
 * couple of seconds.
 *
 * Runs the real pipeline, so what it reports is what the product will get —
 * not what ffprobe would get through a different code path.
 */
StreamFacts probe_stream(const std::string& url, int timeout_seconds = 12);

}  // namespace so
