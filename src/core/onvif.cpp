#include "core/onvif.hpp"

#include "core/log.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <regex>

#ifdef SO_HAVE_CURL
#include <curl/curl.h>
#endif

namespace so {
namespace {

std::string b64(const std::string& raw) {
    std::string out(4 * ((raw.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(raw.data()),
                                  static_cast<int>(raw.size()));
    out.resize(n < 0 ? 0 : static_cast<std::size_t>(n));
    return out;
}

std::string sha1(const std::string& in) {
    unsigned char md[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(in.data()), in.size(), md);
    return std::string(reinterpret_cast<char*>(md), SHA_DIGEST_LENGTH);
}

/// The first capture of `re`, or an empty string.
std::string pick(const std::string& hay, const std::string& re) {
    std::smatch m;
    if (!std::regex_search(hay, m, std::regex(re))) return {};
    return m[1].str();
}

int pick_int(const std::string& hay, const std::string& re) {
    const std::string s = pick(hay, re);
    return s.empty() ? 0 : std::atoi(s.c_str());
}

/// XML namespace prefixes are not fixed by the standard and devices do differ,
/// so every tag is matched as "optional prefix, then the local name". Matching
/// `<tt:Width>` literally works on the camera it was written against and fails
/// on the next one, which is the sort of bug that only appears on site.
std::string tag(const char* name) {
    return "<(?:[A-Za-z0-9_]+:)?" + std::string(name) + ">";
}

std::string tag_value(const char* name) {
    return tag(name) + "\\s*([^<]*)\\s*</(?:[A-Za-z0-9_]+:)?" + std::string(name) + ">";
}

/// The text between `<name ...>` and `</name>`, searched from `from`.
/// Returns false when the block is absent or unterminated.
bool block(const std::string& xml, const char* name, std::size_t from,
           std::size_t& begin, std::string& body) {
    const std::regex open("<(?:[A-Za-z0-9_]+:)?" + std::string(name) + "[ >]");
    const std::regex close("</(?:[A-Za-z0-9_]+:)?" + std::string(name) + ">");
    std::smatch m;
    auto it = xml.cbegin() + static_cast<long>(std::min(from, xml.size()));
    if (!std::regex_search(it, xml.cend(), m, open)) return false;
    const std::size_t s = static_cast<std::size_t>(m.position(0)) + from;
    std::smatch c;
    auto it2 = xml.cbegin() + static_cast<long>(s);
    if (!std::regex_search(it2, xml.cend(), c, close)) return false;
    begin = s;
    body = xml.substr(s, static_cast<std::size_t>(c.position(0)) + c.length(0));
    return true;
}

}  // namespace

OnvifTarget onvif_target_from_rtsp(const std::string& url) {
    OnvifTarget t;
    if (url.rfind("rtsp://", 0) != 0) return t;
    std::string rest = url.substr(7);
    const std::size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.empty()) return t;

    // SPLIT ON THE LAST '@', NOT THE FIRST.
    //
    // A password containing '@' is ordinary — one camera on this site uses
    // `Ex@mple_pw` — and splitting on the first one puts the tail of the
    // password into the host, producing "cannot reach camera mple_pw" for a
    // camera that is answering perfectly.
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        const std::string creds = authority.substr(0, at);
        authority = authority.substr(at + 1);
        const std::size_t colon = creds.find(':');
        if (colon == std::string::npos) {
            t.user = creds;
        } else {
            t.user = creds.substr(0, colon);
            t.pass = creds.substr(colon + 1);
        }
    }
    // The RTSP port is dropped: ONVIF is a separate HTTP service, and 554 is
    // not where it listens.
    const std::size_t colon = authority.find(':');
    t.host = colon == std::string::npos ? authority : authority.substr(0, colon);
    if (t.host.empty()) return t;
    t.ok = true;
    return t;
}

std::string onvif_security_header(const std::string& user, const std::string& pass,
                                  const std::string& nonce_raw,
                                  const std::string& created) {
    const std::string digest = b64(sha1(nonce_raw + created + pass));
    return
        "<s:Header><Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/"
        "2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        "<UsernameToken><Username>" + user + "</Username>"
        "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" + digest + "</Password>"
        "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" + b64(nonce_raw) + "</Nonce>"
        "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-utility-1.0.xsd\">" + created + "</Created>"
        "</UsernameToken></Security></s:Header>";
}

std::vector<OnvifStream> onvif_parse_profiles(const std::string& xml) {
    std::vector<OnvifStream> out;
    std::size_t pos = 0;
    for (;;) {
        std::size_t at = 0;
        std::string prof;
        if (!block(xml, "Profiles", pos, at, prof)) break;
        pos = at + prof.size();

        // The profile's own name comes FIRST in the profile, before the names
        // of the video source, audio source, analytics and PTZ configurations
        // that follow it. Searching the whole profile for a Name finds the
        // profile's; searching the encoder block finds "MainStream", which is
        // the encoder's name and the same string on both streams of some
        // devices.
        OnvifStream s;
        s.profile = pick(prof, tag_value("Name"));

        std::size_t ea = 0;
        std::string enc;
        if (!block(prof, "VideoEncoderConfiguration", 0, ea, enc)) continue;
        s.token = pick(enc, "VideoEncoderConfiguration token=\"([^\"]+)\"");
        s.encoding = pick(enc, tag_value("Encoding"));
        s.width = pick_int(enc, tag_value("Width"));
        s.height = pick_int(enc, tag_value("Height"));
        s.fps = pick_int(enc, tag_value("FrameRateLimit"));
        s.bitrate = pick_int(enc, tag_value("BitrateLimit"));
        s.gov_length = pick_int(enc, tag_value("GovLength"));
        if (s.token.empty() || s.width <= 0) continue;
        out.push_back(std::move(s));
    }
    return out;
}

void onvif_parse_options(const std::string& xml, OnvifStream& into) {
    if (into.encoding.empty()) return;
    // ONLY the block for the codec this stream is using, and only the FIRST
    // one. This device repeats its H264 block inside <tt:Extension>; taking
    // both appends the same six resolutions twice and the page then lists every
    // size two times. A block for another codec describes sizes that would be
    // valid only after also changing the codec, which is a different decision.
    std::size_t at = 0;
    std::string blk;
    if (!block(xml, into.encoding.c_str(), 0, at, blk)) return;

    const std::regex res(tag("Width") + "\\s*(\\d+)\\s*</(?:[A-Za-z0-9_]+:)?Width>\\s*" +
                         tag("Height") + "\\s*(\\d+)");
    for (auto it = std::sregex_iterator(blk.begin(), blk.end(), res);
         it != std::sregex_iterator(); ++it) {
        OnvifResolution r{std::atoi((*it)[1].str().c_str()),
                          std::atoi((*it)[2].str().c_str())};
        const bool dup = std::any_of(into.resolutions.begin(), into.resolutions.end(),
                                     [&](const OnvifResolution& e) {
                                         return e.width == r.width && e.height == r.height;
                                     });
        if (!dup) into.resolutions.push_back(r);
    }

    std::size_t fa = 0;
    std::string fr;
    if (block(blk, "FrameRateRange", 0, fa, fr)) {
        into.fps_min = pick_int(fr, tag_value("Min"));
        into.fps_max = pick_int(fr, tag_value("Max"));
    }
}

std::string onvif_set_resolution_body(const OnvifStream& s, int width, int height) {
    if (width <= 0 || height <= 0 || s.token.empty()) return {};
    // Only a size the camera itself listed. When no list was read the check
    // cannot be made, and sending blind is worse than refusing.
    const bool offered = std::any_of(s.resolutions.begin(), s.resolutions.end(),
                                     [&](const OnvifResolution& r) {
                                         return r.width == width && r.height == height;
                                     });
    if (!offered) return {};

    const auto n = [](int v) { return std::to_string(v); };
    return
        "<trt:SetVideoEncoderConfiguration><trt:Configuration token=\"" + s.token + "\">"
        "<tt:Name>" + (s.profile.empty() ? std::string("Stream") : s.profile) + "</tt:Name>"
        "<tt:UseCount>1</tt:UseCount>"
        "<tt:Encoding>" + s.encoding + "</tt:Encoding>"
        "<tt:Resolution><tt:Width>" + n(width) + "</tt:Width>"
        "<tt:Height>" + n(height) + "</tt:Height></tt:Resolution>"
        "<tt:Quality>1</tt:Quality>"
        "<tt:RateControl>"
        // CARRIED, NOT CHOSEN. This device honours a resolution change and
        // ignores a frame-rate change, so any rate we invented here would be a
        // silent no-op that the page might then report as applied.
        "<tt:FrameRateLimit>" + n(s.fps) + "</tt:FrameRateLimit>"
        "<tt:EncodingInterval>1</tt:EncodingInterval>"
        "<tt:BitrateLimit>" + n(s.bitrate) + "</tt:BitrateLimit>"
        "</tt:RateControl>"
        "<tt:" + s.encoding + "><tt:GovLength>" + n(s.gov_length) + "</tt:GovLength>"
        "<tt:" + s.encoding + "Profile>High</tt:" + s.encoding + "Profile></tt:" + s.encoding + ">"
        "<tt:SessionTimeout>PT0S</tt:SessionTimeout>"
        "</trt:Configuration><trt:ForcePersistence>true</trt:ForcePersistence>"
        "</trt:SetVideoEncoderConfiguration>";
}

#ifdef SO_HAVE_CURL
namespace {

std::size_t sink(char* p, std::size_t sz, std::size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * n);
    return sz * n;
}

std::string now_utc() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string soap(const OnvifTarget& t, const std::string& body, int timeout_s,
                 long* status) {
    std::string nonce(16, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), 16);
    const std::string env =
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
        "xmlns:tt=\"http://www.onvif.org/ver10/schema\">" +
        (t.user.empty() ? std::string() : onvif_security_header(t.user, t.pass, nonce, now_utc())) +
        "<s:Body>" + body + "</s:Body></s:Envelope>";

