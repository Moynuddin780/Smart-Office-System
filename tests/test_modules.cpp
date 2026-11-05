#include "doctest/doctest.h"

#include "modules/attendance.hpp"
#include "modules/desk_duty.hpp"
#include "modules/fall.hpp"
#include "modules/out_count.hpp"
#include "modules/perimeter.hpp"
#include "modules/unauthorised.hpp"

#include <thread>

using namespace std::chrono_literals;
using so::Box;

namespace {

class Sink : public so::EventSink {
public:
    void emit(so::Event e) override { events.push_back(std::move(e)); }
    void emit(so::AttendanceEvent e) override { attendance.push_back(std::move(e)); }
    void emit(so::DeskTick t) override { ticks.push_back(t); }
    void emit(so::ExitReview r) override { reviews.push_back(std::move(r)); }
    std::vector<so::Event> events;
    std::vector<so::AttendanceEvent> attendance;
    std::vector<so::DeskTick> ticks;
    std::vector<so::ExitReview> reviews;
    void clear() { events.clear(); attendance.clear(); ticks.clear(); }
};

so::Track person(int id, float x1, float y1, float x2, float y2) {
    so::Track t;
    t.id = id;
    t.box = Box{x1, y1, x2, y2};
    t.score = 0.9f;
    t.confirmed = true;
    return t;
}

/// A standing skeleton: shoulders above hips, knees below hips, ankles at the
/// bottom. Coordinates are in a 200 px-tall person.
std::array<so::Keypoint, so::kNumKeypoints> standing() {
    std::array<so::Keypoint, so::kNumKeypoints> k{};
    for (auto& p : k) p.conf = 0.0f;
    auto set = [&](int i, float x, float y) { k[static_cast<std::size_t>(i)] = {x, y, 0.9f}; };
    set(so::NOSE, 100, 10);
    set(so::LEFT_SHOULDER, 88, 40);   set(so::RIGHT_SHOULDER, 112, 40);
    set(so::LEFT_HIP, 90, 110);       set(so::RIGHT_HIP, 110, 110);
    set(so::LEFT_KNEE, 90, 155);      set(so::RIGHT_KNEE, 110, 155);
    set(so::LEFT_ANKLE, 90, 200);     set(so::RIGHT_ANKLE, 110, 200);
    return k;
}

/// Seated: knees up near hip level, ankles below, torso still upright.
std::array<so::Keypoint, so::kNumKeypoints> seated() {
    auto k = standing();
    auto set = [&](int i, float x, float y) { k[static_cast<std::size_t>(i)] = {x, y, 0.9f}; };
    set(so::LEFT_KNEE, 90, 118);      set(so::RIGHT_KNEE, 110, 118);
    set(so::LEFT_ANKLE, 90, 175);     set(so::RIGHT_ANKLE, 110, 175);
    return k;
}

/// Lying down: the shoulder-to-hip line is horizontal.
std::array<so::Keypoint, so::kNumKeypoints> lying() {
    std::array<so::Keypoint, so::kNumKeypoints> k{};
    for (auto& p : k) p.conf = 0.0f;
    auto set = [&](int i, float x, float y) { k[static_cast<std::size_t>(i)] = {x, y, 0.9f}; };
    set(so::NOSE, 20, 100);
    set(so::LEFT_SHOULDER, 40, 95);   set(so::RIGHT_SHOULDER, 40, 115);
    set(so::LEFT_HIP, 110, 95);       set(so::RIGHT_HIP, 110, 115);
    set(so::LEFT_KNEE, 155, 100);     set(so::RIGHT_KNEE, 155, 115);
    set(so::LEFT_ANKLE, 200, 100);    set(so::RIGHT_ANKLE, 200, 115);
    return k;
}

so::CameraConfig with_zone(so::ZoneType type, std::int64_t employee = 0) {
    so::CameraConfig c;
    c.id = 1;
    c.tenant_id = 1;
    so::Zone z;
    z.id = 10;
    z.type = type;
    z.name = "Server Room";
    z.employee_id = employee;
    z.polygon = {{0, 0}, {200, 0}, {200, 200}, {0, 200}};
    c.zones.push_back(z);
    return c;
}

so::FrameContext make_ctx(const so::CameraConfig& cfg, std::vector<so::Track> tracks,
                          const cv::Mat* frame = nullptr) {
    so::FrameContext ctx;
    ctx.tenant_id = 1;
    ctx.camera_id = 1;
    ctx.ts = std::chrono::system_clock::now();
    ctx.cfg = &cfg;
    ctx.tracks = std::move(tracks);
    ctx.frame = frame;
    return ctx;
}

}  // namespace

// ── posture ────────────────────────────────────────────────────────────────

TEST_CASE("a standing person reads as upright") {
    const auto v = so::posture(standing());
    CHECK(v.verdict == so::Posture::Up);
    CHECK(v.torso_angle < 20.0f);
}

