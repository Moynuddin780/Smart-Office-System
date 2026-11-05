#pragma once

#include "core/camera_reader.hpp"
#include "core/engine_pool.hpp"
#include "core/postprocess_pool.hpp"
#include "identity/face_identifier.hpp"
#include "modules/module.hpp"
#include "vision/arcface.hpp"
#include "vision/scrfd.hpp"
#include "vision/tracker.hpp"
#include "vision/yolo26_pose.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace so {

/// Everything a worker needs that is shared with the rest of the process.
struct WorkerDeps {
    EnginePool* pose{nullptr};
    EnginePool* scrfd{nullptr};      ///< may be null when no camera uses faces
    EnginePool* arcface{nullptr};    ///< may be null likewise
    PostprocessPool* pool{nullptr};
    Gallery* gallery{nullptr};
    PresenceRoster* presence{nullptr};
    EventSink* sink{nullptr};
    int jpeg_quality{75};   ///< used for the live-view encode done in the worker
};

struct WorkerStats {
    std::int64_t camera_id{0};
    std::string camera_name;
    std::int64_t frames_read{0};
    std::int64_t submitted{0};
    std::int64_t inferred{0};
    std::int64_t skipped_busy{0};
    std::int64_t faces_seen{0};
    std::int64_t faces_bound{0};    ///< faces that landed inside a person track
    std::int64_t faces_judged{0};   ///< faces that cleared the size/pose/score gates
    std::int64_t tracks_seen{0};
    /// Mean and largest face this camera has seen, in the 640 canvas the
    /// recogniser works in. Identification needs about 60 px; a camera whose
    /// faces sit well below that will identify nobody, and saying so is the
    /// difference between "face recognition is broken" and "this camera is
    /// mounted too far from the door".
    double face_px_mean{0};
    double face_px_max{0};
    std::int64_t identified{0};
    double detect_hz{0};
    bool online{false};
    std::string decode_path;

    /// What the wall shows under each tile.
    ///
    /// `decode_fps` is what this board actually gets out of the stream;
    /// `stream_fps` is what the camera says it is sending. Showing only the
    /// first would look healthy on a camera running minutes behind, and the
    /// difference between the two is exactly the condition that produces it.
    double decode_fps{0};
    double stream_fps{0};
    int width{0};
    int height{0};
    /// True when width/height are the stream's OWN size, with no resizing in
    /// the pipeline. Only then do they describe what the camera is sending.
    bool native_size{false};
};

/**
 * One camera end to end: decode, inference dispatch, postprocess, modules.
 *
 * THREADS
 * -------
 * Two per camera — the reader's decode thread and this dispatch thread — plus a
 * share of the process-wide postprocess pool. Giving each camera its own
 * postprocess thread would put eight cameras at twenty-four threads on an
 * eight-core board, which spends more on context switching than it buys.
 *
 * WHAT RUNS WHERE, AND WHY IT MATTERS
 * -----------------------------------
 * The DX-RT completion callback does ONE thing: hand the job to the postprocess
 * pool. Decoding nine tensors, running a tracker and evaluating five modules
 * inside that callback would stall the NPU job queue and serialise the very
 * three-core parallelism EnginePool exists to create.
 *
 * CADENCE
 * -------
 * Decode runs at source rate; inference does not. Detection is gated to
 * `detect_hz` and the face stage to `face_hz`, independently. A camera at 25 fps
 * therefore costs 10 inferences a second, not 25 — the remaining frames are
 * decoded (which is cheap on the hardware decoder) and dropped.
 *
 * BACKPRESSURE
 * ------------
 * A fixed set of in-flight jobs per camera. When they are all outstanding the
 * frame is skipped and counted, rather than allocating another buffer: running
 * out of jobs means the NPU is already the constraint, and queueing more work
 * for it only adds latency.
 */
class CameraWorker {
public:
    CameraWorker(CameraConfig cfg, CameraReaderConfig reader_cfg, WorkerDeps deps);
    ~CameraWorker();

    CameraWorker(const CameraWorker&) = delete;
    CameraWorker& operator=(const CameraWorker&) = delete;

    void set_modules(std::vector<ModulePtr> modules);
    void reconfigure(const CameraConfig& cfg);

    bool start();
    void stop();

    WorkerStats stats() const;
    const CameraConfig& config() const { return cfg_; }

    /// Most recent annotated frame, for the live view. Empty when nobody has
    /// asked recently — the annotation is only produced while someone watches.
    cv::Mat latest_annotated();

    /// The square region of a person's box that a face is looked for in.
    ///
    /// Public so an offline collector can crop faces exactly the way the live
    /// pipeline does. A second implementation of this geometry would drift from
    /// the one that actually feeds the recogniser, and then the crops an
    /// engineer inspects would not be the crops the system judges.
    static cv::Rect head_region(const Box& person, int frame_w, int frame_h);

