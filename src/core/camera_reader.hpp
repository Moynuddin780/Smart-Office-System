#pragma once

#include "core/frame.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

typedef struct _GstElement GstElement;

namespace so {

enum class DecodePath { None, HardwareRkmpp, Software };

const char* to_string(DecodePath p);

/// A camera URL with its password replaced by `***`.
///
/// Exposed rather than kept private because a regression here leaks the site's
/// camera credentials into every log shipper downstream, and that is worth a
/// test.
std::string redact_url(const std::string& url);

/// What the GStreamer autoplug callback needs to know about this reader.
struct AutoplugCtx {
    std::atomic<bool>* seen{nullptr};
    int decoder_threads{4};
};

struct CameraReaderConfig {
    std::string url;             ///< rtsp://... or a local file path
    int ring_size = 2;           ///< drop-oldest; never queue
    bool allow_software = true;

    /// How much jitter the RTSP buffer absorbs, in milliseconds.
    ///
    /// This was 100 ms, paired with drop-on-latency, and that pairing was a
    /// BUG that cost a day. An H.265 camera sends a large IDR every few
    /// seconds; over interleaved TCP that keyframe takes well over 100 ms to
    /// arrive. A 100 ms jitter buffer that is also told never to exceed its
    /// latency drops part of every keyframe, so the depayloader never assembles
    /// a complete one, so the decoder never emits a frame — silently, with no
    /// error anywhere, on a camera whose RTSP session is otherwise perfect.
    int rtsp_latency_ms = 500;

    /// Drop buffers rather than exceed the latency above.
    ///
    /// OFF by default now. It protects against a stalled network making the
    /// stream drift minutes behind real time, but it cannot tell "late because
    /// the network stalled" from "late because this keyframe is big", and
    /// getting that wrong costs every frame. Lateness is already bounded
    /// elsewhere: the ring keeps only the newest frames and the appsink is
    /// max-buffers=1 drop=true.
    bool rtsp_drop_on_latency = false;

    /// rtspsrc transport selection, e.g. "tcp", "udp", "udp+tcp".
    ///
    /// TCP by default: measured on the camera on site, UDP delivered nothing
    /// while interleaved TCP delivered 220 KB in 6 s.
    std::string rtsp_protocols = "tcp";

    /// Try Rockchip's hardware decoder FIRST. Off by default — see the class
    /// comment for the measurement that decided this.
    bool prefer_hardware = false;

    /// Cap the frame size handed to the application. Expressed as a caps
    /// RANGE, so it only ever downscales — a 640x360 sub-stream passes through
    /// untouched rather than being blown up to the cap. 0 disables it.
    ///
    /// Measured on this board, decoding to BGR:
    ///     1920x1080 native        27.3 fps
    ///     1920x1080 capped here   66.9 fps
    ///      640x360  (under cap)  235.0 fps
    /// Inference runs on a 640x640 letterbox regardless, so the cap costs
    /// nothing a detector can see; it only shrinks snapshots.
    int max_width = 960;
    int max_height = 540;

    /// The stream's own size, when it is already known. 0 = not probed yet.
    ///
    /// The hardware decoder is given an EXACT output size, not a range, so it
    /// has to be told a size the stream can actually produce. Asked to turn a
    /// 704x576 camera into 1920x1080 it does not refuse — it emits nothing at
    /// all, with no error on the bus, and the reader falls back to software
    /// twelve seconds later. Four of five cameras on this site were decoding in
    /// software for exactly that reason while the fifth, whose stream happened
    /// to be 1920x1080 already, used the hardware path perfectly.
    ///
    /// Knowing the source size is what lets the cap be applied only when there
    /// is something to cap. Unknown means ask for no scaling, which is always
    /// safe: a first-run camera decodes at its own size.
    int src_width = 0;
    int src_height = 0;

