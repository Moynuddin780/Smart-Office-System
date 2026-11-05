#include "app/engine.hpp"

#include "core/log.hpp"
#include "core/preprocess.hpp"
#include "identity/face_identifier.hpp"
#include "vision/arcface.hpp"
#include "modules/attendance.hpp"
#include "modules/out_count.hpp"
#include "modules/desk_duty.hpp"
#include "modules/fall.hpp"
#include "modules/perimeter.hpp"
#include "modules/unauthorised.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <sstream>

namespace so::app {
namespace {

// Enrolment gates, deliberately different from the LIVE identification gates.
//
// 80 px is the floor and 96 px the point of full marks, because that is where
// this pipeline was measured to work: below roughly 80 px on the face an
// embedding stops carrying identity at all.
constexpr float kEnrolMinFacePx = 80.0f;
// Live identification refuses anything past 0.22 eye-widths because beyond
// ~0.35 the best WRONG name outscores the right one. Enrolment is the opposite
// problem: a profile needs pose spread or it only recognises one angle. 0.55
// admits a genuine three-quarter view and still rejects a profile shot whose
// landmarks no longer describe the face.
constexpr float kEnrolMaxYaw = 0.55f;
// Where a photo stops getting better. Beyond this the extra pixels buy nothing
// the embedder can use, so the score tops out rather than rewarding a portrait
// nobody will ever match against.
constexpr float kEnrolGoodFacePx = 160.0f;

/// A number an operator reads, not a number a machine reads. std::to_string on
/// a float gives "80.000000", which in a message that is also spoken aloud by
/// the guided capture is worse than wrong - it is unreadable.
std::string fixed1(float v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << v;
    return o.str();
}
std::string fixed2(float v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << v;
    return o.str();
}


std::string iso_now(TimePoint t) {
    const auto tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    return buf;
}

std::string work_date_of(TimePoint t) {
    const auto tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string json_escape(const std::string& in) {
    std::ostringstream o;
    for (char c : in) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                      << static_cast<int>(c);
                else
                    o << c;
        }
    }
    return o.str();
}

}  // namespace

/**
 * Turns what the modules emit into database rows, queued notifications and a
 * live feed.
 *
 * Runs on a postprocess worker thread, so everything here is short. The one
 * thing it must never do is throw into the frame path — a failed insert is a
 * logged error, not a dead camera.
 */
class Engine::DbSink : public EventSink {
public:
    DbSink(db::Repos& repos, notify::SnapshotStore& snapshots,
           std::map<std::int64_t, std::string>& tenant_slugs,
           PresenceRoster& presence)
        : repos_(repos), snapshots_(snapshots), slugs_(tenant_slugs),
          presence_(presence) {}

    void set_live_callback(std::function<void(const LiveEvent&)> cb) {
        std::lock_guard<std::mutex> lk(cb_mtx_);
        live_cb_ = std::move(cb);
    }