TEST_CASE("a SEATED person is upright, not fallen") {
    // The case both the torso angle and the head-drop signal get wrong on their
    // own, and the reason the knee test exists.
    const auto v = so::posture(seated());
    CHECK(v.verdict == so::Posture::Up);
    CHECK(v.knees_high);
}

TEST_CASE("a person lying down reads as down") {
    const auto v = so::posture(lying());
    CHECK(v.verdict == so::Posture::Down);
    CHECK(v.torso_angle > 55.0f);
}

TEST_CASE("unusable keypoints report Unknown so the caller can fall back") {
    std::array<so::Keypoint, so::kNumKeypoints> none{};
    for (auto& p : none) p.conf = 0.0f;
    const auto v = so::posture(none);
    CHECK(v.verdict == so::Posture::Unknown);
    CHECK(std::string(v.why) == "no torso");
}

// ── fall state machine ─────────────────────────────────────────────────────

TEST_CASE("FALLEN needs persistence and NEED HELP needs more") {
    so::FallConfig cfg;
    cfg.confirm = 1200ms;
    cfg.help = 10s;
    so::FallTracker f(cfg);

    auto t = person(1, 0, 0, 200, 200);
    t.kps = lying();
    const auto t0 = std::chrono::steady_clock::now();

    CHECK(f.feed(1, t, t0) == so::FallState::None);
    CHECK(f.feed(1, t, t0 + 500ms) == so::FallState::None);
    CHECK(f.feed(1, t, t0 + 1300ms) == so::FallState::Fallen);
    CHECK(f.feed(1, t, t0 + 5s) == so::FallState::Fallen);
    CHECK(f.feed(1, t, t0 + 11s) == so::FallState::NeedHelp);
}

TEST_CASE("getting up clears the state") {
    so::FallTracker f;
    auto down = person(1, 0, 0, 200, 200);
    down.kps = lying();
    auto up = person(1, 0, 0, 100, 200);
    up.kps = standing();

    const auto t0 = std::chrono::steady_clock::now();
    f.feed(1, down, t0);
    CHECK(f.feed(1, down, t0 + 2s) == so::FallState::Fallen);
    CHECK(f.feed(1, up, t0 + 2500ms) == so::FallState::None);
}

TEST_CASE("a person who keeps moving on the floor is FALLEN but not NEED HELP") {
    so::FallConfig cfg;
    cfg.help = 2s;
    cfg.still_frac = 0.12f;
    so::FallTracker f(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 12; ++i) {
        // Shuffling along the floor: the centre moves well over still_frac of
        // body height between frames.
        auto t = person(1, static_cast<float>(i * 40), 0,
                        static_cast<float>(i * 40 + 200), 200);
        t.kps = lying();
        f.feed(1, t, t0 + std::chrono::milliseconds(300 * i));
    }
    CHECK(f.state(1) == so::FallState::Fallen);
}

TEST_CASE("the fall module emits a warning then a critical, with a snapshot") {
    so::FallConfig cfg;
    cfg.confirm = 1ms;
    cfg.help = 2ms;
    so::FallModule m(cfg);
    so::CameraConfig cam;
    cam.id = 1;
    cam.tenant_id = 1;
    cv::Mat frame(240, 320, CV_8UC3, cv::Scalar::all(30));

    auto t = person(1, 10, 10, 210, 210);
    t.kps = lying();
    Sink sink;
    m.on_frame(make_ctx(cam, {t}, &frame), sink);
    std::this_thread::sleep_for(5ms);
    m.on_frame(make_ctx(cam, {t}, &frame), sink);

    REQUIRE(!sink.events.empty());
    CHECK(sink.events.front().module == std::string("fall"));
    CHECK_FALSE(sink.events.front().snapshot.empty());
    CHECK_FALSE(sink.events.front().crop.empty());
}

// ── perimeter ──────────────────────────────────────────────────────────────

TEST_CASE("a person inside the zone long enough raises exactly one event") {
    so::PerimeterConfig cfg;
    cfg.confirm = 1ms;
    so::PerimeterModule m(cfg);
    const auto cam = with_zone(so::ZoneType::Restricted);
    Sink sink;

    const auto t = person(1, 90, 90, 110, 150);   // foot point (100,150) is inside
    m.on_frame(make_ctx(cam, {t}), sink);
    std::this_thread::sleep_for(3ms);
    m.on_frame(make_ctx(cam, {t}), sink);
    m.on_frame(make_ctx(cam, {t}), sink);
    m.on_frame(make_ctx(cam, {t}), sink);

    CHECK(sink.events.size() == 1);
    CHECK(sink.events[0].zone_id == 10);
    CHECK(sink.events[0].severity == so::Severity::Critical);
}

TEST_CASE("a brief dip into the zone raises nothing") {
    so::PerimeterConfig cfg;
    cfg.confirm = 5s;
    so::PerimeterModule m(cfg);
    const auto cam = with_zone(so::ZoneType::Restricted);
    Sink sink;
    m.on_frame(make_ctx(cam, {person(1, 90, 90, 110, 150)}), sink);
    m.on_frame(make_ctx(cam, {person(1, 90, 90, 110, 150)}), sink);
    CHECK(sink.events.empty());
}

