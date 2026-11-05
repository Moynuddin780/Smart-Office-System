#pragma once

#include "vision/arcface.hpp"
#include "identity/presence.hpp"
#include "vision/geometry.hpp"
#include "vision/scrfd.hpp"
#include "vision/tracker.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace so {

using TimePoint = std::chrono::system_clock::time_point;

// ── configuration a camera carries ─────────────────────────────────────────

/// Exit is a zone somebody LEAVES through, not one they must stay out of.
/// It is a separate kind rather than a flag on Perimeter because the two mean
/// opposite things: standing in a restricted zone is the alarm, while standing
/// in an exit zone is ordinary and only the leaving is recorded.
enum class ZoneType { Perimeter, Restricted, Desk, Exit };

struct Zone {
    std::int64_t id{0};
    ZoneType type{ZoneType::Perimeter};
    std::string name;
    Polygon polygon;                 ///< SOURCE-resolution coordinates
    std::int64_t employee_id{0};     ///< desk zones only; 0 = unassigned
    std::string employee_name;
    std::string employee_code;
    bool enabled{true};
};

struct GateLineCfg {
    std::int64_t id{0};
    std::string name;
    GateLine line;                   ///< SOURCE-resolution coordinates
    bool enabled{true};
};

enum class CameraRole { Area, Gate };

struct CameraConfig {
    std::int64_t id{0};
    std::int64_t tenant_id{0};
    std::string name;
    CameraRole role{CameraRole::Area};
    bool face_enabled{false};        ///< run SCRFD + ArcFace on this camera
    std::vector<Zone> zones;
    std::vector<GateLineCfg> gate_lines;
    std::map<std::string, bool> modules;   ///< module id -> enabled
    int detect_hz{10};
    int face_hz{6};
};

// ── what a module sees ─────────────────────────────────────────────────────

/// A face observed this frame, already bound to a person track where possible.
struct FaceObs {
    Box box{};                                    ///< SOURCE space
    float score{0};
    std::array<Landmark, kNumLandmarks> lm{};     ///< SOURCE space
    int track_id{-1};                             ///< -1 = not bound to a person
    float yaw{0};                                 ///< eye-widths; 0 is square on
    float face_px{0};                             ///< box width, the size gate
    std::int64_t employee_id{0};                  ///< 0 = not identified
    std::string employee_name;
    float match_score{0};
    /// What the pooled evidence most resembles, REGARDLESS of whether it was
    /// enough to name anybody. Carried so a module can keep the evidence for
    /// somebody who leaves before the bar is reached; never a verdict.
    std::int64_t guess_employee_id{0};
    float guess_score{0}, guess_margin{0}, guess_weight{0};
};

struct FrameContext {
    std::int64_t tenant_id{0};
    std::int64_t camera_id{0};
    TimePoint ts{};
    const cv::Mat* frame{nullptr};                ///< SOURCE-resolution BGR
    std::vector<Track> tracks;                    ///< SOURCE space
    std::vector<FaceObs> faces;                   ///< SOURCE space
    const CameraConfig* cfg{nullptr};
    /// Who has been recorded arriving today. Null when the engine did not
    /// supply one, in which case a module must not assume anybody is absent.
    const PresenceRoster* presence{nullptr};
};

// ── what a module produces ─────────────────────────────────────────────────

enum class Severity { Info, Warning, Critical };

const char* to_string(Severity s);

struct Event {
    std::int64_t tenant_id{0};
    std::int64_t camera_id{0};
    std::string module;
    Severity severity{Severity::Info};
    std::string title;
    std::string detail;
    TimePoint ts{};
    std::int64_t zone_id{0};
    std::int64_t employee_id{0};
    int track_id{-1};
    cv::Mat snapshot;      ///< annotated full frame; empty if none
    cv::Mat crop;          ///< the person, for the notification thumbnail
    bool notify{true};     ///< false for bookkeeping events nobody needs paging for
};

/// Attendance and desk occupancy are not "events"; they are running state that
/// a module updates. They travel on the same sink so a module needs one output.
enum class Direction { In, Out };

struct AttendanceEvent {
    std::int64_t tenant_id{0};
    std::int64_t camera_id{0};
    std::int64_t employee_id{0};
    Direction direction{Direction::In};
    TimePoint ts{};
    float confidence{0};
    int track_id{-1};
    cv::Mat snapshot;
};

struct DeskTick {
    std::int64_t tenant_id{0};
    std::int64_t zone_id{0};
    std::int64_t employee_id{0};
    TimePoint ts{};
    bool present{false};
    std::int64_t observed_employee_id{0};   ///< who was actually recognised, if anyone
};

/// An exit nobody could be named for, kept so the question can be answered.
///
/// NOT attendance. A person walking out is the hardest face this system ever
/// sees — furthest away, turning, and with no second chance once the door
/// closes — so the evidence very often falls short of the bar for naming them.
/// Until now that evidence was discarded and the exit vanished with it.
///
/// This carries what there was: the crop, the identity it most resembled, and
/// how thin that resemblance was. It becomes an attendance record only when a
/// person confirms it.
struct ExitReview {
    std::int64_t tenant_id{0};
    std::int64_t camera_id{0};
    std::int64_t zone_id{0};
    TimePoint ts{};
    int track_id{-1};
    /// Who the pooled evidence most resembled. 0 when no face was ever read.
    std::int64_t guess_employee_id{0};
    float guess_score{0}, guess_margin{0}, guess_weight{0};
    cv::Mat crop;        ///< the face, for recognising the person
    cv::Mat snapshot;    ///< the frame, for seeing where they were going
};

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void emit(Event e) = 0;
    virtual void emit(AttendanceEvent e) = 0;
    virtual void emit(DeskTick t) = 0;
    virtual void emit(ExitReview r) = 0;
};

// ── the module contract ────────────────────────────────────────────────────

class IModule {
public:
    virtual ~IModule() = default;

    /// Stable identifier, matching the key in CameraConfig::modules and the
    /// `module` column in the events table.
    virtual const char* id() const = 0;

    /// Called once per postprocessed frame, on a postprocess worker thread.
    /// Must not block: it shares its pool with every other camera.
    virtual void on_frame(const FrameContext& ctx, EventSink& sink) = 0;

    /// Configuration changed (a zone was redrawn, an employee reassigned).
    virtual void reconfigure(const CameraConfig&) {}
};

using ModulePtr = std::unique_ptr<IModule>;

}  // namespace so
