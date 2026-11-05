#pragma once

#include <string>
#include <vector>

namespace so {

/// One resolution a camera says it can encode.
struct OnvifResolution {
    int width{0};
    int height{0};
};

/// What one of a camera's streams is doing, and what it could do instead.
///
/// The "could do instead" half is the point. Until now the only question this
/// system could answer about a camera was what it is sending AT THIS MOMENT,
/// by opening the stream and looking. That is enough to choose between two URLs
/// and useless for the question an operator actually asks, which is what else
/// this camera is capable of.
struct OnvifStream {
    std::string profile;     ///< the camera's own name, e.g. "D01_CH01_Main"
    std::string token;       ///< encoder configuration token
    std::string encoding;    ///< "H264", "H265", ...
    int width{0}, height{0}; ///< what it is set to now
    int fps{0};
    int bitrate{0};
    int gov_length{0};
    int fps_min{0}, fps_max{0};
    std::vector<OnvifResolution> resolutions;   ///< everything it will accept
};

/// The host and credentials inside an RTSP URL.
struct OnvifTarget {
    std::string host;
    std::string user;
    std::string pass;
    bool ok{false};
};

/// Pull host/user/password out of `rtsp://user:pass@host:554/path`.
///
/// The camera's ONVIF service lives on HTTP at the same host with the same
/// credentials, so the RTSP URL an operator has already typed is all that is
/// needed — asking them for the address and password a second time would be
/// asking for the same two facts in a second place to get out of step.
OnvifTarget onvif_target_from_rtsp(const std::string& rtsp_url);

/// The WS-Security header a request must carry.
///
/// Split out from the request so it can be tested against a known nonce and
/// timestamp: the digest is `base64(sha1(nonce + created + password))`, and
/// getting any part of that wrong produces a 400 from the camera that looks
/// exactly like a wrong password.
std::string onvif_security_header(const std::string& user, const std::string& pass,
                                  const std::string& nonce_raw,
                                  const std::string& created);

/// Read the streams out of a GetProfiles response.
std::vector<OnvifStream> onvif_parse_profiles(const std::string& xml);

/// Fill in `resolutions` and the frame-rate range from a
/// GetVideoEncoderConfigurationOptions response, for the encoding the stream is
/// already using.
void onvif_parse_options(const std::string& xml, OnvifStream& into);

/// The SetVideoEncoderConfiguration body that changes only the resolution.
///
/// Empty when `width`x`height` is not a size this stream offered — the device
/// answers an unsupported size with a success it does not act on, so refusing
/// here is the difference between "that size is not available" and a page that
/// reports a change the camera never made.
///
/// Every other field is carried over from `s` unchanged, because ONVIF replaces
/// the whole configuration rather than merging into it.
std::string onvif_set_resolution_body(const OnvifStream& s, int width, int height);

/// Change a stream's resolution and report what the camera ACTUALLY has
/// afterwards, by reading it back.
///
/// The read-back is the point. This camera accepts a frame-rate change, returns
/// no fault, and keeps sending the old rate; trusting the response would make
/// the page lie. The caller compares what it asked for with what comes back.
bool onvif_apply_resolution(const OnvifTarget& t, const std::string& token,
                            int width, int height, OnvifStream* after,
                            std::string* error = nullptr, int timeout_s = 8);

/// Ask a camera over the network. Empty on any failure — a camera that does not
/// speak ONVIF is ordinary, not an error worth failing a page over.
///
/// `error` is filled with something an operator can act on when the list comes
/// back empty.
std::vector<OnvifStream> onvif_streams(const OnvifTarget& t, std::string* error = nullptr,
                                       int timeout_s = 6);

}  // namespace so
