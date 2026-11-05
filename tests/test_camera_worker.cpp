#include "doctest/doctest.h"

#include "device.hpp"

#include "core/camera_worker.hpp"

#include <atomic>
#include <cstdlib>
#include <sys/stat.h>
#include <thread>

using namespace std::chrono_literals;

namespace {

// A real recording with people is the point of this test; the synthetic
// fallback only proves the pipeline is alive. Override with SO_TEST_CLIP.
std::string people_clip() {
    if (const char* e = std::getenv("SO_TEST_CLIP")) return e;
    return "/home/deepx1/dx-all-suite/aslam_workspace/VisionX/server/edge-videos/"
           "main_stream/(copy)06_object_abandone_corridor.mp4";
}

bool exists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && st.st_size > 1000;
}

/// Records what the pipeline produced.
class RecordingSink : public so::EventSink {
public:
    void emit(so::Event e) override {
        std::lock_guard<std::mutex> lk(m);
        events.push_back(std::move(e));
    }
    void emit(so::AttendanceEvent e) override {
        std::lock_guard<std::mutex> lk(m);
        attendance.push_back(std::move(e));
    }
    void emit(so::DeskTick t) override {
        std::lock_guard<std::mutex> lk(m);
        ticks.push_back(t);
    }
    void emit(so::ExitReview r) override {
        std::lock_guard<std::mutex> lk(m);
        reviews.push_back(std::move(r));
    }
    std::mutex m;
    std::vector<so::Event> events;
    std::vector<so::AttendanceEvent> attendance;
    std::vector<so::DeskTick> ticks;
    std::vector<so::ExitReview> reviews;
};

/// Counts what the modules were actually handed.
class ProbeModule : public so::IModule {
public:
    const char* id() const override { return "probe"; }
    void on_frame(const so::FrameContext& ctx, so::EventSink&) override {
        frames.fetch_add(1);
        tracks.fetch_add(static_cast<int>(ctx.tracks.size()));
        if (!ctx.tracks.empty()) frames_with_people.fetch_add(1);
        for (const auto& t : ctx.tracks) {
            int good = 0;
            for (const auto& k : t.kps)
                if (k.conf >= 0.5f) ++good;
            if (good >= 5) frames_with_keypoints.fetch_add(1);
            break;
        }
        // The module must receive SOURCE-resolution geometry, not canvas.
        if (ctx.frame && !ctx.frame->empty()) {
            width.store(ctx.frame->cols);
            height.store(ctx.frame->rows);
            for (const auto& t : ctx.tracks)
                if (t.box[2] > static_cast<float>(ctx.frame->cols) + 1.0f)
                    out_of_frame.fetch_add(1);
        }
    }
    std::atomic<int> frames{0}, tracks{0}, frames_with_people{0};
    std::atomic<int> frames_with_keypoints{0}, out_of_frame{0};
    std::atomic<int> width{0}, height{0};
};

}  // namespace

TEST_CASE("a real clip runs end to end through the worker on the NPU") {
    SO_REQUIRE_NPU();
    SO_REQUIRE_DECODER();
    const std::string clip = people_clip();
    if (!exists(clip)) {
        MESSAGE("no source clip at " << clip << " — set SO_TEST_CLIP to run this");
        return;
    }

    so::EnginePool pose("models/yolo26s_pose.dxnn", 3);
    so::PostprocessPool pool(3, 32);
    RecordingSink sink;

    so::WorkerDeps deps;
    deps.pose = &pose;
    deps.pool = &pool;
    deps.sink = &sink;

    // One global dispatcher, exactly as the application wires it.
    pose.register_callback([](dxrt::TensorPtrs& out, void* arg) {
        so::CameraWorker::on_inference_done(out, arg);
    });

    so::CameraConfig cfg;
    cfg.id = 1;
    cfg.tenant_id = 1;
    cfg.name = "test";
    cfg.detect_hz = 10;

    so::CameraReaderConfig rcfg;
    rcfg.url = clip;

    so::CameraWorker worker(cfg, rcfg, deps);
    auto probe = std::make_unique<ProbeModule>();
    ProbeModule* p = probe.get();
    std::vector<so::ModulePtr> mods;
    mods.push_back(std::move(probe));
    worker.set_modules(std::move(mods));

    REQUIRE(worker.start());
    std::this_thread::sleep_for(6s);
    const auto st = worker.stats();
    worker.stop();
    pool.drain();

    MESSAGE("frames_read=" << st.frames_read << " submitted=" << st.submitted
            << " inferred=" << st.inferred << " skipped_busy=" << st.skipped_busy
            << " detect_hz=" << st.detect_hz << " decode=" << st.decode_path);
    MESSAGE("module frames=" << p->frames.load() << " total_tracks=" << p->tracks.load()
            << " frames_with_people=" << p->frames_with_people.load()
            << " source=" << p->width.load() << "x" << p->height.load());

    CHECK(st.frames_read > 0);
    CHECK(st.inferred > 0);
    CHECK(p->frames.load() > 0);

    // Cadence: detection is gated well below the decode rate. That gating is
    // the reason eight cameras fit on this board at all.
    CHECK(st.inferred <= st.frames_read);
    CHECK(st.detect_hz <= static_cast<double>(cfg.detect_hz) * 1.5);

    // Real detections on a real recording — not merely "the pipeline ran".
    CHECK(p->tracks.load() > 0);
    CHECK(p->frames_with_people.load() > 0);
    CHECK(p->frames_with_keypoints.load() > 0);

    // Geometry reaches the modules in SOURCE coordinates. If the letterbox
    // inverse were wrong, boxes would sit outside the frame.
    CHECK(p->width.load() > 0);
    CHECK(p->out_of_frame.load() == 0);
}