    /// Most frames per second handed downstream, 0 = every frame.
    ///
    /// This is NOT a substitute for configuring the camera, it is what is left
    /// when the camera refuses. The VARITO on site advertises a settable
    /// 5-30 fps range over ONVIF, accepts FrameRateLimit=10 with no fault, and
    /// goes on sending 25; its own web UI reverts the field identically. So the
    /// excess frames arrive whatever anyone configures.
    ///
    /// They were already being dropped — by `appsink max-buffers=1 drop=true`,
    /// at the very END of the pipeline, after videoconvert had turned each one
    /// into a full BGR frame. With a worker consuming ten of twenty-five, three
    /// frames in five were converted at 2 MP and then thrown away. Dropping
    /// them a few elements earlier, before the scale and the convert, is the
    /// same frames discarded for none of the cost.
    ///
    /// Decode itself is NOT saved: a frame has to be decoded before anything
    /// can know what it is, and inter-frame prediction means the skipped ones
    /// are needed to decode the kept ones anyway. This bounds everything after
    /// the decoder, which on this board is the larger half.
    int max_fps = 0;

    /// Threads each software decoder may use for ONE camera.
    ///
    /// This was 2, chosen when the worry was four cameras each grabbing all
    /// eight cores. The measured problem is now the opposite: a camera's decode
    /// runs on a single thread, that thread sits at 91% of one core, and six
    /// cores idle below 9% while three cameras fall behind. Work exists and
    /// hands are free; the cap is what keeps them apart.
    ///
    /// 4 leaves room for several cameras to decode at once on an 8-core board
    /// without any one of them able to starve the inference and overlay work
    /// that shares the same CPU.
    int decoder_threads = 4;

    /// How far behind real time a stream may drift before it is reopened.
    ///
    /// A camera sending faster than this board decodes does not lose the
    /// excess — it queues, and the picture falls further behind every second.
    /// Discarding the backlog in the pipeline is not available: dropping RTP
    /// packets or half-parsed frames leaves the decoder unable to assemble
    /// anything, which has gone wrong here twice. Reopening throws the backlog
    /// away with the old pipeline and resumes at the present moment, at the
    /// cost of the handshake and one keyframe.
    ///
    /// 1.5 s is the most staleness worth watching. Below about a second the
    /// reconnects would outnumber the frames saved.
    double max_behind_s = 1.5;
    int reconnect_min_ms = 1000;
    int reconnect_max_ms = 30000;
};

/**
 * One camera, decoded into BGR frames.
 *
 * WHY GSTREAMER'S C API AND NOT cv::VideoCapture
 * -----------------------------------------------
 * The plan called for `cv::VideoCapture(pipeline, cv::CAP_GSTREAMER)`, but the
 * OpenCV on this board reports `GStreamer: NO` — it was built without the
 * backend. That call would not have failed loudly; it would have quietly fallen
 * through to FFMPEG and software-decoded every frame, which is exactly the CPU
 * cost this deployment exists to avoid. Talking to GStreamer directly also
 * removes any dependence on how OpenCV happens to be compiled in the container.
 *
 * DECODE PATH — SOFTWARE BY DEFAULT, AND THAT IS NOT A MISTAKE
 * ------------------------------------------------------------
 * Measured on this board (Rock5B+, 150 frames per run, decode straight to the
 * BGR the pipeline consumes):
 *
 *     640x360 sub-stream      mppvideodec   6.7 fps   149.4 ms/frame
 *                             mpp afbc=0    6.8 fps   147.6 ms/frame
 *                             avdec_h264  223.5 fps     4.5 ms/frame
 *
 *     1280x720                avdec_h264   62.5 fps    16.0 ms/frame
 *                             avdec + scale to 640x360
 *                                         168.8 fps     5.9 ms/frame
 *
 * The hardware decoder is fast in isolation (94.6 fps decoding 1080p to
 * nothing) and collapses the moment anything reads its buffers on the CPU: its
 * output is uncached DMA memory, and a software colour conversion over that is
 * pathological. Since this application must have BGR in host memory to run
 * OpenCV and to feed the NPU, `avdec_h264` wins by 33x on the production case.
 * `prefer_hardware` keeps the other path one flag away for a board or driver
 * pairing where that is not true.
 *
 * THE REAL WIN IS SCALING EARLY. The colour conversion cost scales with output
 * pixels, so `target_width/height` puts a `videoscale` BEFORE the convert and
 * takes 720p from 16.0 ms to 5.9 ms per frame. Inference runs on a 640x640
 * letterbox regardless, so nothing is lost.
 *
 * Whichever path is chosen, the reader PROVES A FIRST FRAME before accepting
 * it: a hardware decode can fail silently — an MPP/RGA error yields zero frames
 * rather than an error — which presents as a camera sitting at 0 fps with
 * nothing in the log.
 *
 * BACKPRESSURE
 * ------------
 * The ring holds `ring_size` frames and drops the OLDEST on overflow. Queuing
 * would trade latency for completeness, which is the wrong trade here: a frame
 * from four seconds ago cannot raise a useful intrusion alert.
 */
class CameraReader {
public:
    explicit CameraReader(CameraReaderConfig cfg);
    ~CameraReader();