    CURL* c = curl_easy_init();
    if (!c) return {};
    std::string out;
    const std::string url = "http://" + t.host + "/onvif/device_service";
    curl_slist* hdr = curl_slist_append(nullptr,
                                        "Content-Type: application/soap+xml; charset=utf-8");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, env.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(env.size()));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, static_cast<long>(std::min(timeout_s, 4)));
    // Some devices answer WS-Security and some insist on HTTP digest; asking
    // curl for ANY costs one extra round trip on the latter and nothing on the
    // former, which is cheaper than guessing per vendor.
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));
    if (!t.user.empty()) {
        curl_easy_setopt(c, CURLOPT_USERNAME, t.user.c_str());
        curl_easy_setopt(c, CURLOPT_PASSWORD, t.pass.c_str());
    }
    const CURLcode rc = curl_easy_perform(c);
    if (status) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? out : std::string();
}

}  // namespace

namespace {
// onvif_streams is defined below; apply reads before it writes.
}  // namespace

bool onvif_apply_resolution(const OnvifTarget& t, const std::string& token,
                            int width, int height, OnvifStream* after,
                            std::string* error, int timeout_s) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!t.ok) return fail("that is not an RTSP URL this system can read an address out of");

    // READ FIRST. The request has to carry every field the camera already has,
    // because Set REPLACES the configuration rather than merging into it, and a
    // list of what it will accept is the only way to refuse a bad size before
    // sending it.
    std::string err;
    const auto streams = onvif_streams(t, &err, timeout_s);
    if (streams.empty()) return fail(err.empty() ? "the camera did not answer" : err);

    const auto it = std::find_if(streams.begin(), streams.end(),
                                 [&](const OnvifStream& s) { return s.token == token; });
    if (it == streams.end()) return fail("that stream is no longer on the camera");

    const std::string body = onvif_set_resolution_body(*it, width, height);
    if (body.empty())
        return fail("the camera does not offer " + std::to_string(width) + "x" +
                    std::to_string(height) + " on this stream");

    long status = 0;
    const std::string reply = soap(t, body, timeout_s, &status);
    if (reply.empty()) return fail("the camera did not answer the change request");
    if (reply.find("Fault") != std::string::npos)
        return fail("the camera refused the change");

    // READ BACK, AND BELIEVE ONLY THIS.
    //
    // The success above means nothing on its own: this same camera accepts a
    // frame-rate change, answers without a fault and goes on sending the old
    // rate. Only what it reports afterwards is true, so that is what the caller
    // is given — and the caller compares it with what was asked for.
    const auto again = onvif_streams(t, nullptr, timeout_s);
    const auto now = std::find_if(again.begin(), again.end(),
                                  [&](const OnvifStream& s) { return s.token == token; });
    if (now == again.end()) return fail("the camera stopped reporting this stream");
    if (after) *after = *now;
    if (now->width != width || now->height != height)
        return fail("the camera reported success but is still set to " +
                    std::to_string(now->width) + "x" + std::to_string(now->height));
    return true;
}