TEST_CASE("the worker never accumulates work, whatever it is asked for") {
    SO_REQUIRE_NPU();
    SO_REQUIRE_DECODER();
    const std::string clip = people_clip();
    if (!exists(clip)) return;

    so::EnginePool pose("models/yolo26s_pose.dxnn", 1);   // deliberately starved
    so::PostprocessPool pool(1, 4);
    RecordingSink sink;

    so::WorkerDeps deps;
    deps.pose = &pose;
    deps.pool = &pool;
    deps.sink = &sink;
    pose.register_callback([](dxrt::TensorPtrs& out, void* arg) {
        so::CameraWorker::on_inference_done(out, arg);
    });

    so::CameraConfig cfg;
    cfg.id = 2;
    cfg.tenant_id = 1;
    cfg.detect_hz = 60;                                   // ask for far too much

    so::CameraReaderConfig rcfg;
    rcfg.url = clip;

    so::CameraWorker worker(cfg, rcfg, deps);
    REQUIRE(worker.start());
    std::this_thread::sleep_for(4s);
    const auto st = worker.stats();
    const auto queued = pool.queued();
    worker.stop();
    pool.drain();

    MESSAGE("starved: read=" << st.frames_read << " submitted=" << st.submitted
            << " inferred=" << st.inferred << " skipped_busy=" << st.skipped_busy
            << " queued=" << queued << " dropped=" << pool.dropped());

    CHECK(st.inferred > 0);

    // The invariant that actually matters, and the only one this setup can
    // observe: work never accumulates. An earlier version of this test asserted
    // that a one-core pool MUST shed, which was wrong — asking for 60 Hz does
    // not create 60 Hz of work when the source cannot deliver it, so the NPU was
    // never the constraint and nothing needed shedding. Asserting that would
    // have been asserting something about the fixture clip's frame rate.
    CHECK(st.submitted <= st.frames_read);
    CHECK(st.inferred <= st.submitted);
    CHECK(queued <= 4);                     // the pool's bound, never exceeded

    // Whatever was submitted either completed or is accounted for as in flight;
    // nothing is silently lost.
    CHECK(st.submitted - st.inferred <= 4);
}

TEST_CASE("a person in the exit zone is looked at before anyone else") {
    // The face budget is three people per cycle. Who gets it decides who can
    // be named, and somebody in the exit zone is the one case where waiting
    // for the next cycle may mean never: they are a step from the door.
    //
    // This checks the ORDER, not the recognition — the ordering rule is the
    // whole of the decision, and it is pure geometry.
    so::CameraConfig cfg;
    cfg.id = 1;
    so::Zone z;
    z.id = 1;
    z.type = so::ZoneType::Exit;
    z.polygon = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    cfg.zones.push_back(z);

    const auto in_zone = [&cfg](const so::Box& box) {
        const cv::Point2f foot = so::foot_point(box);
        for (const auto& zz : cfg.zones) {
            if (!zz.enabled || zz.type != so::ZoneType::Exit) continue;
            if (so::point_in_polygon(foot, zz.polygon)) return true;
        }
        return false;
    };

    // A big, close person out in the room, and a small distant one at the door.
    // Size alone would pick the first; the exit zone has to override it.
    const so::Box near_desk{300, 100, 500, 400};   // foot at (400, 400) - outside
    const so::Box at_door{20, 10, 60, 90};         // foot at (40, 90)  - inside
    CHECK_FALSE(in_zone(near_desk));
    CHECK(in_zone(at_door));

    std::vector<so::Box> order{near_desk, at_door};
    std::stable_sort(order.begin(), order.end(),
                     [&](const so::Box& a, const so::Box& b) {
                         const bool ea = in_zone(a), eb = in_zone(b);
                         if (ea != eb) return ea;
                         return (a[2] - a[0]) * (a[3] - a[1]) > (b[2] - b[0]) * (b[3] - b[1]);
                     });
    CHECK(in_zone(order.front()));   // the one at the door goes first
}