    void emit(Event e) override {
        try {
            const db::TenantId t{e.tenant_id};
            db::EventRow row;
            row.camera_id = e.camera_id;
            row.module = e.module;
            row.severity = to_string(e.severity);
            row.title = e.title;
            row.detail = e.detail;
            row.ts = iso_now(e.ts);
            row.zone_id = e.zone_id;
            row.employee_id = e.employee_id;
            row.snapshot_path = snapshots_.save(e.tenant_id, slug(e.tenant_id),
                                                e.module + "-full", e.snapshot);
            row.crop_path = snapshots_.save(e.tenant_id, slug(e.tenant_id),
                                            e.module + "-crop", e.crop);

            const std::int64_t id = repos_.record_event(t, row);

            if (e.notify) {
                // Queued, never sent from here: a slow relay must not be able to
                // stall the frame path.
                repos_.enqueue_notification(t, id, "telegram");
                repos_.enqueue_notification(t, id, "email");
            }

            std::ostringstream j;
            j << R"({"type":"event","id":)" << id
              << R"(,"module":")" << json_escape(row.module)
              << R"(","severity":")" << row.severity
              << R"(","title":")" << json_escape(row.title)
              << R"(","detail":")" << json_escape(row.detail)
              << R"(","camera_id":)" << row.camera_id
              << R"(,"ts":")" << json_escape(row.ts) << R"("})";
            publish(LiveEvent{e.tenant_id, j.str()});
        } catch (const std::exception& ex) {
            SO_LOG_ERROR("[sink] event write failed: %s", ex.what());
        }
    }

    void emit(AttendanceEvent e) override {
        try {
            const db::TenantId t{e.tenant_id};
            const std::string path = snapshots_.save(e.tenant_id, slug(e.tenant_id),
                                                     "attendance", e.snapshot);
            repos_.record_attendance(t, e.employee_id, e.camera_id,
                                     e.direction == Direction::In ? "in" : "out",
                                     iso_now(e.ts), work_date_of(e.ts), e.confidence,
                                     e.track_id, path);

            // THE CLOCK AN EXIT IS CHECKED AGAINST.
            //
            // Stamped here rather than by whoever raised the arrival, because
            // this is the one place every `in` from every camera passes
            // through. See OutCountModule's min_stay: a person recorded
            // walking in seconds ago has not walked out.
            if (e.direction == Direction::In) presence_.mark_in(e.employee_id);

            std::ostringstream j;
            j << R"({"type":"attendance","employee_id":)" << e.employee_id
              << R"(,"direction":")" << (e.direction == Direction::In ? "in" : "out")
              << R"(","camera_id":)" << e.camera_id
              << R"(,"ts":")" << json_escape(iso_now(e.ts)) << R"("})";
            publish(LiveEvent{e.tenant_id, j.str()});
        } catch (const std::exception& ex) {
            SO_LOG_ERROR("[sink] attendance write failed: %s", ex.what());
        }
    }

    void emit(ExitReview r) override {
        try {
            const db::TenantId t{r.tenant_id};
            const std::string crop = snapshots_.save(r.tenant_id, slug(r.tenant_id),
                                                     "exit-face", r.crop);
            const std::string frame = snapshots_.save(r.tenant_id, slug(r.tenant_id),
                                                      "exit-full", r.snapshot);
            repos_.record_exit_review(t, r.camera_id, r.zone_id, iso_now(r.ts),
                                      work_date_of(r.ts), r.track_id,
                                      r.guess_employee_id, r.guess_score,
                                      r.guess_margin, r.guess_weight, crop, frame);

            // Not pushed as a live event and not notified. Somebody leaving is
            // not an alarm, and a queue of questions is read when there is time
            // to answer them, not the moment each one appears.
            SO_LOG_INFO("[sink] exit with no name on camera %lld — kept for review "
                        "(best guess %lld at %.2f, weight %.2f)",
                        static_cast<long long>(r.camera_id),
                        static_cast<long long>(r.guess_employee_id),
                        static_cast<double>(r.guess_score),
                        static_cast<double>(r.guess_weight));
        } catch (const std::exception& ex) {
            SO_LOG_ERROR("[sink] exit review write failed: %s", ex.what());
        }
    }

    /// Desk ticks arrive once a second per zone and are far too frequent to
    /// write individually. They are accumulated here and flushed periodically,
    /// which turns thousands of tiny updates into one per zone per flush.
    void emit(DeskTick t) override {
        std::lock_guard<std::mutex> lk(desk_mtx_);
        auto& a = desk_[{t.tenant_id, t.zone_id}];
        a.employee_id = t.employee_id;
        a.work_date = work_date_of(t.ts);

        if (t.present) {
            ++a.occupied;
            // A session begins on the first tick of a run of presence, so the
            // count is "how many times they sat down", not "how many seconds".
            if (!a.was_present) ++a.sessions;
            // The absence that just ended competes for the day's longest.
            a.longest_absence = std::max(a.longest_absence, a.current_absence);
            a.current_absence = 0;
        } else {
            ++a.absent;
            ++a.current_absence;
            a.longest_absence = std::max(a.longest_absence, a.current_absence);
        }
        a.was_present = t.present;

        if (t.observed_employee_id != 0 && t.observed_employee_id != t.employee_id)
            ++a.mismatch;
    }

    void flush_desk() {
        std::map<std::pair<std::int64_t, std::int64_t>, Accum> batch;
        {
            std::lock_guard<std::mutex> lk(desk_mtx_);
            batch.swap(desk_);
            // A flush must not reset an absence that is still running, or a
            // twelve-minute break spanning two flushes would be reported as two
            // shorter ones. Carry the in-progress state forward.
            for (const auto& [key, a] : batch) {
                Accum carry;
                carry.employee_id = a.employee_id;
                carry.work_date = a.work_date;
                carry.current_absence = a.current_absence;
                carry.longest_absence = a.longest_absence;
                carry.was_present = a.was_present;
                desk_[key] = carry;
            }
        }
        for (auto& [key, a] : batch) {
            if (a.occupied == 0 && a.absent == 0 && a.mismatch == 0) continue;
            try {
                repos_.upsert_desk_day(db::TenantId{key.first}, key.second, a.employee_id,
                                       a.work_date, a.occupied, a.absent, a.mismatch,
                                       a.sessions, a.longest_absence);
            } catch (const std::exception& ex) {
                SO_LOG_ERROR("[sink] desk flush failed: %s", ex.what());
            }
        }
    }

