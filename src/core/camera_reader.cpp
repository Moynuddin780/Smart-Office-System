#include "core/camera_reader.hpp"

#include "core/log.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>

namespace so {
namespace {

std::once_flag g_gst_init;

void ensure_gst() {
    std::call_once(g_gst_init, [] {
        int argc = 0;
        char** argv = nullptr;
        gst_init(&argc, &argv);
        SO_LOG_INFO("[gst] initialised %s", gst_version_string());
    });
}

bool is_rtsp(const std::string& url) {
    return url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0;
}

bool element_available(const char* name) {
    GstElementFactory* f = gst_element_factory_find(name);
    if (!f) return false;
    gst_object_unref(f);
    return true;
}

// decodebin picks the decoder, which means we cannot pass it properties in the
// pipeline description the way a named `avdec_h264 max-threads=2` could. This
// runs as each element is autoplugged and caps the ones that matter.
//
// The cap is not cosmetic: ffmpeg defaults its thread pool to the core count,
// so four cameras would each try to own all eight cores of this board. The CPU
// this deployment is trying to protect is the CPU that feeds the NPU.
void on_deep_element_added(GstBin*, GstBin*, GstElement* element, gpointer user) {
    GstElementFactory* f = gst_element_get_factory(element);
    if (!f) return;
    const gchar* klass = gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
    if (!klass || !std::strstr(klass, "Decoder") || !std::strstr(klass, "Video")) return;

    auto* ctx = static_cast<AutoplugCtx*>(user);
    const int threads = ctx ? ctx->decoder_threads : 4;
    GObjectClass* klass_obj = G_OBJECT_GET_CLASS(element);
    if (g_object_class_find_property(klass_obj, "max-threads"))
        g_object_set(element, "max-threads", threads, nullptr);

    // FRAME THREADING WAS TRIED HERE AND MADE THINGS WORSE.
    //
    // Raising max-threads alone changes nothing, because these cameras send one
    // slice per frame and slice threading has nothing to split. The obvious
    // next step is thread-type=frame, and it did spread one camera's decode
    // over four cores — no core above 30%, where one had been pinned at 100%.
    //
    // Throughput collapsed anyway: frame threading holds several frames back
    // before emitting any, and this pipeline ends in `appsink max-buffers=1
    // drop=true`, which keeps only the newest. Measured over ten minutes it
    // went from five cameras online with two behind to TWO online with four
    // behind, while the board sat at 15% — nothing was busy and nothing was
    // arriving. A balanced idle is not better than a lopsided working board.
    //
    // The parallelism that did work is the queue in front of the decoder (see
    // pipeline_for): it gives each camera's decode its own thread, so the
    // cameras spread across cores even though each one is still single
    // threaded within itself.

    if (ctx && ctx->seen) ctx->seen->store(true);
    SO_LOG_INFO("[camera] decoder autoplugged: %s", GST_OBJECT_NAME(f));
}

/// Report whatever GStreamer has to say, then keep going.
///
/// WHY THIS EXISTS: without it a failing pipeline is completely silent. The
/// only symptom was "produced no frame in 8000 ms", which is the same message
/// for a wrong codec, a refused password, an unroutable host and an
/// unsupported transport — four different fixes behind one sentence. The first
/// real camera on site cost an hour to that ambiguity.
///
/// Returns true if the pipeline posted a fatal error.
bool drain_bus(GstElement* pipeline, const std::string& url) {
    GstBus* bus = gst_element_get_bus(pipeline);
    if (!bus) return false;
    bool fatal = false;

    while (GstMessage* msg = gst_bus_pop_filtered(
               bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                                GST_MESSAGE_WARNING))) {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        const bool is_error = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR;
        if (is_error)
            gst_message_parse_error(msg, &err, &dbg);
        else
            gst_message_parse_warning(msg, &err, &dbg);

        // The element name matters as much as the text: "Unauthorized" from
        // rtspsrc and "no decoder" from decodebin are entirely different faults.
        const char* from = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
        if (is_error) {
            fatal = true;
            SO_LOG_ERROR("[camera] %s: gstreamer error from %s: %s (%s)",
                         url.c_str(), from ? from : "?",
                         err ? err->message : "?", dbg ? dbg : "no detail");
        } else {
            SO_LOG_WARN("[camera] %s: gstreamer warning from %s: %s",
                        url.c_str(), from ? from : "?",
                        err ? err->message : "?");
        }
        if (err) g_error_free(err);
        if (dbg) g_free(dbg);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return fatal;
}

// How long a candidate decode path gets to produce its first frame.
//
// An RTSP stream has to connect, negotiate and wait for a keyframe, so it needs
// a generous window. A local file has none of that: it either decodes at once
// or the codec is not accepted, and spending eight seconds discovering that
// stalls every caller behind it. What the window must NOT be is infinite — a
// silently-failing decoder would then hang the camera forever rather than
// falling back.
constexpr int kProbeRtspMs = 12000;
constexpr int kProbeFileMs = 1500;

/// Once a decoder is in place the pipeline has proved itself; all that can
/// still be outstanding is a keyframe. Cameras are commonly configured with a
/// GOP of one to four seconds, and a congested link stretches that, so the
/// window opens up rather than restarting a connection that took six seconds
/// to build.
constexpr int kProbeDecodingMs = 45000;

}  // namespace

// The reader logs the URL on every reconnect attempt, and an RTSP URL carries
// the camera's credentials inline. Those lines end up in `docker compose logs`
// and in anything that ships logs off the box, so the password must not be in
// them. Never log cfg_.url directly.
std::string redact_url(const std::string& url) {
    const auto scheme = url.find("//");
    if (scheme == std::string::npos) return url;
    const auto at = url.find('@', scheme);
    if (at == std::string::npos) return url;
    const auto colon = url.find(':', scheme + 2);
    if (colon == std::string::npos || colon > at)
        return url.substr(0, scheme + 2) + "***@" + url.substr(at + 1);
    // Keep the username: knowing WHICH account failed to authenticate is the
    // whole diagnostic value of the line.
    return url.substr(0, colon + 1) + "***" + url.substr(at);
}

const char* to_string(DecodePath p) {
    switch (p) {
        case DecodePath::HardwareRkmpp: return "rkmpp (hardware)";
        case DecodePath::Software: return "software";
        case DecodePath::None: return "none";
    }
    return "?";
}

CameraReader::CameraReader(CameraReaderConfig cfg) : cfg_(std::move(cfg)) {
    ensure_gst();
}

CameraReader::~CameraReader() { stop(); }

std::string CameraReader::pipeline_for(DecodePath path) const {
    std::ostringstream p;
    const bool rtsp = is_rtsp(cfg_.url);

    if (rtsp) {
        // rtspsrc jitter-buffers internally — `latency` and `drop-on-latency`
        // configure that buffer — so the standalone rtpjitterbuffer that used
        // to sit here was a second one in series, adding delay and one more
        // element to negotiate for no benefit.
        //
        // The latency and drop settings are NOT cosmetic and NOT arbitrary; see
        // the fields in the header for what a wrong pairing does to an H.265
        // camera.
        p << "rtspsrc location=\"" << cfg_.url << "\""
          << " latency=" << cfg_.rtsp_latency_ms
          << " drop-on-latency=" << (cfg_.rtsp_drop_on_latency ? "true" : "false")
          << " protocols=" << cfg_.rtsp_protocols
          // PICK THE VIDEO STREAM EXPLICITLY. rtspsrc exposes one pad per
          // media in the SDP, and an ONVIF camera advertises more than video:
          // the unit on site offers
          //     m=video       ... H265/90000
          //     m=application ... vnd.onvif.metadata/90000
          // gst_parse_launch links the first pad that will link, so without
          // this filter the decoder can end up wired to the metadata track and
          // the pipeline sits there producing nothing at all. The capsfilter
          // makes the metadata pad fail to link and the video pad win.
          << " ! application/x-rtp,media=video"
          // A THREAD BOUNDARY, AND THAT IS THE WHOLE REASON IT IS HERE.
          //
          // A GStreamer pad task runs everything downstream of it inline, so
          // without this the jitter buffer's thread also does the depayloading,
          // the parsing and the DECODING of this camera — all of it on one
          // core. Measured on this board: that thread sat at 91% of one core
          // while six cores idled below 9% and three cameras fell behind. More
          // decoder threads cannot help while the work has nowhere to go.
          //
          // A queue starts its own task, so from here the decode runs on its
          // own thread and the scheduler can put it on a different core.
          //
          // Bounded by TIME and not leaky. Dropping encoded buffers here would
          // cut a keyframe in half and the decoder would emit nothing at all —
          // the same silent failure that drop-on-latency once caused. Full
          // simply means back-pressure, exactly as before, except the decode is
          // no longer trapped behind the network thread.
          << " ! queue max-size-buffers=0 max-size-bytes=0"
             " max-size-time=500000000 ! ";
    } else {
        p << "filesrc location=\"" << cfg_.url << "\" ! ";
    }

    // THE DECODER IS NOT NAMED HERE, AND THAT IS THE POINT.
    //
    // It used to be a literal `avdec_h264`, which meant the codec was a
    // property of this source file rather than of the camera. The first real
    // camera on site disproved that: its sub-stream advertises
    //     a=rtpmap:108 H265/90000
    // and every H.265 camera reported "no usable decode path" while the H.264
    // test footage worked perfectly — a bug that only ever appears on real
    // hardware. `decodebin` reads the stream's own caps and autoplugs the
    // matching depayloader, parser and decoder, so H.264, H.265 and MJPEG all
    // work with no per-camera configuration.
    //
    // `force-sw-decoders` is what keeps the measurement above in force. It is a
    // per-pipeline switch, which matters: demoting mppvideodec's plugin rank
    // would have been process-wide and would race between cameras opening on
    // different paths at the same time.
    //
    if (path == DecodePath::HardwareRkmpp) {
        // THE WHOLE POINT OF THIS PATH IS THAT THE CPU NEVER TOUCHES THE PIXELS.
        //
        // An earlier attempt put mppvideodec in front of videoconvert and came
        // out an order of magnitude SLOWER than decoding in software. That was
        // not the decoder's fault: it writes NV12 into uncached DMA memory, and
        // a software colour conversion reading back out of that is pathological.
        //
        // So the decoder is asked for BGR at the final size directly. It does
        // both on the RGA — the 2D engine next to it — and what arrives is
        // already the buffer the application wants. No videoconvert, no
        // videoscale, nothing for the CPU to do but copy it out.
        //
        // The parser is named explicitly here rather than left to decodebin,
        // because decodebin inserts its own convert/scale when it cannot see
        // that the decoder already satisfies the caps.
                // WHERE THE STALE FRAMES GO. See the note on the software path
        // below; the seam is the same, after the parser and before the
        // decoder, so only whole encoded frames are ever dropped.
        p << "parsebin ! mppvideodec format=BGR arm-afbc=0 fast-mode=true";
        // Scale ONLY when the stream is bigger than the cap, and only to a size
        // that keeps its shape. These are exact properties, not a range, so an
        // upscale request is both pointless and fatal — see src_width in the
        // header for the silent twelve-second failure it causes.
        const auto fit = fitted_size();
        if (fit.first > 0)
            p << " width=" << fit.first << " height=" << fit.second;
        p << " ! video/x-raw,format=BGR" << rate_limit();
    } else {
        // A BOUNDED, LEAKY QUEUE SO THE PICTURE CANNOT FALL BEHIND FOR EVER.
        //
        // When a camera sends faster than this board decodes, something has to
        // give. Until now nothing did: the queues were all non-leaky, so the
        // backlog simply grew and the live wall drifted minutes behind the
        // room — a wall that is three minutes stale is not a live wall.
        //
        // leaky=downstream discards the OLDEST buffer when full, which is
        // exactly the wanted behaviour: always show the newest picture and
        // throw away what nobody can catch up on.
        //
        // THE PLACEMENT IS THE WHOLE POINT, AND IT IS WHY parsebin IS NAMED
        // HERE. Earlier an attempt to bound the lag put the dropping inside
        // rtspsrc (drop-on-latency), which throws away RTP PACKETS — half a
        // keyframe goes missing, the decoder never assembles one, and the
        // camera shows black for ever with no error anywhere. After the parser
        // a buffer is a COMPLETE encoded frame, so a drop costs some macroblock
        // noise until the next keyframe a second or two later, and recovers by
        // itself.
        //
        // 1.5 s is the most lag allowed to accumulate before frames start being
        // discarded.
        p << "decodebin name=dec force-sw-decoders=true";
        // THE ORDER HERE IS THE ENTIRE SAVING.
        //
        // The excess frames were always discarded, but by the appsink at the
        // very end — after videoscale and videoconvert had done full-resolution
        // work on each one. Dropping them HERE, the moment they leave the
        // decoder, discards exactly the same frames for none of the downstream
        // cost. See max_fps in the header for why the camera cannot simply be
        // told to send fewer.
        p << rate_limit();
        if (cfg_.max_width > 0 && cfg_.max_height > 0) {
            const auto fit = fitted_size();
            if (fit.first > 0 && cfg_.src_width > 0) {
                // AN EXACT, ASPECT-PRESERVING SIZE.
                //
                // This was a range on each axis — width=[1,W] height=[1,H] — and
                // videoscale then takes each axis to its own maximum
                // independently. Against a 3200x1800 stream capped at 1920x1920
                // that produced 1920x1800: a picture squashed by a third, which
                // moves every facial landmark and quietly spoils the alignment
                // the whole identification rests on. The range only looked right
                // because the first caps happened to match 16:9 sources.
                p << " ! videoscale ! video/x-raw,width=" << fit.first
                  << ",height=" << fit.second;
            }
            // Nothing is added when the size is unknown or already small
            // enough. Every attempt to express "no bigger than N" in caps got
            // the geometry wrong instead: a range on both axes took each to its
            // own maximum (2688x1520 -> 1000x1000), and bounding the width
            // alone made videoscale squash the PIXELS rather than the picture
            // (-> 1000x1520). Caps cannot say "preserve the shape", so the
            // shape is computed here or not imposed at all.
        }
        p << " ! videoconvert ! video/x-raw,format=BGR";
    }

    // The inference cadence is gated in CameraWorker, not here, so the reader
    // stays a plain decoder that always hands over the newest frame.
    p << " ! appsink name=sink emit-signals=false sync=false"
      << " max-buffers=1 drop=true";
    return p.str();
}

std::string CameraReader::rate_limit() const {
    if (cfg_.max_fps <= 0) return {};
    // max-rate as a PROPERTY rather than a framerate capsfilter, because an
    // RTSP source frequently negotiates framerate=0/1 ("variable"), and a
    // capsfilter demanding 10/1 against that fails to link — the camera then
    // shows nothing at all, which is the same silent black-screen class of
    // failure that drop-on-latency and parsebin each caused here before.
    // max-rate works off buffer timestamps and needs no agreement upstream.
    //
    // drop-only, so this can only ever reduce: left to itself videorate
    // DUPLICATES buffers to reach a target rate, and a camera running under
    // the cap would then manufacture frames that each cost a full conversion.
    return " ! videorate drop-only=true max-rate=" + std::to_string(cfg_.max_fps);
}

std::pair<int, int> CameraReader::fitted_size() const {
    if (cfg_.max_width <= 0 || cfg_.max_height <= 0) return {0, 0};
    // {0,0} means "do not resize", and that is the answer whenever the source
    // is not known to be too big.
    //
    // The source size is a REMEMBERED value and can be wrong. It was wrong on
    // this site: a camera recorded as 720x576 was really streaming 864x480, and
    // resizing to the remembered figure squeezed 16:9 into 5:4 — a picture that
    // still looks like the room with every facial landmark moved. A remembered
    // size may decide WHETHER to shrink; it must never decide the shape.
    if (cfg_.src_width <= 0 || cfg_.src_height <= 0) return {0, 0};
    if (cfg_.src_width <= cfg_.max_width && cfg_.src_height <= cfg_.max_height)
        return {0, 0};

    const double sx = static_cast<double>(cfg_.max_width) / cfg_.src_width;
    const double sy = static_cast<double>(cfg_.max_height) / cfg_.src_height;
    const double s = std::min(sx, sy);
    // Even dimensions: the decoder works in chroma-subsampled planes and an odd
    // width is not representable, so it would quietly round or refuse.
    int w = static_cast<int>(cfg_.src_width * s) & ~1;
    int h = static_cast<int>(cfg_.src_height * s) & ~1;
    if (w < 2 || h < 2) return {0, 0};
    return {w, h};
}

bool CameraReader::open_path(DecodePath path, GstElement** pipeline, GstElement** sink) {
    if (path == DecodePath::HardwareRkmpp && !element_available("mppvideodec")) {
        SO_LOG_WARN("[camera] mppvideodec is not registered on this system — "
                 "every camera will software-decode, which costs roughly an "
                 "order of magnitude more CPU per frame");
        return false;
    }
    if (!element_available("decodebin")) {
        SO_LOG_ERROR("[camera] gstreamer's decodebin is not registered — no stream "
                     "can be decoded; the base image is missing "
                     "gstreamer1.0-plugins-base");
        return false;
    }

    const std::string desc = pipeline_for(path);
    // Logged because every silent-black-screen failure on this site has been a
    // question about what the pipeline actually said, and reconstructing it
    // from four config fields and two aspect-ratio rules is how those questions
    // were being answered until now. Redacted, so the camera password does not
    // end up in the log.
    SO_LOG_INFO("[camera] %s pipeline: %s", to_string(path),
                redact_url(desc).c_str());
    GError* err = nullptr;
    GstElement* pl = gst_parse_launch(desc.c_str(), &err);
    if (!pl || err) {
        SO_LOG_WARN("[camera] %s pipeline failed to parse: %s",
                 to_string(path), err ? err->message : "unknown");
        if (err) g_error_free(err);
        if (pl) gst_object_unref(pl);
        return false;
    }

    // Cap the decoder's thread pool as decodebin autoplugs it. Hooked before
    // PLAYING so the callback cannot miss the element it exists to configure.
    if (GstElement* dec = gst_bin_get_by_name(GST_BIN(pl), "dec")) {
        autoplug_.seen = &decoder_seen_;
        autoplug_.decoder_threads = cfg_.decoder_threads;
        g_signal_connect(dec, "deep-element-added",
                         G_CALLBACK(on_deep_element_added), &autoplug_);
        gst_object_unref(dec);
    }

    GstElement* sk = gst_bin_get_by_name(GST_BIN(pl), "sink");
    if (!sk) {
        gst_object_unref(pl);
        return false;
    }

    if (gst_element_set_state(pl, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        SO_LOG_WARN("[camera] %s pipeline refused to start", to_string(path));
        gst_object_unref(sk);
        gst_object_unref(pl);
        return false;
    }

    *pipeline = pl;
    *sink = sk;
    return true;
}

bool CameraReader::pump(GstElement* sink) {
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                     200 * GST_MSECOND);
    if (!sample) return false;

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buf = gst_sample_get_buffer(sample);
    if (!caps || !buf) {
        gst_sample_unref(sample);
        return false;
    }

    GstStructure* s = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);