TEST_CASE("a person whose box covers the zone but whose feet do not is outside") {
    // A tall person standing just past the zone: the box overlaps it, the feet
    // do not. An overlap test would call this an intrusion.
    so::PerimeterConfig cfg;
    cfg.confirm = 1ms;
    so::PerimeterModule m(cfg);
    const auto cam = with_zone(so::ZoneType::Restricted);
    Sink sink;
    const auto t = person(1, 90, 50, 110, 260);   // foot point y=260, zone ends at 200
    m.on_frame(make_ctx(cam, {t}), sink);
    std::this_thread::sleep_for(3ms);
    m.on_frame(make_ctx(cam, {t}), sink);
    CHECK(sink.events.empty());
}

// ── desk on-duty ───────────────────────────────────────────────────────────

TEST_CASE("occupancy ticks while somebody is at the desk") {
    so::DeskConfig cfg;
    cfg.tick = 1ms;
    so::DeskDutyModule m(cfg);
    const auto cam = with_zone(so::ZoneType::Desk, /*employee*/7);
    Sink sink;

    m.on_frame(make_ctx(cam, {person(1, 90, 90, 110, 150)}), sink);
    std::this_thread::sleep_for(3ms);
    m.on_frame(make_ctx(cam, {person(1, 90, 90, 110, 150)}), sink);

    REQUIRE(sink.ticks.size() >= 2);
    CHECK(sink.ticks.back().present);
    CHECK(sink.ticks.back().employee_id == 7);
    CHECK(sink.ticks.back().zone_id == 10);
}

TEST_CASE("a short absence does not end the session") {
    so::DeskConfig cfg;
    cfg.tick = 1ms;
    cfg.absence_grace = 10s;
    so::DeskDutyModule m(cfg);
    const auto cam = with_zone(so::ZoneType::Desk, 7);
    Sink sink;

    m.on_frame(make_ctx(cam, {person(1, 90, 90, 110, 150)}), sink);
    std::this_thread::sleep_for(3ms);
    m.on_frame(make_ctx(cam, {}), sink);          // stepped away
    CHECK(sink.ticks.back().present);             // still counted, within grace
}

TEST_CASE("an absence past the grace period stops counting") {
    so::DeskConfig cfg;
    cfg.tick = 1ms;
    cfg.absence_grace = 2ms;
    so::DeskDutyModule m(cfg);
    const auto cam = with_zone(so::ZoneType::Desk, 7);
    Sink sink;

    m.on_frame(make_ctx(cam, {person(1, 90, 90, 110, 150)}), sink);
    std::this_thread::sleep_for(3ms);
    m.on_frame(make_ctx(cam, {}), sink);
    std::this_thread::sleep_for(5ms);
    m.on_frame(make_ctx(cam, {}), sink);
    CHECK_FALSE(sink.ticks.back().present);
}

TEST_CASE("a recognised stranger at the desk is recorded without changing the count") {
    so::DeskConfig cfg;
    cfg.tick = 1ms;
    so::DeskDutyModule m(cfg);
    const auto cam = with_zone(so::ZoneType::Desk, /*assigned*/7);
    Sink sink;

    auto ctx = make_ctx(cam, {person(1, 90, 90, 110, 150)});
    so::FaceObs f;
    f.track_id = 1;
    f.employee_id = 9;                 // somebody else
    f.employee_name = "Rahim";
    ctx.faces.push_back(f);
    m.on_frame(ctx, sink);

    REQUIRE(!sink.ticks.empty());
    CHECK(sink.ticks.back().present);
    CHECK(sink.ticks.back().employee_id == 7);            // the assignment stands
    CHECK(sink.ticks.back().observed_employee_id == 9);   // ...and the mismatch is recorded
}

// ── unauthorised ───────────────────────────────────────────────────────────

TEST_CASE("a face that matched nobody, held past the grace period, alerts") {
    so::UnauthorisedConfig cfg;
    cfg.unknown_grace = 1ms;
    so::UnauthorisedModule m(cfg);
    so::CameraConfig cam;
    cam.id = 1;
    cam.tenant_id = 1;
    Sink sink;

    auto ctx = make_ctx(cam, {person(1, 0, 0, 100, 300)});
    so::FaceObs f;
    f.track_id = 1;
    f.employee_id = 0;                 // seen, matched nobody
    f.box = Box{30, 10, 70, 60};
    ctx.faces.push_back(f);

    m.on_frame(ctx, sink);
    std::this_thread::sleep_for(3ms);
    auto ctx2 = make_ctx(cam, {person(1, 0, 0, 100, 300)});
    ctx2.faces.push_back(f);
    m.on_frame(ctx2, sink);

    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].module == std::string("unauthorised"));
}