std::vector<OnvifStream> onvif_streams(const OnvifTarget& t, std::string* error,
                                       int timeout_s) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::vector<OnvifStream>{};
    };
    if (!t.ok) return fail("that is not an RTSP URL this system can read an address out of");

    long status = 0;
    const std::string xml = soap(t, "<trt:GetProfiles/>", timeout_s, &status);
    if (xml.empty())
        return fail("the camera did not answer on ONVIF (port 80) within the timeout");
    if (status == 401)
        return fail("the camera refused the RTSP username and password for ONVIF");

    auto streams = onvif_parse_profiles(xml);
    if (streams.empty())
        return fail("the camera answered but listed no video profiles — it may not "
                    "speak ONVIF Media on this endpoint");

    for (auto& s : streams) {
        const std::string opts = soap(
            t, "<trt:GetVideoEncoderConfigurationOptions><trt:ConfigurationToken>" +
                   s.token + "</trt:ConfigurationToken></trt:GetVideoEncoderConfigurationOptions>",
            timeout_s, nullptr);
        onvif_parse_options(opts, s);
    }
    return streams;
}

#else

std::vector<OnvifStream> onvif_streams(const OnvifTarget&, std::string* error, int) {
    if (error) *error = "this build has no HTTP client, so cameras cannot be asked";
    return {};
}

bool onvif_apply_resolution(const OnvifTarget&, const std::string&, int, int,
                            OnvifStream*, std::string* error, int) {
    if (error) *error = "this build has no HTTP client, so cameras cannot be changed";
    return false;
}

#endif

}  // namespace so