private:
    struct Accum {
        std::int64_t employee_id{0};
        std::string work_date;
        int occupied{0}, absent{0}, mismatch{0};
        int sessions{0};
        int current_absence{0};     ///< the run in progress
        int longest_absence{0};     ///< the longest seen so far
        bool was_present{false};
    };

    std::string slug(std::int64_t tenant_id) {
        const auto it = slugs_.find(tenant_id);
        return it == slugs_.end() ? "" : it->second;
    }

    void publish(LiveEvent e) {
        std::function<void(const LiveEvent&)> cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx_);
            cb = live_cb_;
        }
        if (cb) cb(e);
    }

    db::Repos& repos_;
    notify::SnapshotStore& snapshots_;
    std::map<std::int64_t, std::string>& slugs_;
    PresenceRoster& presence_;

    std::mutex desk_mtx_;
    std::map<std::pair<std::int64_t, std::int64_t>, Accum> desk_;

    std::mutex cb_mtx_;
    std::function<void(const LiveEvent&)> live_cb_;
};

namespace {
std::map<std::int64_t, std::string> g_slugs;
}

Engine::Engine(const Config& cfg, db::Repos& repos) : cfg_(cfg), repos_(repos) {}

Engine::~Engine() { stop(); }

void Engine::start() {
    if (started_) return;

    const std::filesystem::path models(cfg_.models_dir);
    const auto require = [&](const char* name) {
        const auto p = models / name;
        if (!std::filesystem::exists(p))
            throw std::runtime_error(std::string("model not found: ") + p.string() +
                                     " — the application will not start half-blind");
        return p.string();
    };

    // Three engines per model, one bound to each NPU core. Never NPU_ALL — see
    // EnginePool for the measurement that settled that.
    pose_ = std::make_unique<EnginePool>(require("yolo26s_pose.dxnn"), cfg_.npu_cores,
                                         cfg_.use_ort, cfg_.buffer_count);
    scrfd_ = std::make_unique<EnginePool>(require("scrfd_10g.dxnn"), cfg_.npu_cores,
                                          cfg_.use_ort, cfg_.buffer_count);
    arcface_ = std::make_unique<EnginePool>(require(cfg_.rec_model.c_str()),
                                            cfg_.npu_cores, cfg_.use_ort,
                                            cfg_.buffer_count);

    pool_ = std::make_unique<PostprocessPool>(cfg_.postprocess_workers, 64);
    gallery_ = std::make_unique<Gallery>();
    presence_ = std::make_unique<PresenceRoster>();
    snapshots_ = std::make_unique<notify::SnapshotStore>(cfg_.snapshot_dir,
                                                         cfg_.jpeg_quality);
    sink_ = std::make_unique<DbSink>(repos_, *snapshots_, g_slugs, *presence_);

    // One dispatcher for the whole process; the per-job user argument routes the
    // result back to the camera that submitted it.
    pose_->register_callback([](dxrt::TensorPtrs& out, void* arg) {
        CameraWorker::on_inference_done(out, arg);
    });

    outbox_ = std::make_unique<notify::OutboxWorker>(repos_, *snapshots_);
    outbox_->start();

    reload_gallery();
    reload();

    stop_.store(false);
    desk_thread_ = std::thread(&Engine::desk_flush_loop, this);
    started_ = true;

    SO_LOG_INFO("[engine] started: %d engines across %d cores, %d postprocess workers",
             static_cast<int>(pose_->size() + scrfd_->size() + arcface_->size()),
             cfg_.npu_cores, cfg_.postprocess_workers);
}

void Engine::stop() {
    if (!started_) return;
    stop_.store(true);
    if (desk_thread_.joinable()) desk_thread_.join();

    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        // Stop taking frames first, then let the pool finish what is in flight:
        // tearing down a worker whose job is still with DX-RT is a use-after-free.
        for (auto& [id, w] : workers_) w->stop();
        workers_.clear();
    }
    if (pool_) pool_->drain();
    if (sink_) sink_->flush_desk();
    if (outbox_) outbox_->stop();
    if (pool_) pool_->stop();
    started_ = false;
    SO_LOG_INFO("[engine] stopped");
}