TEST_CASE("a matched face never alerts") {
    so::UnauthorisedConfig cfg;
    cfg.unknown_grace = 1ms;
    so::UnauthorisedModule m(cfg);
    so::CameraConfig cam;
    Sink sink;

    for (int i = 0; i < 5; ++i) {
        auto ctx = make_ctx(cam, {person(1, 0, 0, 100, 300)});
        so::FaceObs f;
        f.track_id = 1;
        f.employee_id = 7;
        ctx.faces.push_back(f);
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(2ms);
    }
    CHECK(sink.events.empty());
}

TEST_CASE("a track with NO face is unidentified, never unauthorised") {
    // The case D2 makes common: on a sub-stream most people never present a
    // resolvable face. Alerting on them would be a permanent false-alarm stream.
    so::UnauthorisedConfig cfg;
    cfg.unknown_grace = 1ms;
    so::UnauthorisedModule m(cfg);
    so::CameraConfig cam;
    Sink sink;

    for (int i = 0; i < 6; ++i) {
        m.on_frame(make_ctx(cam, {person(1, 0, 0, 100, 300)}), sink);
        std::this_thread::sleep_for(2ms);
    }
    CHECK(sink.events.empty());
    CHECK(m.unidentified_count() == 1);
}

TEST_CASE("an unmatched face inside the grace period does not alert yet") {
    so::UnauthorisedConfig cfg;
    cfg.unknown_grace = 10s;
    so::UnauthorisedModule m(cfg);
    so::CameraConfig cam;
    Sink sink;

    for (int i = 0; i < 4; ++i) {
        auto ctx = make_ctx(cam, {person(1, 0, 0, 100, 300)});
        so::FaceObs f;
        f.track_id = 1;
        f.employee_id = 0;
        ctx.faces.push_back(f);
        m.on_frame(ctx, sink);
    }
    CHECK(sink.events.empty());
}

// ── attendance ─────────────────────────────────────────────────────────────

TEST_CASE("being recognised is the attendance record, with no line to cross") {
    // Attendance used to require a line drawn across the doorway, and direction
    // came from which way the track crossed it. That is gone: one camera
    // watches the way in, so anybody it names has arrived. The line cost more
    // than it gave — until somebody drew it the camera recognised people all
    // day and recorded nothing, which looks configured and is not.
    so::AttendanceModuleConfig cfg;
    cfg.sighting_debounce = 1ms;
    so::AttendanceModule m(cfg);

    so::CameraConfig cam;
    cam.id = 1;
    cam.tenant_id = 1;
    // No gate line anywhere, and none needed.
    Sink sink;

    auto seen = [&](float cy) {
        auto ctx = make_ctx(cam, {person(1, 90, cy - 20, 110, cy + 20)});
        so::FaceObs f;
        f.track_id = 1;
        f.employee_id = 7;
        f.employee_name = "Aslam";
        f.match_score = 0.5f;
        ctx.faces.push_back(f);
        m.on_frame(ctx, sink);
    };

    seen(40);
    REQUIRE(sink.attendance.size() == 1);
    CHECK(sink.attendance[0].employee_id == 7);
    CHECK(sink.attendance[0].direction == so::Direction::In);

    // Standing still is still being present — it used to cross nothing and so
    // counted as nothing at all.
    std::this_thread::sleep_for(3ms);
    seen(45);
    CHECK(sink.attendance.size() == 2);
}

TEST_CASE("one arrival, not one per frame") {
    // A person who stays in view is recognised continuously. Without the
    // debounce that is a row per frame — hundreds an hour, all saying the same
    // thing.
    so::AttendanceModule m;          // default sighting_debounce: 60 s
    so::CameraConfig cam;
    Sink sink;

    for (int i = 0; i < 8; ++i) {
        auto ctx = make_ctx(cam, {person(1, 90, 40, 110, 80)});
        so::FaceObs f;
        f.track_id = 1;
        f.employee_id = 7;
        ctx.faces.push_back(f);
        m.on_frame(ctx, sink);
    }
    CHECK(sink.attendance.size() == 1);
}

TEST_CASE("an anonymous crossing is traffic, not attendance") {
    so::AttendanceModule m;
    so::CameraConfig cam;
    so::GateLineCfg g;
    g.line = so::GateLine{{0, 100}, {200, 100}};
    cam.gate_lines.push_back(g);
    Sink sink;

    m.on_frame(make_ctx(cam, {person(1, 90, 20, 110, 60)}), sink);
    m.on_frame(make_ctx(cam, {person(1, 90, 120, 110, 160)}), sink);
    CHECK(sink.attendance.empty());
}

TEST_CASE("an identity settled earlier survives the person turning away") {
    so::AttendanceModuleConfig cfg;
    cfg.debounce = 1ms;
    so::AttendanceModule m(cfg);
    so::CameraConfig cam;
    so::GateLineCfg g;
    g.line = so::GateLine{{0, 100}, {200, 100}};
    cam.gate_lines.push_back(g);
    Sink sink;

    // Recognised well before the line...
    auto ctx = make_ctx(cam, {person(1, 90, 20, 110, 60)});
    so::FaceObs f;
    f.track_id = 1;
    f.employee_id = 7;
    ctx.faces.push_back(f);
    m.on_frame(ctx, sink);

    // ...and crosses it with their back to the camera, no face at all.
    m.on_frame(make_ctx(cam, {person(1, 90, 120, 110, 160)}), sink);

    REQUIRE(sink.attendance.size() == 1);
    CHECK(sink.attendance[0].employee_id == 7);
}

