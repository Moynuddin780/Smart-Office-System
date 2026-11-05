#pragma once

#include "core/camera_worker.hpp"
#include "core/config.hpp"
#include "core/engine_pool.hpp"
#include "core/postprocess_pool.hpp"
#include "db/repos.hpp"
#include "identity/gallery.hpp"
#include "identity/presence.hpp"
#include "notify/outbox.hpp"
#include "notify/snapshot.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace so::app {

/// A live event, for the WebSocket feed. Kept separate from the database row so
/// the feed does not wait on a write.
struct LiveEvent {
    std::int64_t tenant_id{0};
    std::string json;
};

/**
 * The running system: engine pools, cameras, modules, and the bridge to the
 * database.
 *
 * ONE set of engine pools serves every tenant. That is the whole point of the
 * appliance — three models resident once per NPU core, shared. Tenancy is
 * enforced above this, on the data, not by giving each client their own copy of
 * a model the board could not hold three of.
 */
class Engine {
public:
    Engine(const Config& cfg, db::Repos& repos);
    ~Engine();

    /// Load models, start the cameras that are enabled. Throws on a missing
    /// model or an unreachable NPU rather than starting half-alive: the one
    /// thing worse than not starting is starting and silently detecting nothing.
    void start();
    void stop();

    /// Re-read cameras, zones, lines and modules from the database and apply
    /// them to the running workers, starting and stopping cameras as needed.
    void reload();

    /// Re-read every enrolled face. Cheap and safe to call after an enrolment:
    /// the gallery swaps in place, so cameras already running pick it up on
    /// their next frame without a restart.
    void reload_gallery();

    /// Most recent annotated frame for a camera, or empty.
    cv::Mat annotated(std::int64_t camera_id);
    /// The same frame with nothing drawn on it — what enrolment captures.
    cv::Mat raw_frame(std::int64_t camera_id);

    /// The latest annotated frame already encoded as JPEG, plus a counter that
    /// increments per frame. Encoding happens once per camera in the worker, so
    /// N viewers of one camera cost one encode, not N — and none of it happens
    /// on an HTTP thread.
    std::shared_ptr<const std::string> annotated_jpeg(std::int64_t camera_id,
                                                     std::uint64_t* seq,
                                                     std::int64_t* age_ms = nullptr);
    void set_live_view(std::int64_t camera_id, bool on);

    /// Subscribe to live events. The callback runs on a postprocess thread, so
    /// it must not block.
    void on_live_event(std::function<void(const LiveEvent&)> cb);

    /// One enrolment photo, judged.
    struct EnrolResult {
        bool ok{false};
        std::string reason;       ///< why it was refused, in words an operator can act on
        std::vector<float> embedding;
        float yaw{0}, face_px{0}, score{0};
        int quality{0};           ///< 0..100, from pose and face size
        /// Faces in the frame other than the one enrolled. A guided capture
        /// uses this to say "only one person in frame, please" rather than
        /// silently enrolling whoever happened to be largest.
        int other_faces{0};
        /// The aligned 112x112 crop actually embedded. Worth keeping so a
        /// registry can show what the recogniser saw, not what the camera did.
        cv::Mat aligned;
    };

    /**
     * Turn one uploaded photo into an embedding, or say why not.
     *
     * The refusal reason matters as much as the embedding. An enrolment UI that
     * silently accepts a photo the recogniser will never match from is how a
     * deployment ends up with an employee who is "enrolled" and never
     * recognised, with nothing anywhere saying why.
     */
    EnrolResult enrol(const cv::Mat& image_bgr);

    struct Status {
        bool npu_ok{false};
        int engines{0};
        int cameras_running{0};
        int cameras_online{0};
        std::vector<double> core_utilisation;
        std::vector<int> core_temperature;
        std::int64_t gallery_version{0};
        int gallery_count{0};
    };
    Status status();

    std::vector<WorkerStats> worker_stats();

private:
    class DbSink;

    void desk_flush_loop();
    /// Write each camera's REAL stream size back to the database.
    void sync_probed_sizes();
    /// Refresh the roster of who has been recorded arriving today.
    void refresh_presence();
    /// Re-read today's unfinished days so working time keeps moving for people
    /// the cameras are not currently looking at. See the definition.
    void refresh_open_days();
    /// Often enough that the figure on screen is never far out, rarely enough
    /// that it costs nothing: a handful of queries per employee still inside.
    static constexpr std::chrono::minutes kDayRefreshInterval{5};
    std::chrono::steady_clock::time_point last_day_refresh_{};

    Config cfg_;
    db::Repos& repos_;

    std::unique_ptr<EnginePool> pose_, scrfd_, arcface_;
    std::unique_ptr<PostprocessPool> pool_;
    std::unique_ptr<Gallery> gallery_;
    std::unique_ptr<PresenceRoster> presence_;
    std::unique_ptr<notify::SnapshotStore> snapshots_;
    std::unique_ptr<notify::OutboxWorker> outbox_;
    std::unique_ptr<DbSink> sink_;

    std::map<std::int64_t, std::shared_ptr<CameraWorker>> workers_;

    /// What each running worker's READER was built from.
    ///
    /// reload() can hand a live worker new zones, but it cannot hand it a new
    /// stream: the URL and the frame cap are fixed when the reader is
    /// constructed. Without this record, editing a camera's RTSP URL changed
    /// the database and nothing else, and the only way to apply it was to
    /// restart the whole application — which looked, from the UI, exactly like
    /// the edit had been ignored.
    std::map<std::int64_t, std::string> worker_signature_;
    std::mutex workers_mtx_;

    std::thread desk_thread_;
    std::atomic<bool> stop_{false};
    bool started_{false};
};

}  // namespace so::app
