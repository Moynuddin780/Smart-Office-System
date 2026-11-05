#pragma once

// What must be true of a camera before it is stored.
//
// These rules live in a header of their own, over a small plain struct rather
// than the database row, for one reason: they are the product's rules and they
// deserve tests. Hanging them off db::CameraRow would have dragged libpq into
// the test binary to check a string prefix.

#include <string>
#include <utility>

namespace so::api {

/// The fields a camera is judged on. Deliberately not db::CameraRow.
struct CameraDraft {
    std::string name;
    std::string rtsp_main;
    std::string rtsp_sub;
    std::string stream_pref = "sub";   ///< "sub" or "main"
    std::string role = "area";
    bool face_enabled = false;
    int detect_hz = 10;
    int face_hz = 6;
    int probed_width = 0;
    int probed_height = 0;
};

/// Live cameras only — no files, no recordings.
///
/// The reader can open a local file, and during development that was convenient:
/// a clip stood in for a camera. It is the wrong thing to allow in the product.
/// A recording looping on the wall beside real cameras reads as a room being
/// watched when nobody is watching it, and the operator cannot tell the two
/// apart. So the rule lives at the EDGE, where a URL enters the system — not in
/// the reader, whose file support the tests still need.
inline std::string stream_url_problem(const std::string& url, const char* which) {
    if (url.empty()) return "";
    const bool rtsp = url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0;
    if (!rtsp)
        return std::string("The ") + which +
               " stream must be a live RTSP URL beginning with rtsp:// — a file or a "
               "recording cannot be used as a camera.";
    // The host is whatever sits immediately after "rtsp://". Note the check is
    // on THAT character and not on "the first character that is not a slash" —
    // the latter happily skips over the empty host in "rtsp:///path" and calls
    // the path's first letter a hostname.
    const auto host = url.find("//") + 2;
    if (host >= url.size() || url[host] == '/' || url[host] == '@')
        return std::string("The ") + which + " stream URL is missing the camera host.";
    return "";
}

/// The stream the camera is actually set to use has to exist.
///
/// Without this a camera can be saved with only a main URL but set to "sub",
/// which produces a camera that is configured, enabled, and permanently blank.
inline std::string stream_choice_problem(const CameraDraft& c) {
    const bool sub = c.stream_pref != "main";
    if (sub && c.rtsp_sub.empty())
        return "This camera is set to use its sub-stream, so the sub-stream URL is required.";
    if (!sub && c.rtsp_main.empty())
        return "This camera is set to use its main stream, so the main stream URL is required.";
    return "";
}

/// The resolution guard for a face camera.
///
/// This used to demand 1280x720 before face recognition could be switched on at
/// all, on the arithmetic that a 60 px face needs about 450 px of person. The
/// floor is no longer 60 px — small faces are now admitted on accumulated
/// evidence instead of refused on size — so that bar rejected cameras that do
/// in fact identify people, and did it as a hard error the operator could not
/// override.
///
/// What remains is the case no amount of evidence survives: below 640x360 a
/// person has to fill the frame before their face reaches even the 24 px the
/// landmarks need, so such a stream cannot do this job at any distance.
///
/// Whether a camera ACTUALLY sees usable faces is not decided here and cannot
/// be: it depends on where the camera points and how far away people walk. That
/// is reported from measured face sizes once the camera has run, which beats
/// any guess made from the resolution alone.
inline std::string gate_resolution_problem(const CameraDraft& c) {
    if (!c.face_enabled) return "";
    if (c.stream_pref == "main") return "";                      // the operator opted out
    if (c.probed_width == 0 || c.probed_height == 0) return "";  // not probed yet
    if (c.probed_width >= 640 && c.probed_height >= 360) return "";
    return "This camera's sub-stream is " + std::to_string(c.probed_width) + "x" +
           std::to_string(c.probed_height) +
           ", which is too small for face recognition at any distance: a face "
           "needs about 24 px, which is roughly 180 px of person. Either raise "
           "the sub-stream in the camera's own settings, or set this camera to "
           "use the main stream.";
}

/// Every rule, in one call.
///
/// Create and update both go through this so they cannot drift apart — an
/// update path quietly skipping a rule the create path enforces is exactly how
/// invalid rows appear in a database that has validation.
///
/// NOTE what is deliberately NOT here: the role is not checked against a list.
/// A site names its cameras' roles itself; the engine does not branch on the
/// role, only on which modules are enabled for the camera and whether a gate
/// line exists. Only emptiness is refused, because a blank role renders as a
/// blank label.
inline std::string camera_problem(const CameraDraft& c) {
    if (c.name.empty()) return "The camera needs a name.";
    if (c.role.empty()) return "The camera needs a role — 'area', 'gate', or your own name for it.";
    if (c.rtsp_main.empty() && c.rtsp_sub.empty())
        return "At least one stream URL is required.";

    for (const auto& [url, which] : {std::pair<const std::string&, const char*>{c.rtsp_main, "main"},
                                     std::pair<const std::string&, const char*>{c.rtsp_sub, "sub"}}) {
        const std::string p = stream_url_problem(url, which);
        if (!p.empty()) return p;
    }
    const std::string choice = stream_choice_problem(c);
    if (!choice.empty()) return choice;

    if (c.detect_hz < 1 || c.detect_hz > 60)
        return "The detection rate must be between 1 and 60 per second.";
    if (c.face_hz < 1 || c.face_hz > 60)
        return "The face rate must be between 1 and 60 per second.";
    return gate_resolution_problem(c);
}

}  // namespace so::api