TEST_CASE("a seated person occupies their desk, feet or no feet") {
    // REGRESSION TEST FOR A REPORT THAT SAID PEOPLE WERE AWAY WHILE THEY SAT THERE.
    //
    // Occupancy was decided on the FOOT POINT, which is right for a perimeter —
    // somebody walking past the edge of a zone is not inside it — and wrong for
    // a desk. A seated person's feet are under the desk or behind the chair,
    // outside the shape drawn around the working area, so they read as absent
    // and flickered in and out: three people who sat down once each produced
    // 59, 67 and 41 "sessions" in a day, and their time away exceeded their
    // time at the desk.
    //
    // A desk asks whether the seat is taken, so it is answered by how much of
    // the person overlaps it.
    so::DeskConfig cfg;
    cfg.tick = 0ms;
    so::DeskDutyModule m(cfg);

    so::CameraConfig cam;
    cam.id = 1;
    cam.tenant_id = 1;
    so::Zone z;
    z.id = 1;
    z.type = so::ZoneType::Desk;
    z.enabled = true;
    z.employee_id = 7;
    z.polygon = {{100, 100}, {300, 100}, {300, 250}, {100, 250}};   // the desk area
    cam.zones.push_back(z);

    Sink sink;
    // Seated: the body overlaps the desk, but the feet land BELOW it — exactly
    // the case the foot point got wrong.
    auto ctx = make_ctx(cam, {person(1, 150, 120, 260, 300)});
    m.on_frame(ctx, sink);

    REQUIRE(!sink.ticks.empty());
    CHECK(sink.ticks.back().present);
    CHECK(sink.ticks.back().employee_id == 7);
}

TEST_CASE("somebody walking past a desk does not occupy it") {
    // The other half of the rule. Overlap must not be so generous that passing
    // traffic counts, or the report would show a desk busy all day.
    so::DeskConfig cfg;
    cfg.tick = 0ms;
    so::DeskDutyModule m(cfg);

    so::CameraConfig cam;
    cam.id = 1;
    so::Zone z;
    z.id = 1;
    z.type = so::ZoneType::Desk;
    z.enabled = true;
    z.employee_id = 7;
    z.polygon = {{100, 100}, {300, 100}, {300, 250}, {100, 250}};
    cam.zones.push_back(z);

    Sink sink;
    // Clipping the far edge only.
    auto ctx = make_ctx(cam, {person(1, 290, 90, 340, 260)});
    m.on_frame(ctx, sink);
    REQUIRE(!sink.ticks.empty());
    CHECK_FALSE(sink.ticks.back().present);
}

TEST_CASE("a desk counts nothing while its owner has not arrived") {
    // The desk credits whoever sits in it to the assigned employee, and does so
    // without checking a face every second — once somebody is at their own desk
    // they stay theirs, which is what makes the count cheap and steady. That
    // reasoning holds only while the employee is in the building: on a day they
    // never came in, the person in the seat is somebody else, and handing them
    // the hours in silence is worse than showing nothing.
    so::CameraConfig cam;
    cam.id = 1;
    cam.tenant_id = 1;
    so::Zone z;
    z.id = 1;
    z.type = so::ZoneType::Desk;
    z.enabled = true;
    z.employee_id = 7;
    z.polygon = {{0, 0}, {200, 0}, {200, 200}, {0, 200}};
    cam.zones.push_back(z);

    so::PresenceRoster roster;        // nobody has arrived

    so::DeskConfig cfg;
    cfg.tick = std::chrono::milliseconds(0);
    so::DeskDutyModule m(cfg);
    Sink sink;

    auto tick = [&] {
        auto ctx = make_ctx(cam, {person(1, 40, 40, 160, 160)});   // squarely inside
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
    };

    tick(); tick();
    CHECK(sink.ticks.empty());         // somebody is sitting there, but not them

    // Once they are recorded arriving, the same seat starts counting.
    roster.add(7);
    tick();
    REQUIRE_FALSE(sink.ticks.empty());
    CHECK(sink.ticks.back().present);
    CHECK(sink.ticks.back().employee_id == 7);
}

TEST_CASE("without a roster a desk behaves as it always did") {
    // A missing roster means "not known", not "absent". Treating it as absent
    // would make every desk in the product go quiet the moment the lookup
    // failed, which is a much worse failure than the one it guards against.
    so::CameraConfig cam;
    cam.id = 1;
    so::Zone z;
    z.id = 1;
    z.type = so::ZoneType::Desk;
    z.enabled = true;
    z.employee_id = 7;
    z.polygon = {{0, 0}, {200, 0}, {200, 200}, {0, 200}};
    cam.zones.push_back(z);

    so::DeskConfig cfg;
    cfg.tick = std::chrono::milliseconds(0);
    so::DeskDutyModule m(cfg);
    Sink sink;

    auto ctx = make_ctx(cam, {person(1, 40, 40, 160, 160)});
    ctx.presence = nullptr;
    m.on_frame(ctx, sink);
    REQUIRE_FALSE(sink.ticks.empty());
    CHECK(sink.ticks.back().present);
}

