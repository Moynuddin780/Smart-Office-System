#pragma once

#include "api/auth.hpp"
#include "db/database.hpp"
#include "identity/gallery.hpp"
#include "modules/module.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace so::db {

/**
 * A tenant id, as a distinct type.
 *
 * Every tenant-scoped repository method takes this as its FIRST parameter and
 * there is no overload without it. A handler therefore cannot forget the
 * filter — the call it would have to write does not exist — and cannot pass a
 * raw id it lifted out of a request body by accident, because that is an
 * int64_t and this is not.
 */
struct TenantId {
    std::int64_t value{0};
    explicit TenantId(std::int64_t v) : value(v) {}
    std::string str() const { return std::to_string(value); }
};

// ── rows ───────────────────────────────────────────────────────────────────

struct Tenant {
    std::int64_t id{0};
    std::string name, slug, status, plan;
    int max_cameras{8}, max_employees{200};
    std::string created_at;
    /// The company's first administrator, and a count of what is inside.
    /// A vendor console that lists companies without saying how to get into
    /// one, or whether anything has been set up in it, is a list of names.
    std::string admin_email;
    int cameras{0}, employees{0};
};

struct User {
    std::int64_t id{0};
    std::int64_t tenant_id{0};
    std::string email, password_hash, name, role;
    std::int64_t employee_id{0};
    bool active{true};
};

/// One enrolment photo, with the embedding it produced.
struct FaceImage {
    std::int64_t id{0};
    std::vector<float> embedding;
    int quality{0};
    float yaw{0}, face_px{0}, det_score{0};
    std::string image_path, filename, source, created_at;
    int weight{1};
};

/// The recomputed profile: one vector per person.
struct FaceProfile {
    std::int64_t employee_id{0};
    std::vector<float> embedding;
    int image_count{0};
    int quality{0};
    /// Median pairwise cosine between this profile's own images; < 0 = unknown
    /// (fewer than two images). Below ~0.50 the profile is incoherent and will
    /// start handing its name to other people.
    float coherence{-1.0f};
};

/// Another enrolled person this profile is at risk of being confused with.
struct NearNeighbour {
    std::int64_t employee_id{0};
    std::string name;
    float similarity{0};
};

struct Employee {
    std::int64_t id{0};
    std::string emp_code, name, department, designation, email, phone;
    bool active{true};
    int face_profiles{0};
};

struct CameraRow {
    std::int64_t id{0};
    std::string name, rtsp_main, rtsp_sub, stream_pref, role;
    /// Where it is. `site` groups (a building or branch), `location` describes
    /// the spot inside it. See migrations/0006_camera_place.sql.
    std::string site, location;
    bool face_enabled{false}, enabled{true};
    int detect_hz{10}, face_hz{6};
    int probed_width{0}, probed_height{0};
    /// Longest side the pipeline will carry for this camera, in pixels. The
    /// short side follows from the stream's aspect ratio. See
    /// migrations/0007_camera_max_px.sql for why this is per camera.
    int max_px{1000};
    /// Frames per second the pipeline will carry, 0 = every frame. Needed
    /// because a camera may ignore its own frame-rate setting — see
    /// migrations/0010_camera_max_fps.sql.
    int max_fps{0};
};

/// An exit the system could not name, waiting for somebody to answer it.
struct ExitReviewRow {
    std::int64_t id{0};
    std::int64_t camera_id{0}, zone_id{0};
    std::string camera_name;
    std::string ts;              ///< HH:MM on the day
    std::string work_date;
    std::int64_t guess_employee_id{0};
    std::string guess_name, guess_code;
    float guess_score{0}, guess_margin{0}, guess_weight{0};
    std::string crop_path, frame_path;
};

struct Settings {
    std::string timezone{"Asia/Dhaka"};
    std::string office_start{"09:00:00"}, office_end{"18:00:00"};
    int grace_minutes{15}, early_leave_grace_minutes{15};
    /// Shortest absence that counts as a break, in seconds. See
    /// migrations/0012_min_break.sql for why it is a setting.
    int min_break_seconds{60};
    std::string telegram_bot_token, telegram_chat_id;
    std::string smtp_host, smtp_user, smtp_password, smtp_from, smtp_from_name, email_to;
    int smtp_port{587};
};

struct DayRow {
    std::int64_t employee_id{0};
    std::string emp_code, name, department;
    std::string first_in, last_out, status;
    int gross_seconds{0}, inside_seconds{0}, break_seconds{0}, entries{0};
    bool assumed_out{false};
};