    /// The annotated frame, already JPEG-encoded, and the counter that says
    /// whether it is new.
    ///
    /// Encoding lives HERE, once per produced frame, instead of once per viewer
    /// in the HTTP layer. Two reasons, both measured on this board: encoding a
    /// 720p frame costs single-digit milliseconds and five people watching one
    /// camera used to pay it five times over; and it used to happen on a Drogon
    /// event loop, where every millisecond spent is a millisecond no API request
    /// on that loop is being served.
    ///
    /// `seq` lets a reader tell a new frame from the one it already sent without
    /// comparing buffers.
    /// The newest encoded frame, with its AGE in milliseconds.
    ///
    /// The age is not a nicety. When a camera is unplugged the worker simply
    /// stops producing, and whatever it produced last stays in this buffer —
    /// so without an age the API cheerfully serves a picture of a room that
    /// nobody is watching any more, and it looks exactly like a live one.
    std::shared_ptr<const std::string> latest_jpeg(std::uint64_t* seq,
                                                   std::int64_t* age_ms = nullptr);

    /// The same frame WITHOUT any drawing on it.
    ///
    /// Enrolment captures from here, never from the annotated one: a detection
    /// box drawn over a face becomes part of the pixels the embedding is
    /// computed from, and a registry built that way holds a person plus a
    /// rectangle.
    cv::Mat latest_raw();
    /// Turn annotation + encoding on for a few seconds.
    ///
    /// A LEASE rather than a flag. With a flag, whoever switched it on had to
    /// remember to switch it off, and a viewer that simply closed its laptop
    /// left the camera drawing overlays and encoding JPEGs forever. Every
    /// request from a viewer renews the lease; when they stop asking, it lapses
    /// on its own.
    void set_live_view(bool on) {
        live_until_.store(on ? now_ms() + kLiveLeaseMs : 0);
    }
    bool live_view() const { return now_ms() < live_until_.load(); }

    /// Called by the process-wide DX-RT callback. `user_arg` is a Job*.
    static void on_inference_done(dxrt::TensorPtrs& outputs, void* user_arg);

private:
    struct Job {
        CameraWorker* owner{nullptr};
        std::vector<std::uint8_t> input;    ///< letterboxed 640x640x3 RGB
        std::vector<std::uint8_t> output;   ///< pose output buffer
        LetterboxMap map;
        cv::Mat frame;                      ///< source BGR, owned for this job
        TimePoint ts{};
        bool face_due{false};
        std::atomic<bool> busy{false};
    };

    void dispatch_loop();
    Job* acquire_job();
    void release_job(Job* j);
    void postprocess(Job* j, std::vector<PoseDetection> dets);



    /// How many people get a face crop per cycle.
    ///
    /// Each one costs an SCRFD inference, so this is the knob that keeps the
    /// two-stage search from scaling with the size of a crowd. Three covers a
    /// doorway; anybody beyond that is picked up on the next cycle, and people
    /// nobody has identified yet are always chosen first.
    /// People given a face crop per face cycle.
    ///
    /// Was 3, which starved groups: three people walking in together filled it
    /// exactly, and anybody already in the room — at a desk, in the corridor —
    /// pushed one of them out. Measured on this board the NPU sits at ~9% with
    /// the budget at 3, so this limit was never the hardware's.
    static constexpr std::size_t kMaxFaceCrops = 6;
    std::vector<FaceObs> run_face_stage(Job* j, const std::vector<Track>& tracks);

    CameraConfig cfg_;
    WorkerDeps deps_;
    CameraReader reader_;

    Yolo26PoseDecoder pose_decoder_;
    ScrfdDecoder scrfd_decoder_;
    std::unique_ptr<ArcFace> arcface_;
    std::unique_ptr<FaceIdentifier> identifier_;
    Tracker tracker_;

    std::vector<std::unique_ptr<Job>> jobs_;
    std::vector<std::uint8_t> scrfd_out_;
    /// Input buffer for the per-person face crop, sized on first use.
    std::vector<std::uint8_t> face_input_;

    std::vector<ModulePtr> modules_;
    mutable std::mutex modules_mtx_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    static constexpr std::int64_t kLiveLeaseMs = 4000;
    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::atomic<std::int64_t> live_until_{0};

    mutable std::mutex annotated_mtx_;
    cv::Mat annotated_;
    cv::Mat raw_;
    std::shared_ptr<const std::string> annotated_jpeg_;
    std::uint64_t annotated_seq_{0};
    std::int64_t annotated_at_{0};   ///< now_ms() when it was encoded
    int jpeg_quality_{75};

    std::atomic<std::int64_t> frames_read_{0}, submitted_{0}, inferred_{0};
    std::atomic<std::int64_t> skipped_busy_{0}, faces_seen_{0}, identified_{0};
    std::atomic<std::int64_t> faces_bound_{0}, faces_judged_{0}, tracks_seen_{0};
    std::atomic<double> face_px_sum_{0.0};
    std::atomic<double> face_px_max_{0.0};
    std::chrono::steady_clock::time_point started_{};
};

}  // namespace so