// ── out count ───────────────────────────────────────────────────────────────

namespace {

/// A face already matched to an employee, bound to a track.
so::FaceObs known_face(int track_id, std::int64_t employee_id) {
    so::FaceObs f;
    f.track_id = track_id;
    f.employee_id = employee_id;
    f.employee_name = "Eshita";
    f.match_score = 0.62f;
    f.score = 0.9f;
    return f;
}

/// Inside the 200x200 exit zone, standing on their feet at (100, 180).
so::Track in_zone(int id) { return person(id, 60, 20, 140, 180); }
/// Across the room: the BOX still overlaps the zone, the feet do not.
so::Track walking_past(int id) { return person(id, 150, 20, 230, 260); }

so::OutCountConfig fast() {
    so::OutCountConfig c;
    c.confirm = 30ms;
    c.gone = 60ms;
    c.debounce = 500ms;
    return c;
}

}  // namespace

TEST_CASE("leaving through the exit zone records an out, once the track is gone") {
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);                      // already counted in today

    so::OutCountModule m(fast());
    Sink sink;

    // Crossing the room towards the door. The zone is a doorway, not the whole
    // view, so anybody leaving is seen here first — and this is what separates
    // them from somebody arriving, whose track begins in the doorway itself.
    {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
    }

    // In the doorway, recognised, for longer than `confirm`.
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    // NOTHING YET. Standing in a doorway is not leaving — the person may be
    // holding it open for somebody.
    CHECK(sink.attendance.empty());

    // Now they are gone.
    std::this_thread::sleep_for(80ms);
    auto ctx = make_ctx(cfg, {});
    ctx.presence = &roster;
    m.on_frame(ctx, sink);

    REQUIRE(sink.attendance.size() == 1);
    CHECK(sink.attendance[0].employee_id == 7);
    CHECK(sink.attendance[0].direction == so::Direction::Out);

    // An `out` nobody is paged for, but one that shows on the timeline.
    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].module == std::string("out_count"));
    CHECK_FALSE(sink.events[0].notify);
}

TEST_CASE("somebody recorded arriving seconds ago is not recorded leaving") {
    // FROM THE FLOOR, 2026-10-29. An employee walked in through the main
    // entry: 08:40:17 `in` on one camera, 08:40:19 `out` on the other. A second
    // case was twelve seconds. Both were arrivals whose track was lost a moment
    // after they stepped inside — which, to a rule built on "stood in the
    // doorway, then stopped existing", is indistinguishable from leaving.
    //
    // Geometry alone cannot be trusted to prevent this: the zone may be drawn
    // short of the threshold, and a tracker may drop an id anywhere. The clock
    // can be. Nobody enters a building and leaves it two seconds later.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.mark_in(7);                  // recorded arriving just now

    so::OutCountModule m(fast());
    Sink sink;

    {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
    }
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto ctx = make_ctx(cfg, {});
    ctx.presence = &roster;
    m.on_frame(ctx, sink);

    // Everything the old rule asked for held — confirmed in the zone, came
    // from the room, named, present — and it is still refused.
    CHECK(sink.attendance.empty());
}

TEST_CASE("walking back into the room is not leaving") {
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;

    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }

    // They turn round and walk back in — still tracked, now outside the zone.
    // THIS is the case the `gone` delay exists to separate from leaving, and
    // the track reappearing settles it immediately.
    auto back = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
    back.presence = &roster;
    m.on_frame(back, sink);

    std::this_thread::sleep_for(80ms);
    auto later = make_ctx(cfg, {});
    later.presence = &roster;
    m.on_frame(later, sink);

    CHECK(sink.attendance.empty());
}

TEST_CASE("an out is never recorded for somebody who was never counted in") {
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster empty_roster;        // nobody has arrived today

    so::OutCountModule m(fast());
    Sink sink;
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &empty_roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &empty_roster;
    m.on_frame(gone, sink);

    // The day roll-up pairs punches in order. An `out` with no `in` before it
    // would read as a departure from a building this person never entered.
    CHECK(sink.attendance.empty());

    // A null roster is "the engine did not say", not "nobody is in", and must
    // be treated as just as unknown.
    so::OutCountModule m2(fast());
    Sink sink2;
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.faces = {known_face(1, 7)};
        m2.on_frame(ctx, sink2);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    m2.on_frame(make_ctx(cfg, {}), sink2);
    CHECK(sink2.attendance.empty());
}

TEST_CASE("an unrecognised person leaving is traffic, not an out") {
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});    // no faces at all
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    CHECK(sink.attendance.empty());
}