struct DeskRow {
    std::int64_t zone_id{0}, employee_id{0};
    std::string zone_name, employee_name;
    int occupied_seconds{0}, absent_seconds{0}, longest_absence_seconds{0};
    int sessions_count{0}, mismatch_seconds{0};
    /// Was this employee recorded as present at all on this day?
    ///
    /// Desk time for somebody who never arrived is somebody else sitting
    /// there, and a report that credits it to them without saying so is worse
    /// than one that shows nothing.
    bool employee_present{false};
};

struct EventRow {
    std::int64_t id{0}, camera_id{0}, zone_id{0}, employee_id{0};
    std::string module, severity, title, detail, ts, snapshot_path, crop_path;
    /// Denormalised for display. An alert is read by somebody who has to
    /// decide whether to walk somewhere, and a camera name alone does not
    /// answer that.
    std::string camera_site, camera_location;
    std::string camera_name;
    bool acknowledged{false};
};

// ── repositories ───────────────────────────────────────────────────────────

class Repos {
public:
    explicit Repos(Database& db) : db_(db) {}

    // Cross-tenant, SuperAdmin only. Named so that a reader can see at the call
    // site that this one is not tenant-scoped.
    std::vector<Tenant> all_tenants();
    Tenant create_tenant(const std::string& name, const std::string& slug);
    void set_tenant_status(std::int64_t id, const std::string& status);
    void delete_tenant(std::int64_t id);

    // Auth. Lookup is by email across the whole table because that is what a
    // login form gives us; the tenant comes OUT of the row, never in.
    std::optional<User> find_user_by_email(const std::string& email);
    std::optional<User> find_user_by_id(std::int64_t id);
    std::int64_t create_user(std::optional<std::int64_t> tenant_id,
                             const std::string& email, const std::string& hash,
                             const std::string& name, const std::string& role,
                             std::optional<std::int64_t> employee_id = std::nullopt);
    void update_password(std::int64_t user_id, const std::string& hash);
    bool any_superadmin();

    // Tenant-scoped.
    std::vector<CameraRow> cameras(TenantId t);
    std::optional<CameraRow> camera(TenantId t, std::int64_t id);
    std::int64_t create_camera(TenantId t, const CameraRow& c);
    void update_camera(TenantId t, std::int64_t id, const CameraRow& c);
    void delete_camera(TenantId t, std::int64_t id);

    std::vector<Zone> zones(TenantId t, std::int64_t camera_id = 0);
    std::int64_t create_zone(TenantId t, std::int64_t camera_id, const Zone& z);
    void update_zone(TenantId t, std::int64_t id, const Zone& z);
    void delete_zone(TenantId t, std::int64_t id);

    /// Which modules run on a camera. A module with no row defaults to ON,
    /// so adding a camera does not require visiting a settings page before it
    /// does anything.
    std::map<std::string, bool> camera_modules(TenantId t, std::int64_t camera_id);
    void set_camera_module(TenantId t, std::int64_t camera_id,
                           const std::string& module, bool enabled);

    std::vector<GateLineCfg> gate_lines(TenantId t, std::int64_t camera_id = 0);
    std::int64_t create_gate_line(TenantId t, std::int64_t camera_id, const GateLineCfg& g);
    void delete_gate_line(TenantId t, std::int64_t id);

    std::vector<Employee> employees(TenantId t, bool active_only = false);
    std::optional<Employee> employee(TenantId t, std::int64_t id);
    std::int64_t create_employee(TenantId t, const Employee& e);
    void update_employee(TenantId t, std::int64_t id, const Employee& e);
    void delete_employee(TenantId t, std::int64_t id);

    /// The employee holding this code in this tenant, if any.
    ///
    /// Exists so the API can refuse a duplicate with a sentence instead of
    /// letting the unique index raise and surfacing it as a 500. A clash on
    /// employee code is somebody typing a code that is taken — ordinary, and
    /// not a server fault.
    std::optional<Employee> employee_by_code(TenantId t, const std::string& code);

    // ── face registry ──────────────────────────────────────────────────
    // A profile is ONE vector per person, recomputed from the photos that
    // built it. The photos are kept because a bad profile is repaired by
    // deleting two of them, and because the agreement BETWEEN them is the only
    // signal that says a profile is bad at all.
    std::int64_t add_face_image(TenantId t, std::int64_t employee_id,
                                const std::vector<float>& embedding, int quality,
                                float yaw, float face_px, float det_score,
                                const std::string& image_path,
                                const std::string& filename, int size_bytes,
                                const std::string& source);
    std::vector<FaceImage> face_images(TenantId t, std::int64_t employee_id);
    /// Replace one image's embedding in place, keeping its photo and metadata.
    ///
    /// This exists for a CHANGE OF RECOGNITION MODEL. Embeddings from two
    /// models cannot be averaged or compared — a cross-model cosine gives
    /// confident WRONG matches rather than no match — so when the model
    /// changes every stored vector has to be recomputed from the photo it came
    /// from, and a profile must never be left holding a mixture.
    void update_face_embedding(TenantId t, std::int64_t id,
                               const std::vector<float>& embedding);
    std::optional<FaceImage> face_image(TenantId t, std::int64_t id);
    void delete_face_image(TenantId t, std::int64_t id);
    void delete_face_images(TenantId t, std::int64_t employee_id);