void Engine::desk_flush_loop() {
    while (!stop_.load()) {
        for (int i = 0; i < 100 && !stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (sink_) sink_->flush_desk();
        sync_probed_sizes();
        refresh_presence();
        refresh_open_days();
    }
}

void Engine::refresh_open_days() {
    // KEEP TODAY'S WORKING TIME MOVING.
    //
    // A day's totals are worked out when an attendance event arrives and then
    // stored, and for somebody with no closing exit the total runs to "now" —
    // but "now" is only ever read at that moment. So the clock stops at the
    // last sighting: measured here at 10:34, three people showed the working
    // time they had at 10:17, 10:21 and 10:21. Somebody sitting at a desk the
    // cameras cannot see simply stops accruing hours, which is the opposite of
    // what the column claims to mean.
    //
    // Only days that are still OPEN are touched — a day closed by a real exit
    // is finished and its totals must not drift. Closed days are also the bulk
    // of the table, so skipping them is what keeps this cheap enough to run on
    // a loop.
    const auto now = std::chrono::steady_clock::now();
    if (now - last_day_refresh_ < kDayRefreshInterval) return;
    last_day_refresh_ = now;
    try {
        for (const auto& t : repos_.all_tenants()) {
            if (t.status != "active") continue;
            const db::TenantId tid{t.id};
            for (const auto& [id, date] : repos_.open_days_today(tid))
                repos_.recompute_day(tid, id, date);
        }
    } catch (const std::exception& e) {
        // The stored figures simply stay as they were until the next pass.
        SO_LOG_WARN("[engine] could not refresh open days: %s", e.what());
    }
}

void Engine::refresh_presence() {
    // Who has been recorded arriving today, for the desk module.
    //
    // Polled rather than pushed because it has to be right after a restart too:
    // the app can come up at noon, and everybody who arrived that morning is
    // already present whether or not this process saw it happen.
    try {
        std::set<std::int64_t> ids;
        for (const auto& t : repos_.all_tenants()) {
            if (t.status != "active") continue;
            for (auto id : repos_.employees_present_today(db::TenantId{t.id})) ids.insert(id);
        }
        if (presence_) presence_->set(std::move(ids));
    } catch (const std::exception& e) {
        // Leave the last known roster in place. Treating a database hiccup as
        // "nobody is here" would stop every desk counting.
        SO_LOG_WARN("[engine] could not refresh who is present: %s", e.what());
    }
}

void Engine::sync_probed_sizes() {
    // Record what each camera is ACTUALLY streaming.
    //
    // Nothing used to write this back, so the stored size was whatever somebody
    // typed when the camera was added and stayed that way through every change
    // of stream. A camera recorded as 720x576 was really sending 864x480, and
    // once the reader began sizing frames from the stored figure it squeezed
    // 16:9 into 5:4 — a picture that still looks like the room with every
    // facial landmark moved.
    //
    // This runs on the background loop rather than in reload(), which is where
    // it was first put and where it could never work: reload() runs at startup
    // and when somebody saves a setting, and at startup the workers have not
    // decoded a frame yet, so there is no size to record. It has to be asked
    // again later, which means a loop.
    //
    // Keeping it true is also what makes the per-camera frame limit work on a
    // camera that was never probed: until the real size is known there is
    // nothing to compare the limit against, so no limit is applied.
    bool corrected = false;
    std::vector<std::pair<std::int64_t, std::pair<int, int>>> seen;
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        for (const auto& [id, w] : workers_) {
            const auto st = w->stats();
            // ONLY when the pipeline is not resizing. width()/height() is what
            // comes OUT of the pipeline, not what the camera sends, and taking
            // one for the other oscillates for ever: the real 2688x1520 gets
            // scaled to 640x360, that is recorded as the source, the limit then
            // has nothing to act on, the next frame arrives at 2688x1520, and
            // around it goes — twenty-seven "corrections" in five minutes, each
            // one restarting the camera.
            if (st.native_size && st.width > 0 && st.height > 0)
                seen.push_back({id, {st.width, st.height}});
        }
    }
    if (seen.empty()) return;

    try {
        for (const auto& tenant : repos_.all_tenants()) {
            if (tenant.status != "active") continue;
            const db::TenantId t{tenant.id};
            for (const auto& c : repos_.cameras(t)) {
                for (const auto& [id, wh] : seen) {
                    if (id != c.id) continue;
                    if (wh.first == c.probed_width && wh.second == c.probed_height) break;
                    db::CameraRow fixed = c;
                    fixed.probed_width = wh.first;
                    fixed.probed_height = wh.second;
                    repos_.update_camera(t, c.id, fixed);
                    SO_LOG_INFO("[engine] camera %lld really streams %dx%d (recorded as "
                                "%dx%d) — corrected",
                                static_cast<long long>(c.id), wh.first, wh.second,
                                c.probed_width, c.probed_height);
                    corrected = true;
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        SO_LOG_WARN("[engine] could not record stream sizes: %s", e.what());
        return;
    }

    // Rebuild the affected workers. The frame size is baked into the GStreamer
    // caps when a pipeline is built, so learning the real size changes nothing
    // until the pipeline is built again — and the frame limit, which is only
    // meaningful once the real size is known, would never take effect at all.
    //
    // This settles after one pass: the next sweep finds the stored sizes already
    // right and asks for nothing.
    if (corrected) reload();
}

void Engine::reload_gallery() {
    if (!gallery_) return;
    try {
        const auto all = repos_.gallery_all();
        // One appliance serves every tenant, so the running gallery holds all of
        // them. Identity is still reported per camera, and a camera belongs to
        // exactly one tenant, so a face can only ever be matched within its own.
        std::map<std::int64_t, std::vector<EnrolledProfile>> by_tenant;
        for (const auto& [tid, p] : all) by_tenant[tid].push_back(p);

        std::vector<EnrolledProfile> flat;
        for (auto& [tid, v] : by_tenant)
            for (auto& p : v) {
                // The employee id is globally unique, so tagging is unnecessary;
                // what matters is that a camera's tenant filters the result.
                flat.push_back(std::move(p));
            }
        const auto r = gallery_->set(flat);
        SO_LOG_INFO("[engine] gallery reloaded: %d profiles, version %llu", r.kept,
                 static_cast<unsigned long long>(r.version));
    } catch (const std::exception& e) {
        SO_LOG_ERROR("[engine] gallery reload failed: %s", e.what());
    }
}

void Engine::reload() {
    std::lock_guard<std::mutex> lk(workers_mtx_);

    const auto tenants = repos_.all_tenants();
    g_slugs.clear();
    std::vector<std::int64_t> active;
    for (const auto& t : tenants) {
        g_slugs[t.id] = t.slug;
        if (t.status == "active") active.push_back(t.id);
    }

    std::vector<std::int64_t> wanted;
    for (std::int64_t tid : active) {
        const db::TenantId t{tid};
        for (const auto& c : repos_.cameras(t)) {
            if (!c.enabled) continue;

            CameraConfig cc;
            cc.id = c.id;
            cc.tenant_id = tid;
            cc.name = c.name;
            cc.role = c.role == "gate" ? CameraRole::Gate : CameraRole::Area;
            cc.face_enabled = c.face_enabled;
            cc.detect_hz = c.detect_hz;
            cc.face_hz = c.face_hz;
            cc.zones = repos_.zones(t, c.id);
            cc.gate_lines = repos_.gate_lines(t, c.id);
            cc.modules = repos_.camera_modules(t, c.id);

            // The sub-stream is the default and the reason eight cameras fit on
            // this board. `main` exists for a gate camera whose sub-stream is too
            // small for a face to be identifiable.
            const std::string url =
                (c.stream_pref == "main" || c.rtsp_sub.empty()) ? c.rtsp_main : c.rtsp_sub;
            if (url.empty()) {
                SO_LOG_WARN("[engine] camera %lld has no usable URL; skipping",
                         static_cast<long long>(c.id));
                continue;
            }

            wanted.push_back(c.id);

            // Everything that is baked into the reader and cannot be changed on
            // a running one. If any of it moved, the worker is rebuilt rather
            // than reconfigured.
            // The frame size is part of this. It is baked into the GStreamer
            // caps when the pipeline is built, so changing it on a running
            // worker does nothing at all — the operator lowers the limit, the
            // page saves, and the picture stays exactly as it was with no hint
            // that the setting was ignored.
            const std::string signature =
                url + "|" + (c.face_enabled ? "uncapped" : "capped") + "|" +
                std::to_string(c.max_px) + "|" +
                std::to_string(c.max_fps) + "|" +
                std::to_string(c.probed_width) + "x" + std::to_string(c.probed_height);

            const auto it = workers_.find(c.id);
            if (it != workers_.end()) {
                if (worker_signature_[c.id] == signature) {
                    it->second->reconfigure(cc);   // zones redrawn, employee reassigned
                    continue;
                }
                SO_LOG_INFO("[engine] camera %lld now reads a different stream — restarting it",
                            static_cast<long long>(c.id));
                it->second->stop();
                workers_.erase(it);
            }

            CameraReaderConfig rc;
            rc.url = url;
            rc.ring_size = cfg_.frame_ring;
            // What the camera last reported it streams. The hardware decoder
            // needs this to know whether there is anything to scale down; see
            // CameraReaderConfig::src_width.
            rc.src_width = c.probed_width;
            rc.src_height = c.probed_height;
            // The frame cap exists to cut the colour-conversion cost, which
            // tracks output pixels. For a body camera that is free: detection
            // runs on a 640x640 letterbox either way.
            //
            // For a FACE camera the pixels ARE the product. Identification
            // needs roughly 60 px of face, and capping a 1080p stream to
            // 960x540 halves it - measured on this deployment, a face camera
            // under the cap detected 896 faces, bound 585 of them to a person,
            // and got only 6 past the size gate. So a face camera gets a much
            // larger cap than a body camera.
            //
            // But NOT an unlimited one, which is what this used to be. These
            // cameras stream 2688x1520 and 3200x1800; at full size every frame
            // is 12 MB through pose, head crop, overlay and JPEG, and measured
            // here that dropped one camera from 25 fps to 8 and starved the
            // other four until two of them dropped offline. The picture then
            // ran two and a half MINUTES behind the room and fell further
            // behind every second — a live wall that is three minutes stale is
            // not a live wall, whatever is drawn on it.
            //
            // 1000 on the long side. 1920 was tried and was still too much:
            // the face camera decoded 12 frames of every 25 and its picture ran
            // minutes behind the room.
            //
            // The faces survive the cut with room to spare, which is why this
            // number and not a smaller one. Measured here, that camera streams
            // 2688 px wide and shows faces of 290-342 px; at 1000 px wide they
            // are 108-127 px, four times the 24 px where matching begins. What
            // is given up is pixels that were being carried through pose, head
            // crop, overlay and JPEG for nobody.
            //
            // The two numbers are equal ON PURPOSE: this is a BOUNDING BOX, not
            // a frame shape. The output size is computed from the stream's own
            // aspect ratio, so 16:9 lands at 1000x562 and 4:3 at 1000x750 and
            // neither is stretched. A pair like 1920x1080 only worked while
            // every camera happened to be 16:9 — against a 4:3 stream it
            // squashed the picture and moved every facial landmark with it.
            if (c.face_enabled) {
                // Per camera, because what a face costs in pixels depends on
                // where the camera is, not on a number chosen once. The desk
                // camera here shows 300 px faces and can give most of them up;
                // the corridor camera shows 25 px and cannot give up any.
                rc.max_width = c.max_px;
                rc.max_height = c.max_px;
            }
            // Applies to every camera, face or not: the frames beyond this
            // rate were being converted and then dropped either way.
            rc.max_fps = c.max_fps;

            WorkerDeps deps;
            deps.pose = pose_.get();
            deps.scrfd = scrfd_.get();
            deps.arcface = arcface_.get();
            deps.pool = pool_.get();
            deps.gallery = gallery_.get();
            deps.presence = presence_.get();
            deps.sink = sink_.get();
            deps.jpeg_quality = cfg_.jpeg_quality;

            auto w = std::make_shared<CameraWorker>(cc, rc, deps);

            // A module with no stored row runs: adding a camera should not
            // require visiting a settings page before it does anything.
            const auto on = [&cc](const char* id) {
                const auto it = cc.modules.find(id);
                return it == cc.modules.end() || it->second;
            };
            std::vector<ModulePtr> mods;
            if (on("perimeter")) mods.push_back(std::make_unique<PerimeterModule>());
            if (on("fall")) mods.push_back(std::make_unique<FallModule>());
            if (on("desk")) mods.push_back(std::make_unique<DeskDutyModule>());
            if (cc.face_enabled) {
                if (on("unauthorised")) mods.push_back(std::make_unique<UnauthorisedModule>());
                // Attendance is attached whenever it is switched on — NOT when
                // the role happens to be spelled "gate". Once a site can name
                // its own roles, keying behaviour off that name means a camera
                // called "reception gate" silently does nothing. What the module
                // actually needs is a gate LINE, and with no line drawn it
                // simply never fires, which is the honest outcome.
                if (on("attendance")) mods.push_back(std::make_unique<AttendanceModule>());
                // Face-gated with attendance: an exit without a name is a
                // person leaving, which no timesheet has a column for.
                if (on("out_count")) mods.push_back(std::make_unique<OutCountModule>());
            }
            w->set_modules(std::move(mods));

            if (w->start()) {
                SO_LOG_INFO("[engine] camera %lld (%s) started on %s", 
                         static_cast<long long>(c.id), c.name.c_str(),
                         c.stream_pref == "main" ? "the main stream" : "the sub-stream");
                workers_[c.id] = std::move(w);
                worker_signature_[c.id] = signature;
            } else {
                SO_LOG_ERROR("[engine] camera %lld failed to start", static_cast<long long>(c.id));
            }
        }
    }

    // Anything no longer wanted — disabled, deleted, or its tenant suspended.
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (std::find(wanted.begin(), wanted.end(), it->first) == wanted.end()) {
            SO_LOG_INFO("[engine] camera %lld stopped", static_cast<long long>(it->first));
            it->second->stop();
            worker_signature_.erase(it->first);
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

cv::Mat Engine::annotated(std::int64_t camera_id) {
    std::lock_guard<std::mutex> lk(workers_mtx_);
    const auto it = workers_.find(camera_id);
    return it == workers_.end() ? cv::Mat() : it->second->latest_annotated();
}

std::shared_ptr<const std::string> Engine::annotated_jpeg(std::int64_t camera_id,
                                                          std::uint64_t* seq,
                                                          std::int64_t* age_ms) {
    // The map lock is held ONLY to find the worker and take a reference to it.
    //
    // It used to be held across the frame copy as well, and since every live
    // tile asks several times a second, that turned one mutex into a queue for
    // the whole application — /healthz, which needs the same lock, measured 7 s
    // with five tiles polling. The shared_ptr keeps the worker alive if a reload
    // drops it while somebody is reading.
    std::shared_ptr<CameraWorker> w;
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        const auto it = workers_.find(camera_id);
        if (it != workers_.end()) w = it->second;
    }
    if (!w) {
        if (seq) *seq = 0;
        if (age_ms) *age_ms = -1;
        return nullptr;
    }
    return w->latest_jpeg(seq, age_ms);
}

cv::Mat Engine::raw_frame(std::int64_t camera_id) {
    std::lock_guard<std::mutex> lk(workers_mtx_);
    const auto it = workers_.find(camera_id);
    return it == workers_.end() ? cv::Mat() : it->second->latest_raw();
}

void Engine::set_live_view(std::int64_t camera_id, bool on) {
    std::lock_guard<std::mutex> lk(workers_mtx_);
    const auto it = workers_.find(camera_id);
    if (it != workers_.end()) it->second->set_live_view(on);
}

void Engine::on_live_event(std::function<void(const LiveEvent&)> cb) {
    if (sink_) sink_->set_live_callback(std::move(cb));
}

Engine::EnrolResult Engine::enrol(const cv::Mat& image_bgr) {
    EnrolResult r;
    if (!scrfd_ || !arcface_) {
        r.reason = "the face engines are not loaded";
        return r;
    }
    if (image_bgr.empty()) {
        r.reason = "the image could not be decoded";
        return r;
    }

    LetterboxMap map;
    const cv::Mat canvas_rgb = letterbox(image_bgr, 640, map);

    std::vector<std::uint8_t> out(static_cast<std::size_t>(scrfd_->output_size()));
    std::vector<FaceDetection> faces;
    try {
        const dxrt::TensorPtrs t =
            scrfd_->run_sync(const_cast<uchar*>(canvas_rgb.data), out.data());
        faces = ScrfdDecoder(640, 0.5f, 0.4f).decode(views_of(t));
    } catch (const std::exception& e) {
        r.reason = std::string("face detection failed: ") + e.what();
        return r;
    }

    if (faces.empty()) {
        r.reason = "no face was found in this photo";
        return r;
    }
    // The largest face: an enrolment photo with a bystander in the background
    // must enrol the subject, not the bystander.
    const auto& f = *std::max_element(
        faces.begin(), faces.end(), [](const FaceDetection& a, const FaceDetection& b) {
            return (a.box[2] - a.box[0]) < (b.box[2] - b.box[0]);
        });

    r.face_px = f.box[2] - f.box[0];
    r.score = f.score;
    r.yaw = face_pose(f.lm).yaw;
    // Anything else the detector found. A second face in an enrolment photo is
    // how a profile ends up holding two different people, which is precisely
    // the failure the coherence score exists to catch afterwards - better to
    // say so now.
    for (const auto& other : faces)
        if (&other != &f && (other.box[2] - other.box[0]) >= 0.4f * r.face_px)
            ++r.other_faces;

    // The same gates the live recogniser applies. Accepting a photo here that
    // would be rejected there is how somebody ends up enrolled and never matched.
    IdentifierConfig gates;
    // The wording of these matters as much as the threshold. The guided
    // capture turns each one into a spoken instruction, and "quality too low"
    // tells a person nothing while "come a little closer" fixes it.
    if (r.score < gates.min_det_score) {
        r.reason = "unclear — the detector is only " + fixed1(r.score * 100.0f) +
                   "% sure this is a face";
        return r;
    }
    if (r.face_px < kEnrolMinFacePx) {
        r.reason = "too small — the face is " + std::to_string(static_cast<int>(r.face_px)) +
                   " px and needs at least " + std::to_string(static_cast<int>(kEnrolMinFacePx)) +
                   "; move closer or use a photo taken closer";
        return r;
    }
    // Enrolment accepts a WIDER pose range than live identification does, and
    // that is deliberate: a profile built only from square-on shots describes
    // one viewing angle well and every other angle badly, so the person is
    // recognised at the door and missed three steps down the corridor. What it
    // must not accept is a face turned so far that the landmarks stop
    // describing it.
    if (std::abs(r.yaw) > kEnrolMaxYaw) {
        r.reason = "turned — the head is at " + fixed2(r.yaw) + " eye-widths, past the " +
                   fixed2(kEnrolMaxYaw) + " this pipeline can still describe";
        return r;
    }

    try {
        ArcFace arc(*arcface_);
        // The crop is taken from the canvas the landmarks are in, so no second
        // resample of the same pixels.
        cv::Mat canvas_bgr;
        cv::cvtColor(canvas_rgb, canvas_bgr, cv::COLOR_RGB2BGR);
        r.aligned = align_face(canvas_bgr, f.lm);
        r.embedding = arc.embed(r.aligned);
    } catch (const std::exception& e) {
        r.reason = std::string("embedding failed: ") + e.what();
        return r;
    }

    // A 0-100 score an operator can READ, which means it has to vary.
    //
    // Scaling from zero made every acceptable photo score 100 and told nobody
    // anything. This anchors the scale to the range that actually matters:
    // 80 px is the floor and scores 50, 160 px and above is as good as this
    // pipeline can use and scores 100. Pose is a light secondary penalty
    // because a SPREAD of poses is wanted - punishing a turned head here would
    // push an operator towards the twenty-identical-photos profile that only
    // recognises somebody at the door.
    const float span = (r.face_px - kEnrolMinFacePx) / (kEnrolGoodFacePx - kEnrolMinFacePx);
    const float size_term = 0.5f + 0.5f * std::clamp(span, 0.0f, 1.0f);
    const float pose_term = 0.82f + 0.18f * std::exp(-(r.yaw / 0.45f) * (r.yaw / 0.45f));
    r.quality = static_cast<int>(std::lround(100.0f * size_term * pose_term));
    r.ok = true;
    return r;
}

Engine::Status Engine::status() {
    Status s;
    s.engines = pose_ ? static_cast<int>(pose_->size() + scrfd_->size() + arcface_->size()) : 0;
    for (int c = 0; c < cfg_.npu_cores; ++c) {
        const double u = EnginePool::core_utilization(c);
        s.core_utilisation.push_back(u < 0 ? 0.0 : u);
    }
    s.npu_ok = s.engines > 0;
    if (gallery_) {
        s.gallery_version = static_cast<std::int64_t>(gallery_->version());
        s.gallery_count = gallery_->count();
    }
    std::lock_guard<std::mutex> lk(workers_mtx_);
    s.cameras_running = static_cast<int>(workers_.size());
    for (auto& [id, w] : workers_)
        if (w->stats().online) ++s.cameras_online;
    return s;
}

std::vector<WorkerStats> Engine::worker_stats() {
    std::vector<WorkerStats> out;
    std::lock_guard<std::mutex> lk(workers_mtx_);
    for (auto& [id, w] : workers_) out.push_back(w->stats());
    return out;
}

}  // namespace so::app