TEST_CASE("somebody walking past the doorway is not leaving through it") {
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;
    for (int i = 0; i < 3; ++i) {
        // The BOX overlaps the zone; the feet are outside it. Judging this on
        // overlap would book out everybody who crosses the far side of the room.
        auto ctx = make_ctx(cfg, {walking_past(1)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    CHECK(sink.attendance.empty());
}

TEST_CASE("a glance into the doorway is not an exit") {
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountConfig slow = fast();
    slow.confirm = 500ms;               // longer than this person stays
    so::OutCountModule m(slow);
    Sink sink;

    auto ctx = make_ctx(cfg, {in_zone(1)});
    ctx.faces = {known_face(1, 7)};
    ctx.presence = &roster;
    m.on_frame(ctx, sink);              // one frame only

    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    CHECK(sink.attendance.empty());
}

TEST_CASE("only an exit zone counts, not a perimeter one drawn on the same camera") {
    // Standing in a restricted zone is an ALARM; standing in an exit zone is
    // what everybody does on the way home. Sharing one zone kind between them
    // would make every intrusion zone a turnstile.
    const auto cfg = with_zone(so::ZoneType::Restricted);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    CHECK(sink.attendance.empty());
}

TEST_CASE("a face recognised anywhere on the camera still counts them out at the door") {
    // THE DOORWAY IS THE WORST PLACE TO READ A FACE and the only place this
    // module cares about. On the exit camera here a person at the door is about
    // eighty pixels tall, so their face is roughly twelve — far below the
    // twenty-four the matcher needs. Requiring the name to be legible AT the
    // door would mean this module never records anything on that camera.
    //
    // It does not have to be. The tracker follows one person across the whole
    // frame, and they walked towards the door from somewhere nearer the camera
    // where their face WAS big enough. The name is earned there and carried.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;

    // Recognised out in the room — NOT in the exit zone, face still readable.
    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }

    // Now at the door, back turned, no face readable at all.
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.presence = &roster;        // no faces this frame
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    REQUIRE(sink.attendance.size() == 1);
    CHECK(sink.attendance[0].employee_id == 7);
    CHECK(sink.attendance[0].direction == so::Direction::Out);
}

TEST_CASE("a name learned on one track is not inherited by the next one") {
    // Carrying identity across the frame is only sound while it stays bound to
    // ONE track. If a track is lost and a new one is created for somebody else
    // walking the same path, inheriting the name would book out whoever was
    // recognised last, every time anybody leaves.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;

    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};     // track 1 is Eshita
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }

    // A DIFFERENT track reaches the door, with no face of its own.
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(2)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    CHECK(sink.attendance.empty());
}

TEST_CASE("somebody arriving through the same door is never counted out") {
    // ONE DOOR, BOTH DIRECTIONS. The camera that watches the way in is also the
    // camera that watches the way out, so the module has to tell an arrival
    // from a departure using nothing but the track.
    //
    // The discriminator is WHERE THE TRACK BEGAN. Somebody leaving walked
    // across the room first, so their track exists outside the zone before it
    // enters it. Somebody arriving appears already in the doorway — their track
    // is BORN inside the zone. An arrival whose track is then lost at the
    // threshold, which happens when a second person blocks the view, would
    // otherwise be recorded as having left the moment they came in.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;

    // Track BORN in the doorway: this is somebody coming in from outside.
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    // ...and lost right there, before they reach the room.
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    CHECK(sink.attendance.empty());
}

TEST_CASE("somebody who crossed the room first is counted out") {
    // The other half of the same rule: this track existed OUTSIDE the zone
    // before entering it, so the person was in the room and has now left.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;

    // In the room, recognised on the way across — this is also where the name
    // is earned, because by the door their back is to the camera.
    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    // Then into the doorway, back turned, no face at all.
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    REQUIRE(sink.attendance.size() == 1);
    CHECK(sink.attendance[0].employee_id == 7);
    CHECK(sink.attendance[0].direction == so::Direction::Out);
}

TEST_CASE("crossing out of the zone towards the door still counts, even seen for a moment") {
    // THE ZONE IS DRAWN ON THE FLOOR IN FRONT OF THE DOORS, not across the
    // doorway itself. So somebody leaving crosses the zone, steps out of it on
    // the door side, is visible there for a moment, and only then goes through.
    //
    // Forgetting them the instant they leave the zone — which is what the first
    // version did — loses exactly that person: by the time they disappear there
    // is no candidate left to record. The leaving has to survive the gap
    // between the far edge of the zone and the door.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;

    // Crossing the room, recognised on the way.
    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    // Standing in the zone.
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    // PAST the far edge, at the doors, still visible for a frame or two.
    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 210, 20, 260, 190)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    // Through the doors.
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    REQUIRE(sink.attendance.size() == 1);
    CHECK(sink.attendance[0].employee_id == 7);
    CHECK(sink.attendance[0].direction == so::Direction::Out);
}