    CameraReader(const CameraReader&) = delete;
    CameraReader& operator=(const CameraReader&) = delete;

    /// Start the decode thread. Returns false only if GStreamer itself is broken.
    bool start();
    void stop();

    /// Most recent frame, or false if none is available within `timeout`.
    bool read(Frame& out, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

    bool online() const { return online_.load(); }
    DecodePath decode_path() const { return path_.load(); }
    double fps() const;
    int width() const { return width_.load(); }
    /// True when the frames handed over are the stream's OWN size, untouched.
    /// Only then does width()/height() describe what the camera is sending.
    bool native_size() const { return fitted_size().first == 0; }
    /// Frames per second the STREAM says it carries, 0 when it did not say.
    double stream_fps() const { return stream_fps_.load(); }
    int height() const { return height_.load(); }
    std::int64_t frames_decoded() const { return decoded_.load(); }
    std::int64_t frames_dropped() const { return dropped_.load(); }

    /// Pipeline description for a given decode path — exposed for tests.
    std::string pipeline_for(DecodePath path) const;

    /// The output size the hardware decoder should be asked for, or {0,0} for
    /// "do not scale". Aspect-preserving, and never an upscale.
    std::pair<int, int> fitted_size() const;

private:
    void run();
    bool open_path(DecodePath path, GstElement** pipeline, GstElement** sink);
    bool pump(GstElement* sink);

    /// The videorate clause, or empty when every frame is wanted.
    std::string rate_limit() const;

    CameraReaderConfig cfg_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    /// Set the moment decodebin plugs a video decoder in.
    ///
    /// This is the reader's "progress" signal, and it exists because the probe
    /// window has to serve two opposite cases. A dead URL must fail fast. A
    /// live camera may legitimately need far longer: the unit on site spends
    /// about six seconds on digest auth, SETUP and PLAY before a single byte of
    /// video moves, and then the first DECODED frame waits for an H.265
    /// keyframe. A fixed window that is short enough for the first case kills
    /// the second one just after the decoder appears — which is exactly the
    /// livelock this camera hit, reconnecting forever while working perfectly.
    std::atomic<bool> decoder_seen_{false};
    /// Handed to the autoplug callback. It used to receive a bare `bool*`,
    /// which left nowhere to say how many threads a decoder may use.
    AutoplugCtx autoplug_{};
    std::atomic<bool> online_{false};
    std::atomic<DecodePath> path_{DecodePath::None};
    std::atomic<int> width_{0}, height_{0};
    std::atomic<double> stream_fps_{0.0};
    std::atomic<std::int64_t> decoded_{0}, dropped_{0};

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Frame> ring_;
    std::int64_t seq_{0};

    mutable std::mutex fps_mtx_;
    std::chrono::steady_clock::time_point fps_t0_{};
    std::int64_t fps_base_{0};
};

}  // namespace so