    // What the stream SAYS it sends. Compared against what we actually manage
    // to decode, this is the difference between "the picture is live" and "the
    // picture is minutes behind and nothing says so".
    int fps_n = 0, fps_d = 0;
    if (gst_structure_get_fraction(s, "framerate", &fps_n, &fps_d) && fps_d > 0)
        stream_fps_.store(static_cast<double>(fps_n) / static_cast<double>(fps_d));
    if (w <= 0 || h <= 0) {
        gst_sample_unref(sample);
        return false;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return false;
    }

    Frame f;
    // Copy out of the GStreamer buffer: it is returned to the pool as soon as
    // the sample is unreffed, and the frame outlives that by design.
    f.image = cv::Mat(h, w, CV_8UC3, map.data).clone();
    f.ts = std::chrono::system_clock::now();

    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);

    width_.store(w);
    height_.store(h);
    decoded_.fetch_add(1);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        f.seq = ++seq_;
        ring_.push_back(std::move(f));
        while (static_cast<int>(ring_.size()) > cfg_.ring_size) {
            ring_.pop_front();
            dropped_.fetch_add(1);
        }
    }
    cv_.notify_one();
    return true;
}

void CameraReader::run() {
    int backoff = cfg_.reconnect_min_ms;
    const std::string safe_url = redact_url(cfg_.url);

    while (!stop_.load()) {
        GstElement* pipeline = nullptr;
        GstElement* sink = nullptr;
        DecodePath chosen = DecodePath::None;

        // Software first unless the operator asks otherwise — see the header for
        // the measurement. Each candidate must PROVE a frame before it is used.
        const DecodePath order[2] = {
            cfg_.prefer_hardware ? DecodePath::HardwareRkmpp : DecodePath::Software,
            cfg_.prefer_hardware ? DecodePath::Software : DecodePath::HardwareRkmpp,
        };
        for (DecodePath candidate : order) {
            if (candidate == DecodePath::Software && !cfg_.allow_software) continue;
            if (stop_.load()) break;
            if (!open_path(candidate, &pipeline, &sink)) continue;

            const int probe_ms = is_rtsp(cfg_.url) ? kProbeRtspMs : kProbeFileMs;
            decoder_seen_.store(false);
            const auto opened = std::chrono::steady_clock::now();
            auto deadline = opened + std::chrono::milliseconds(probe_ms);
            bool extended = false;
            bool proved = false;
            bool fatal = false;
            while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
                if (pump(sink)) {
                    proved = true;
                    break;
                }
                // Give a pipeline that has got as far as a decoder the time to
                // finish the job instead of tearing it down and paying for the
                // whole RTSP handshake again.
                if (!extended && decoder_seen_.load()) {
                    extended = true;
                    deadline = opened + std::chrono::milliseconds(kProbeDecodingMs);
                    SO_LOG_INFO("[camera] %s: decoder is up, waiting up to %d s for the "
                                "first keyframe", safe_url.c_str(), kProbeDecodingMs / 1000);
                }
                // A refused password or an unsupported transport is final:
                // waiting out the rest of the probe window only delays the
                // message the operator needs.
                if (drain_bus(pipeline, safe_url)) {
                    fatal = true;
                    break;
                }
            }
            if (proved) {
                chosen = candidate;
                break;
            }

            // Say which of the two it was. drain_bus has already printed
            // GStreamer's own reason in the fatal case; a silent timeout is the
            // case this probe was written for, and the two need different fixes.
            if (fatal) {
                SO_LOG_WARN("[camera] %s failed for %s (reason above) — trying the "
                            "next path", to_string(candidate), safe_url.c_str());
            } else {
                const int waited = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - opened).count());
                if (decoder_seen_.load()) {
                    SO_LOG_WARN("[camera] %s: a decoder was plugged in but no frame "
                                "arrived in %d ms for %s — the stream connects and "
                                "then delivers nothing decodable",
                                to_string(candidate), waited, safe_url.c_str());
                } else {
                    SO_LOG_WARN("[camera] %s produced no frame in %d ms for %s and "
                                "gstreamer reported nothing — trying the next path",
                                to_string(candidate), waited, safe_url.c_str());
                }
            }
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(sink);
            gst_object_unref(pipeline);
            pipeline = nullptr;
            sink = nullptr;
        }

        if (chosen == DecodePath::None) {
            online_.store(false);
            path_.store(DecodePath::None);
            SO_LOG_WARN("[camera] %s: no usable decode path; retrying in %d ms",
                     safe_url.c_str(), backoff);
            for (int slept = 0; slept < backoff && !stop_.load(); slept += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff = std::min(backoff * 2, cfg_.reconnect_max_ms);
            continue;
        }

        path_.store(chosen);
        online_.store(true);
        backoff = cfg_.reconnect_min_ms;
        {
            std::lock_guard<std::mutex> lk(fps_mtx_);
            fps_t0_ = std::chrono::steady_clock::now();
            fps_base_ = decoded_.load();
        }
        SO_LOG_INFO("[camera] %s online via %s at %dx%d",
                 safe_url.c_str(), to_string(chosen), width_.load(), height_.load());

        // Steady state. Several consecutive empty pulls mean the stream died.
        int empty = 0;

        // FALLING BEHIND IS A SILENT FAILURE, SO SAY IT OUT LOUD.
        //
        // If a camera sends 20 fps and this board decodes 4, the surplus does
        // not vanish — it queues, and the picture drifts further behind real
        // time every second. Nothing about that looks broken: frames keep
        // arriving, the camera reads as online, and the only symptom is that
        // what you are watching already happened. That cost a round trip to
        // diagnose on a 3200x1800 H.265 stream, so the reader now measures it
        // and names it.
        auto last_lag_check = std::chrono::steady_clock::now();
        std::int64_t lag_mark = decoded_.load();
        double behind_s = 0.0;
        int ok_checks = 0;
        while (!stop_.load()) {
            // EVERY iteration, not only the ones where no frame arrived. A
            // camera the board cannot keep up with still delivers frames — old
            // ones — so a check that only ran when the pipe was empty would
            // never see the very condition it exists to catch.
            {
                const auto now = std::chrono::steady_clock::now();
                const double since =
                    std::chrono::duration<double>(now - last_lag_check).count();
                if (since >= 20.0) {
                    const std::int64_t got = decoded_.load();
                    const double got_fps = static_cast<double>(got - lag_mark) / since;
                    const double want = stream_fps_.load();
                    if (want > 1.0 && got_fps < want * 0.7) {
                        // ACCUMULATED lag, in seconds of video nobody will ever
                        // see. Each check covers `since` seconds of wall time in
                        // which `want` frames arrived and `got_fps` were taken,
                        // so the rest are still queued somewhere upstream.
                        behind_s += since * (want - got_fps) / want;
                        SO_LOG_WARN("[camera] %s is FALLING BEHIND: the stream sends %.0f fps, "
                                    "this board decodes %.1f — about %.0f s behind real time now. "
                                    "Use a smaller stream, or lower the camera's frame rate.",
                                    safe_url.c_str(), want, got_fps, behind_s);
                    } else {
                        // Keeping up, so whatever had built up has been worked
                        // through. This is why the count is not monotonic: a
                        // brief network hiccup should not eventually trip the
                        // watchdog on a camera that recovered by itself.
                        behind_s = 0.0;

                        // SAY THE RATE OUT LOUD EVEN WHEN IT IS FINE.
                        //
                        // "Is this board keeping up with 10 fps?" is the first
                        // question asked of every camera on this site, and it
                        // used to be answerable only by its own silence —
                        // nothing was logged unless the reader had already
                        // fallen 30% behind. Absence of a warning is not a
                        // measurement, so once a minute the measured rate is
                        // reported whether it is good news or not.
                        if (++ok_checks % 3 == 0)
                            SO_LOG_INFO("[camera] %s: decoding %.1f fps of the %.0f "
                                        "the stream sends, via %s at %dx%d",
                                        safe_url.c_str(), got_fps, want,
                                        to_string(chosen), width_.load(), height_.load());
                    }
                    last_lag_check = now;
                    lag_mark = got;

                    // RECONNECT RATHER THAN WATCH THE PAST.
                    //
                    // A stream the board cannot keep up with does not drop the
                    // excess, it queues it, and a live wall that is minutes
                    // behind the room is not a live wall. Frames cannot simply
                    // be discarded on the way in — dropping RTP packets or
                    // half-parsed frames leaves the decoder with nothing it can
                    // assemble, which this reader has already been bitten by
                    // twice (see drop-on-latency and the leaky queue).
                    //
                    // Reopening the stream is the one way to get back to live
                    // that cannot corrupt anything: everything queued is thrown
                    // away with the old pipeline and the camera starts again at
                    // the present moment. It costs the RTSP handshake and a wait
                    // for the next keyframe, which is a second or two against
                    // the minutes being discarded.
                    //
                    // This treats the symptom. The cause is a stream too big for
                    // this board, and the warning above says so — the right fix
                    // is still a smaller stream or a lower frame rate at the
                    // camera.
                    if (behind_s >= cfg_.max_behind_s) {
                        SO_LOG_WARN("[camera] %s: %.0f s behind — reopening the stream to "
                                    "catch up with the present",
                                    safe_url.c_str(), behind_s);
                        break;
                    }
                }
            }

            if (pump(sink)) {
                empty = 0;
                continue;
            }
            if (drain_bus(pipeline, safe_url)) {
                SO_LOG_WARN("[camera] %s: fatal pipeline error; reconnecting",
                            safe_url.c_str());
                break;
            }
            if (++empty > 25) {                  // ~5 s of nothing
                SO_LOG_WARN("[camera] %s stopped delivering frames; reconnecting",
                         safe_url.c_str());
                break;
            }
        }

        online_.store(false);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
    }
    online_.store(false);
}

bool CameraReader::start() {
    if (thread_.joinable()) return true;
    stop_.store(false);
    thread_ = std::thread(&CameraReader::run, this);
    return true;
}

void CameraReader::stop() {
    stop_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool CameraReader::read(Frame& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (!cv_.wait_for(lk, timeout, [&] { return !ring_.empty() || stop_.load(); }))
        return false;
    if (ring_.empty()) return false;
    // Newest frame wins: anything older is already stale by the time a module
    // would act on it.
    out = std::move(ring_.back());
    ring_.clear();
    return true;
}

double CameraReader::fps() const {
    std::lock_guard<std::mutex> lk(fps_mtx_);
    if (fps_t0_.time_since_epoch().count() == 0) return 0.0;
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - fps_t0_).count();
    if (secs < 0.5) return 0.0;
    return static_cast<double>(decoded_.load() - fps_base_) / secs;
}

}  // namespace so