    /// Rebuild the profile from whatever images it currently has. No NPU: this
    /// is a weighted mean and a median, and it runs after every change so the
    /// stored profile is never out of step with its photos.
    FaceProfile recompute_profile(TenantId t, std::int64_t employee_id);

    /// Employees recorded arriving today, for the desk module's check that the
    /// owner of a desk is actually in the building.
    std::vector<std::int64_t> employees_present_today(TenantId t);
    std::optional<FaceProfile> profile(TenantId t, std::int64_t employee_id);
    std::vector<FaceProfile> profiles(TenantId t);

    /// Everyone this profile sits close enough to be confused with.
    std::vector<NearNeighbour> too_close(TenantId t, std::int64_t employee_id,
                                         float threshold);

    std::vector<EnrolledProfile> gallery(TenantId t);
    /// Every tenant's profiles at once — what the running engine needs, since
    /// one appliance serves them all.
    std::vector<std::pair<std::int64_t, EnrolledProfile>> gallery_all();

    Settings settings(TenantId t);
    void save_settings(TenantId t, const Settings& s);

    void record_attendance(TenantId t, std::int64_t employee_id, std::int64_t camera_id,
                           const std::string& direction, const std::string& ts_iso,
                           const std::string& work_date, float confidence,
                           int track_id, const std::string& snapshot_path);
    /// Recompute a day from its events. The day table is a cache, so a
    /// mis-recorded punch is deleted and this is re-run, never hand-edited.
    void recompute_day(TenantId t, std::int64_t employee_id, const std::string& work_date);
    /// Every work_date that has attendance events, within an optional range.
    /// Empty bounds mean today, which is the day somebody is looking at when
    /// they notice a total is wrong.
    std::vector<std::string> work_dates(TenantId t, const std::string& from,
                                        const std::string& to);
    /// Everybody with at least one event on that date.
    std::vector<std::int64_t> employees_with_events(TenantId t, const std::string& work_date);
    /// Today's days that have NOT been closed by a real exit, as
    /// (employee_id, work_date). These are the only rows whose totals still
    /// move, so they are the only ones worth re-reading on a timer.
    std::vector<std::pair<std::int64_t, std::string>> open_days_today(TenantId t);

    // ── exits nobody could name ────────────────────────────────────────────
    void record_exit_review(TenantId t, std::int64_t camera_id, std::int64_t zone_id,
                            const std::string& ts_iso, const std::string& work_date,
                            int track_id, std::int64_t guess_employee_id,
                            float score, float margin, float weight,
                            const std::string& crop_path, const std::string& frame_path);
    std::vector<ExitReviewRow> open_exit_reviews(TenantId t, const std::string& work_date);
    /// Answer one. A non-zero employee turns it into an `out`; zero dismisses
    /// it as a false exit. Returns the review's details so the caller can write
    /// the attendance event with the right camera and timestamp.
    std::optional<ExitReviewRow> resolve_exit_review(TenantId t, std::int64_t id,
                                                     std::int64_t employee_id,
                                                     std::int64_t user_id);
    std::vector<DayRow> attendance_day(TenantId t, const std::string& work_date);
    std::vector<DayRow> attendance_range(TenantId t, std::int64_t employee_id,
                                         const std::string& from, const std::string& to);

    void upsert_desk_day(TenantId t, std::int64_t zone_id, std::int64_t employee_id,
                         const std::string& work_date, int occupied_delta,
                         int absent_delta, int mismatch_delta,
                         int sessions_delta, int longest_absence);
    std::vector<DeskRow> desk_day(TenantId t, const std::string& work_date);

    std::int64_t record_event(TenantId t, const EventRow& e);
    std::vector<EventRow> events(TenantId t, const std::string& module,
                                 const std::string& from, const std::string& to,
                                 int limit);
    void acknowledge_event(TenantId t, std::int64_t id, std::int64_t user_id);

    void enqueue_notification(TenantId t, std::int64_t event_id, const std::string& channel);

    Database& raw() { return db_; }

private:
    Database& db_;
};

}  // namespace so::db