TEST_CASE("turning back into the room still cancels the exit") {
    // The other side of the same change: keeping the candidate alive after the
    // zone must NOT turn "changed their mind" into a departure. The difference
    // is that this track keeps being seen, in the room, for longer than anyone
    // takes to step through a door.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountConfig c = fast();
    c.linger = 60ms;              // how long outside the zone before it is a change of mind
    so::OutCountModule m(c);
    Sink sink;

    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    // Back into the room, and STAYS there, in view.
    for (int i = 0; i < 8; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    CHECK(sink.attendance.empty());
}

namespace {
/// A face the recogniser could NOT name, but which most resembled someone.
so::FaceObs weak_face(int track_id, std::int64_t guess, float score) {
    so::FaceObs f;
    f.track_id = track_id;
    f.employee_id = 0;              // below the bar — no name
    f.score = 0.9f;
    f.face_px = 31.0f;
    f.box = so::Box{70, 30, 101, 61};
    f.guess_employee_id = guess;
    f.guess_score = score;
    f.guess_margin = 0.05f;
    f.guess_weight = 0.4f;
    return f;
}
}  // namespace

TEST_CASE("an exit nobody could name is kept as a question, not dropped") {
    // THE CASE THIS WHOLE PATH EXISTS FOR. Somebody walks out; the face is far
    // away and turning; the pooled evidence never reaches the bar. Recording
    // nothing loses the exit AND the only look the system will ever get at
    // them. Recording a guess as fact would put the wrong name on a timesheet.
    //
    // So neither: the exit is kept as an unanswered question, carrying the face
    // and what it most resembled, for a person to confirm or dismiss.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;
    cv::Mat frame(200, 200, CV_8UC3, cv::Scalar(30, 30, 30));

    // Crossing the room, seen but never named.
    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)}, &frame);
        ctx.faces = {weak_face(1, 7, 0.41f)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    // In the doorway, then gone.
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)}, &frame);
        ctx.faces = {weak_face(1, 7, 0.41f)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {}, &frame);
    gone.presence = &roster;
    m.on_frame(gone, sink);

    // NO attendance was written — the system did not guess.
    CHECK(sink.attendance.empty());

    // But the question was kept, with the evidence.
    REQUIRE(sink.reviews.size() == 1);
    const auto& r = sink.reviews[0];
    CHECK(r.guess_employee_id == 7);
    CHECK(r.guess_score == doctest::Approx(0.41f));
    CHECK(r.track_id == 1);
    CHECK_FALSE(r.crop.empty());        // the face, to recognise them by
    CHECK_FALSE(r.snapshot.empty());    // the frame, to see where they went
}

TEST_CASE("an exit that WAS named raises no question") {
    // The review list has to stay worth reading. An exit the system is sure
    // about is attendance and nothing else; putting it in the queue as well
    // would bury the ones that actually need a human.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;
    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});
    gone.presence = &roster;
    m.on_frame(gone, sink);

    CHECK(sink.attendance.size() == 1);
    CHECK(sink.reviews.empty());
}

TEST_CASE("somebody still standing there has not left, whatever their track id") {
    // CONFIRMED FALSE EXIT, 27 OCTOBER. Mehedi arrived at 10:18:00 and was
    // booked out at 10:18:12. He had not left — he was standing at the
    // reception desk, which is where the far edge of the exit zone falls.
    //
    // What actually happened is track churn: his track was lost for a moment
    // and the tracker opened a NEW one for the same person standing in the same
    // place. The old candidate then timed out with nobody to contradict it, and
    // "the track is gone" was read as "the person is gone".
    //
    // They are not the same thing, and the frame says so: somebody is standing
    // right where the vanished track was last seen. A person who walked out
    // leaves empty floor behind them.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;

    // Across the room, recognised, then into the zone.
    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }

    // Track 1 is gone — but track 2 is standing in the same spot. Same person,
    // new id.
    std::this_thread::sleep_for(80ms);
    auto churn = make_ctx(cfg, {in_zone(2)});
    churn.presence = &roster;
    m.on_frame(churn, sink);

    CHECK(sink.attendance.empty());
    CHECK(sink.reviews.empty());
}

TEST_CASE("empty floor behind them still counts as leaving") {
    // The other side of the same rule: it must not become "never record an exit
    // while anyone is near the door", or a second person walking up behind
    // somebody would cancel their departure. Nobody is there at all here.
    const auto cfg = with_zone(so::ZoneType::Exit);
    so::PresenceRoster roster;
    roster.add(7);

    so::OutCountModule m(fast());
    Sink sink;
    for (int i = 0; i < 2; ++i) {
        auto ctx = make_ctx(cfg, {person(1, 300, 20, 380, 260)});
        ctx.faces = {known_face(1, 7)};
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(15ms);
    }
    for (int i = 0; i < 3; ++i) {
        auto ctx = make_ctx(cfg, {in_zone(1)});
        ctx.presence = &roster;
        m.on_frame(ctx, sink);
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(80ms);
    auto gone = make_ctx(cfg, {});          // nobody anywhere
    gone.presence = &roster;
    m.on_frame(gone, sink);

    REQUIRE(sink.attendance.size() == 1);
    CHECK(sink.attendance[0].direction == so::Direction::Out);
}
