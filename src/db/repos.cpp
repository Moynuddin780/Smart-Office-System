#include "db/repos.hpp"

#include <iomanip>
#include <stdexcept>

#include "core/log.hpp"
#include "domain/attendance_rollup.hpp"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace so::db {
namespace {

std::string S(std::int64_t v) { return std::to_string(v); }

/// Floats get their OWN NAME, not an overload of S().
///
/// An overload makes every S(int) call in this file ambiguous — int converts to
/// int64_t and to double at the same rank — and there are dozens of them. A
/// separate name also makes the truncation impossible to reintroduce by
/// accident: passing a score to S() no longer compiles into silence, it is
/// simply the wrong function to reach for.
///
/// Every score passed through S() reached the database as an integer: a match
/// of 0.41 was stored as 0, a weight of 1.4 as 1. The review page then showed
/// "score 0 · margin 0 · weight 0" beside a confident-looking name, which is
/// the exact opposite of its purpose — it exists so a reviewer can see how thin
/// the evidence is.
std::string F(double v) {
    std::ostringstream o;
    o << std::setprecision(6) << v;
    return o.str();
}
std::string B(bool v) { return v ? "true" : "false"; }

/// Polygon <-> JSON. Deliberately hand-rolled for this one fixed shape
/// ([[x,y],...]) rather than pulling in a JSON library for the database layer;
/// the API layer has one for request bodies where the shape is not ours.
std::string polygon_to_json(const Polygon& p) {
    std::ostringstream s;
    s << '[';
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (i) s << ',';
        s << '[' << p[i].x << ',' << p[i].y << ']';
    }
    s << ']';
    return s.str();
}

Polygon polygon_from_json(const std::string& j) {
    Polygon out;
    float x = 0;
    bool have_x = false;
    std::size_t i = 0;
    while (i < j.size()) {
        if (j[i] == '-' || j[i] == '.' || (j[i] >= '0' && j[i] <= '9')) {
            std::size_t n = 0;
            const float v = std::stof(j.substr(i), &n);
            i += n;
            if (!have_x) {
                x = v;
                have_x = true;
            } else {
                out.emplace_back(x, v);
                have_x = false;
            }
        } else {
            ++i;
        }
    }
    return out;
}

ZoneType zone_type_from(const std::string& s) {
    if (s == "desk") return ZoneType::Desk;
    if (s == "exit") return ZoneType::Exit;
    if (s == "restricted") return ZoneType::Restricted;
    return ZoneType::Perimeter;
}

const char* zone_type_to(ZoneType t) {
    switch (t) {
        case ZoneType::Desk: return "desk";
        case ZoneType::Exit: return "exit";
        case ZoneType::Restricted: return "restricted";
        case ZoneType::Perimeter: return "perimeter";
    }
    return "perimeter";
}

}  // namespace

// ── tenants ────────────────────────────────────────────────────────────────

std::vector<Tenant> Repos::all_tenants() {
    const auto r = db_.query(
        "SELECT t.id,t.name,t.slug,t.status,t.plan,t.max_cameras,t.max_employees,"
        "t.created_at,"
        "COALESCE((SELECT u.email FROM users u WHERE u.tenant_id=t.id AND u.role='admin'"
        "          ORDER BY u.id LIMIT 1),''),"
        "(SELECT count(*) FROM cameras c WHERE c.tenant_id=t.id),"
        "(SELECT count(*) FROM employees e WHERE e.tenant_id=t.id) "
        "FROM tenants t ORDER BY t.name");
    std::vector<Tenant> out;
    for (int i = 0; i < r.rows(); ++i) {
        Tenant t;
        t.id = r.integer(i, 0);
        t.name = r.text(i, 1);
        t.slug = r.text(i, 2);
        t.status = r.text(i, 3);
        t.plan = r.text(i, 4);
        t.max_cameras = static_cast<int>(r.integer(i, 5));
        t.max_employees = static_cast<int>(r.integer(i, 6));
        t.created_at = r.text(i, 7);
        t.admin_email = r.text(i, 8);
        t.cameras = static_cast<int>(r.integer(i, 9));
        t.employees = static_cast<int>(r.integer(i, 10));
        out.push_back(std::move(t));
    }
    return out;
}

Tenant Repos::create_tenant(const std::string& name, const std::string& slug) {
    const auto r = db_.query(
        "INSERT INTO tenants(name,slug) VALUES ($1,$2) "
        "RETURNING id,name,slug,status,plan,max_cameras,max_employees,created_at",
        {name, slug});
    if (r.rows() != 1) throw Error("tenant insert returned no row");
    // Every tenant gets its settings row immediately, so no later code has to
    // handle "the settings do not exist yet".
    db_.query("INSERT INTO tenant_settings(tenant_id) VALUES ($1) ON CONFLICT DO NOTHING",
              {S(r.integer(0, 0))});
    Tenant t;
    t.id = r.integer(0, 0);
    t.name = r.text(0, 1);
    t.slug = r.text(0, 2);
    t.status = r.text(0, 3);
    t.plan = r.text(0, 4);
    t.max_cameras = static_cast<int>(r.integer(0, 5));
    t.max_employees = static_cast<int>(r.integer(0, 6));
    t.created_at = r.text(0, 7);
    return t;
}

void Repos::set_tenant_status(std::int64_t id, const std::string& status) {
    db_.query("UPDATE tenants SET status=$2 WHERE id=$1", {S(id), status});
}

void Repos::delete_tenant(std::int64_t id) {
    db_.query("DELETE FROM tenants WHERE id=$1", {S(id)});
}

// ── users ──────────────────────────────────────────────────────────────────

namespace {
std::optional<User> user_from(const Result& r, int i) {
    if (r.rows() <= i) return std::nullopt;
    User u;
    u.id = r.integer(i, 0);
    u.tenant_id = r.integer(i, 1, 0);
    u.email = r.text(i, 2);
    u.password_hash = r.text(i, 3);
    u.name = r.text(i, 4);
    u.role = r.text(i, 5);
    u.employee_id = r.integer(i, 6, 0);
    u.active = r.boolean(i, 7, true);
    return u;
}
const char* kUserCols =
    "id,tenant_id,email,password_hash,name,role,employee_id,active";
}  // namespace

std::optional<User> Repos::find_user_by_email(const std::string& email) {
    const auto r = db_.query(std::string("SELECT ") + kUserCols +
                                 " FROM users WHERE lower(email)=lower($1)",
                             {email});
    return user_from(r, 0);
}

std::optional<User> Repos::find_user_by_id(std::int64_t id) {
    const auto r = db_.query(std::string("SELECT ") + kUserCols + " FROM users WHERE id=$1",
                             {S(id)});
    return user_from(r, 0);
}

std::int64_t Repos::create_user(std::optional<std::int64_t> tenant_id,
                                const std::string& email, const std::string& hash,
                                const std::string& name, const std::string& role,
                                std::optional<std::int64_t> employee_id) {
    std::vector<std::optional<std::string>> p{
        tenant_id ? std::optional<std::string>(S(*tenant_id)) : std::nullopt,
        email, hash, name, role,
        employee_id ? std::optional<std::string>(S(*employee_id)) : std::nullopt};
    const auto r = db_.query_opt(
        "INSERT INTO users(tenant_id,email,password_hash,name,role,employee_id) "
        "VALUES ($1,$2,$3,$4,$5,$6) RETURNING id",
        p);
    return r.integer(0, 0);
}

void Repos::update_password(std::int64_t user_id, const std::string& hash) {
    db_.query("UPDATE users SET password_hash=$2 WHERE id=$1", {S(user_id), hash});
}

bool Repos::any_superadmin() {
    return db_.query("SELECT 1 FROM users WHERE role='superadmin' LIMIT 1").rows() > 0;
}

// ── cameras ────────────────────────────────────────────────────────────────

namespace {
const char* kCamCols =
    "id,name,rtsp_main,rtsp_sub,stream_pref,role,face_enabled,enabled,"
    "detect_hz,face_hz,probed_width,probed_height,site,location,max_px,max_fps";

/// How many columns kCamCols names. Checked, not trusted.
///
/// `Result::integer(row, col, dflt)` answers with the default when the column
/// is out of range, which is the right behaviour for a missing value and a trap
/// for a missing COLUMN. max_fps was added to CameraRow, to the migration, to
/// the UI and to the pipeline, and the one edit that failed to apply was the
/// one that appends it to this list — so every camera read back max_fps=0, the
/// frame-rate limit silently did nothing on a correct database, and the only
/// visible symptom was a pipeline string with no videorate in it.
constexpr int kCamColCount = 16;

CameraRow cam_from(const Result& r, int i) {
    if (r.cols() < kCamColCount)
        throw std::runtime_error("cameras query returned " + std::to_string(r.cols()) +
                                 " columns, expected " + std::to_string(kCamColCount) +
                                 " — kCamCols and cam_from disagree");
    CameraRow c;
    c.id = r.integer(i, 0);
    c.name = r.text(i, 1);
    c.rtsp_main = r.text(i, 2);
    c.rtsp_sub = r.text(i, 3);
    c.stream_pref = r.text(i, 4);
    c.role = r.text(i, 5);
    c.face_enabled = r.boolean(i, 6);
    c.enabled = r.boolean(i, 7, true);
    c.detect_hz = static_cast<int>(r.integer(i, 8, 10));
    c.face_hz = static_cast<int>(r.integer(i, 9, 6));
    c.probed_width = static_cast<int>(r.integer(i, 10, 0));
    c.probed_height = static_cast<int>(r.integer(i, 11, 0));
    c.site = r.text(i, 12);
    c.location = r.text(i, 13);
    c.max_px = static_cast<int>(r.integer(i, 14, 1000));
    c.max_fps = static_cast<int>(r.integer(i, 15, 0));
    return c;
}
}  // namespace

std::vector<CameraRow> Repos::cameras(TenantId t) {
    const auto r = db_.query(std::string("SELECT ") + kCamCols +
                                 " FROM cameras WHERE tenant_id=$1 ORDER BY name",
                             {t.str()});
    std::vector<CameraRow> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back(cam_from(r, i));
    return out;
}

std::optional<CameraRow> Repos::camera(TenantId t, std::int64_t id) {
    const auto r = db_.query(std::string("SELECT ") + kCamCols +
                                 " FROM cameras WHERE tenant_id=$1 AND id=$2",
                             {t.str(), S(id)});
    if (r.rows() == 0) return std::nullopt;
    return cam_from(r, 0);
}

std::int64_t Repos::create_camera(TenantId t, const CameraRow& c) {
    const auto r = db_.query(
        "INSERT INTO cameras(tenant_id,name,rtsp_main,rtsp_sub,stream_pref,role,"
        "face_enabled,enabled,detect_hz,face_hz,probed_width,probed_height,"
        "site,location,max_px,max_fps) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16) RETURNING id",
        {t.str(), c.name, c.rtsp_main, c.rtsp_sub, c.stream_pref, c.role,
         B(c.face_enabled), B(c.enabled), S(c.detect_hz), S(c.face_hz),
         S(c.probed_width), S(c.probed_height), c.site, c.location,
         S(c.max_px), S(c.max_fps)});
    return r.integer(0, 0);
}

void Repos::update_camera(TenantId t, std::int64_t id, const CameraRow& c) {
    db_.query(
        "UPDATE cameras SET name=$3,rtsp_main=$4,rtsp_sub=$5,stream_pref=$6,role=$7,"
        "face_enabled=$8,enabled=$9,detect_hz=$10,face_hz=$11,"
        "probed_width=$12,probed_height=$13,site=$14,location=$15,max_px=$16,"
        "max_fps=$17 "
        "WHERE tenant_id=$1 AND id=$2",
        {t.str(), S(id), c.name, c.rtsp_main, c.rtsp_sub, c.stream_pref, c.role,
         B(c.face_enabled), B(c.enabled), S(c.detect_hz), S(c.face_hz),
         S(c.probed_width), S(c.probed_height), c.site, c.location,
         S(c.max_px), S(c.max_fps)});
}

void Repos::delete_camera(TenantId t, std::int64_t id) {
    db_.query("DELETE FROM cameras WHERE tenant_id=$1 AND id=$2", {t.str(), S(id)});
}

// ── zones and gate lines ───────────────────────────────────────────────────

std::vector<Zone> Repos::zones(TenantId t, std::int64_t camera_id) {
    const std::string sql =
        "SELECT z.id,z.type,z.name,z.polygon::text,z.employee_id,z.enabled,"
        "COALESCE(e.name,''),COALESCE(e.emp_code,''),z.camera_id "
        "FROM zones z LEFT JOIN employees e ON e.id=z.employee_id "
        "WHERE z.tenant_id=$1" +
        std::string(camera_id ? " AND z.camera_id=$2" : "") + " ORDER BY z.id";
    const auto r = camera_id ? db_.query(sql, {t.str(), S(camera_id)})
                             : db_.query(sql, {t.str()});
    std::vector<Zone> out;
    for (int i = 0; i < r.rows(); ++i) {
        Zone z;
        z.id = r.integer(i, 0);
        z.type = zone_type_from(r.text(i, 1));
        z.name = r.text(i, 2);
        z.polygon = polygon_from_json(r.text(i, 3));
        z.employee_id = r.integer(i, 4, 0);
        z.enabled = r.boolean(i, 5, true);
        z.employee_name = r.text(i, 6);
        z.employee_code = r.text(i, 7);
        out.push_back(std::move(z));
    }
    return out;
}

std::int64_t Repos::create_zone(TenantId t, std::int64_t camera_id, const Zone& z) {
    std::vector<std::optional<std::string>> p{
        t.str(), S(camera_id), zone_type_to(z.type), z.name, polygon_to_json(z.polygon),
        z.employee_id ? std::optional<std::string>(S(z.employee_id)) : std::nullopt,
        B(z.enabled)};
    const auto r = db_.query_opt(
        // desk_key defaults to the zone NAME, which is already how an
        // operator says "these two shapes are the same desk": they draw it
        // on each camera that can see it and give both the same name. Left
        // null, a desk covered by two cameras goes back to being two rows
        // in the report for one person.
        "INSERT INTO zones(tenant_id,camera_id,type,name,polygon,employee_id,enabled,desk_key) "
        "VALUES ($1,$2,$3,$4,$5::jsonb,$6,$7,CASE WHEN $3='desk' THEN $4 END) RETURNING id",
        p);
    return r.integer(0, 0);
}

void Repos::update_zone(TenantId t, std::int64_t id, const Zone& z) {
    std::vector<std::optional<std::string>> p{
        t.str(), S(id), zone_type_to(z.type), z.name, polygon_to_json(z.polygon),
        z.employee_id ? std::optional<std::string>(S(z.employee_id)) : std::nullopt,
        B(z.enabled)};
    db_.query_opt(
        "UPDATE zones SET type=$3,name=$4,polygon=$5::jsonb,employee_id=$6,enabled=$7,"
        "updated_at=now() WHERE tenant_id=$1 AND id=$2",
        p);
}

void Repos::delete_zone(TenantId t, std::int64_t id) {
    db_.query("DELETE FROM zones WHERE tenant_id=$1 AND id=$2", {t.str(), S(id)});
}

std::map<std::string, bool> Repos::camera_modules(TenantId t, std::int64_t camera_id) {
    const auto r = db_.query(
        "SELECT module,enabled FROM camera_modules WHERE tenant_id=$1 AND camera_id=$2",
        {t.str(), S(camera_id)});
    std::map<std::string, bool> out;
    for (int i = 0; i < r.rows(); ++i) out[r.text(i, 0)] = r.boolean(i, 1, true);
    return out;
}

void Repos::set_camera_module(TenantId t, std::int64_t camera_id,
                              const std::string& module, bool enabled) {
    db_.query(
        "INSERT INTO camera_modules(tenant_id,camera_id,module,enabled) "
        "VALUES ($1,$2,$3,$4) "
        "ON CONFLICT (camera_id,module) DO UPDATE SET enabled=EXCLUDED.enabled",
        {t.str(), S(camera_id), module, B(enabled)});
}

std::vector<GateLineCfg> Repos::gate_lines(TenantId t, std::int64_t camera_id) {
    const std::string sql =
        "SELECT id,name,ax,ay,bx,by,enabled,camera_id FROM gate_lines WHERE tenant_id=$1" +
        std::string(camera_id ? " AND camera_id=$2" : "") + " ORDER BY id";
    const auto r = camera_id ? db_.query(sql, {t.str(), S(camera_id)})
                             : db_.query(sql, {t.str()});
    std::vector<GateLineCfg> out;
    for (int i = 0; i < r.rows(); ++i) {
        GateLineCfg g;
        g.id = r.integer(i, 0);
        g.name = r.text(i, 1);
        g.line.a = {static_cast<float>(r.number(i, 2)), static_cast<float>(r.number(i, 3))};
        g.line.b = {static_cast<float>(r.number(i, 4)), static_cast<float>(r.number(i, 5))};
        g.enabled = r.boolean(i, 6, true);
        out.push_back(g);
    }
    return out;
}

std::int64_t Repos::create_gate_line(TenantId t, std::int64_t camera_id,
                                     const GateLineCfg& g) {
    const auto r = db_.query(
        "INSERT INTO gate_lines(tenant_id,camera_id,name,ax,ay,bx,by,enabled) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8) RETURNING id",
        {t.str(), S(camera_id), g.name, std::to_string(g.line.a.x),
         std::to_string(g.line.a.y), std::to_string(g.line.b.x),
         std::to_string(g.line.b.y), B(g.enabled)});
    return r.integer(0, 0);
}

void Repos::delete_gate_line(TenantId t, std::int64_t id) {
    db_.query("DELETE FROM gate_lines WHERE tenant_id=$1 AND id=$2", {t.str(), S(id)});
}

// ── employees and faces ────────────────────────────────────────────────────

std::vector<Employee> Repos::employees(TenantId t, bool active_only) {
    const std::string sql =
        "SELECT e.id,e.emp_code,e.name,e.department,e.designation,e.email,e.phone,"
        "e.active,COALESCE((SELECT p.image_count FROM face_profiles p "
        "WHERE p.employee_id=e.id AND p.tenant_id=e.tenant_id),0) "
        "FROM employees e WHERE e.tenant_id=$1" +
        std::string(active_only ? " AND e.active" : "") + " ORDER BY e.name";
    const auto r = db_.query(sql, {t.str()});
    std::vector<Employee> out;
    for (int i = 0; i < r.rows(); ++i) {
        Employee e;
        e.id = r.integer(i, 0);
        e.emp_code = r.text(i, 1);
        e.name = r.text(i, 2);
        e.department = r.text(i, 3);
        e.designation = r.text(i, 4);
        e.email = r.text(i, 5);
        e.phone = r.text(i, 6);
        e.active = r.boolean(i, 7, true);
        e.face_profiles = static_cast<int>(r.integer(i, 8, 0));
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<Employee> Repos::employee(TenantId t, std::int64_t id) {
    for (auto& e : employees(t))
        if (e.id == id) return e;
    return std::nullopt;
}

std::int64_t Repos::create_employee(TenantId t, const Employee& e) {
    const auto r = db_.query(
        "INSERT INTO employees(tenant_id,emp_code,name,department,designation,email,phone,"
        "active) VALUES ($1,$2,$3,$4,$5,$6,$7,$8) RETURNING id",
        {t.str(), e.emp_code, e.name, e.department, e.designation, e.email, e.phone,
         B(e.active)});
    return r.integer(0, 0);
}

void Repos::update_employee(TenantId t, std::int64_t id, const Employee& e) {
    db_.query(
        "UPDATE employees SET emp_code=$3,name=$4,department=$5,designation=$6,"
        "email=$7,phone=$8,active=$9 WHERE tenant_id=$1 AND id=$2",
        {t.str(), S(id), e.emp_code, e.name, e.department, e.designation, e.email,
         e.phone, B(e.active)});
}

std::optional<Employee> Repos::employee_by_code(TenantId t, const std::string& code) {
    const auto r = db_.query(
        "SELECT e.id,e.emp_code,e.name,e.department,e.designation,e.email,e.phone,"
        "e.active,0 "
        "FROM employees e WHERE e.tenant_id=$1 AND e.emp_code=$2",
        {t.str(), code});
    if (r.rows() == 0) return std::nullopt;
    Employee e;
    e.id = r.integer(0, 0);
    e.emp_code = r.text(0, 1);
    e.name = r.text(0, 2);
    e.department = r.text(0, 3);
    e.designation = r.text(0, 4);
    e.email = r.text(0, 5);
    e.phone = r.text(0, 6);
    e.active = r.boolean(0, 7, true);
    return e;
}

void Repos::delete_employee(TenantId t, std::int64_t id) {
    db_.query("DELETE FROM employees WHERE tenant_id=$1 AND id=$2", {t.str(), S(id)});
}

namespace {

std::vector<float> unit(std::vector<float> v) {
    double n = 0;
    for (float x : v) n += static_cast<double>(x) * x;
    n = std::sqrt(n);
    if (n > 1e-9)
        for (float& x : v) x = static_cast<float>(x / n);
    return v;
}

float cosine_of(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    double s = 0;
    for (std::size_t i = 0; i < a.size(); ++i) s += static_cast<double>(a[i]) * b[i];
    return static_cast<float>(s);
}

std::vector<float> floats_from(const Result& r, int row, int col) {
    const auto raw = r.bytes(row, col);
    std::vector<float> v(raw.size() / sizeof(float));
    if (!v.empty()) std::memcpy(v.data(), raw.data(), raw.size());
    return v;
}

EnrolledProfile profile_from(const Result& r, int i, int emb_col) {
    EnrolledProfile p;
    p.employee_id = r.integer(i, 0);
    p.name = r.text(i, 1);
    p.code = r.text(i, 2);
    p.embedding = floats_from(r, i, emb_col);
    return p;
}

FaceImage image_from(const Result& r, int i) {
    FaceImage f;
    f.id = r.integer(i, 0);
    f.embedding = floats_from(r, i, 1);
    f.quality = static_cast<int>(r.integer(i, 2));
    f.yaw = static_cast<float>(r.number(i, 3));
    f.face_px = static_cast<float>(r.number(i, 4));
    f.det_score = static_cast<float>(r.number(i, 5));
    f.image_path = r.text(i, 6);
    f.filename = r.text(i, 7);
    f.source = r.text(i, 8);
    f.weight = static_cast<int>(r.integer(i, 9, 1));
    f.created_at = r.text(i, 10);
    return f;
}

const char* kImageCols =
    "id,embedding,quality,yaw,face_px,det_score,image_path,filename,source,weight,"
    "to_char(created_at,'YYYY-MM-DD HH24:MI:SS')";

FaceProfile profile_row(const Result& r, int i) {
    FaceProfile p;
    p.employee_id = r.integer(i, 0);
    p.embedding = floats_from(r, i, 1);
    p.image_count = static_cast<int>(r.integer(i, 2));
    p.quality = static_cast<int>(r.integer(i, 3));
    p.coherence = r.is_null(i, 4) ? -1.0f : static_cast<float>(r.number(i, 4));
    return p;
}

const char* kProfileCols = "employee_id,embedding,image_count,quality,coherence";

}  // namespace

std::int64_t Repos::add_face_image(TenantId t, std::int64_t employee_id,
                                   const std::vector<float>& embedding, int quality,
                                   float yaw, float face_px, float det_score,
                                   const std::string& image_path,
                                   const std::string& filename, int size_bytes,
                                   const std::string& source) {
    const auto r = db_.query(
        "INSERT INTO face_images(tenant_id,employee_id,embedding,dim,quality,yaw,"
        "face_px,det_score,image_path,filename,size_bytes,source) "
        "VALUES ($1,$2,$3::bytea,$4,$5,$6,$7,$8,$9,$10,$11,$12) RETURNING id",
        {t.str(), S(employee_id),
         to_bytea(embedding.data(), embedding.size() * sizeof(float)),
         S(static_cast<std::int64_t>(embedding.size())), S(quality),
         std::to_string(yaw), std::to_string(face_px), std::to_string(det_score),
         image_path, filename, S(size_bytes), source});
    return r.integer(0, 0);
}

void Repos::update_face_embedding(TenantId t, std::int64_t id,
                                  const std::vector<float>& embedding) {
    db_.query("UPDATE face_images SET embedding=$3::bytea, dim=$4 "
              "WHERE tenant_id=$1 AND id=$2",
              {t.str(), S(id),
               to_bytea(embedding.data(), embedding.size() * sizeof(float)),
               S(static_cast<std::int64_t>(embedding.size()))});
}

std::vector<FaceImage> Repos::face_images(TenantId t, std::int64_t employee_id) {
    const auto r = db_.query(
        std::string("SELECT ") + kImageCols +
            " FROM face_images WHERE tenant_id=$1 AND employee_id=$2 ORDER BY created_at",
        {t.str(), S(employee_id)});
    std::vector<FaceImage> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back(image_from(r, i));
    return out;
}

std::optional<FaceImage> Repos::face_image(TenantId t, std::int64_t id) {
    const auto r = db_.query(
        std::string("SELECT ") + kImageCols +
            " FROM face_images WHERE tenant_id=$1 AND id=$2",
        {t.str(), S(id)});
    if (r.rows() == 0) return std::nullopt;
    return image_from(r, 0);
}

void Repos::delete_face_image(TenantId t, std::int64_t id) {
    db_.query("DELETE FROM face_images WHERE tenant_id=$1 AND id=$2", {t.str(), S(id)});
}

void Repos::delete_face_images(TenantId t, std::int64_t employee_id) {
    db_.query("DELETE FROM face_images WHERE tenant_id=$1 AND employee_id=$2",
              {t.str(), S(employee_id)});
    db_.query("DELETE FROM face_profiles WHERE tenant_id=$1 AND employee_id=$2",
              {t.str(), S(employee_id)});
}

std::vector<std::int64_t> Repos::employees_present_today(TenantId t) {
    // The day boundary is the TENANT's, not the server's: an office in Dhaka
    // and a server in UTC disagree about when "today" began for seven hours of
    // every evening, and a desk going quiet at 6pm would be hard to explain.
    const auto r = db_.query(
        "SELECT DISTINCT employee_id FROM attendance_events "
        "WHERE tenant_id=$1 AND (ts AT TIME ZONE COALESCE("
        "  (SELECT timezone FROM tenant_settings WHERE tenant_id=$1), 'UTC'))::date "
        "      = (now() AT TIME ZONE COALESCE("
        "  (SELECT timezone FROM tenant_settings WHERE tenant_id=$1), 'UTC'))::date",
        {t.str()});
    std::vector<std::int64_t> out;
    out.reserve(static_cast<std::size_t>(r.rows()));
    for (int i = 0; i < r.rows(); ++i) out.push_back(r.integer(i, 0));
    return out;
}

FaceProfile Repos::recompute_profile(TenantId t, std::int64_t employee_id) {
    const auto imgs = face_images(t, employee_id);

    FaceProfile p;
    p.employee_id = employee_id;

    std::vector<const FaceImage*> usable;
    for (const auto& f : imgs)
        if (!f.embedding.empty()) usable.push_back(&f);

    if (usable.empty()) {
        db_.query("DELETE FROM face_profiles WHERE tenant_id=$1 AND employee_id=$2",
                  {t.str(), S(employee_id)});
        return p;
    }

    // Mixed embedding widths cannot be averaged; keep the majority, exactly as
    // the gallery does, because a cross-model mean is not a face at all.
    std::map<std::size_t, int> widths;
    for (const auto* f : usable) ++widths[f->embedding.size()];
    std::size_t dim = usable.front()->embedding.size();
    int best = 0;
    for (const auto& [d, n] : widths)
        if (n > best) { best = n; dim = d; }

    // WEIGHTED mean: a row standing for several photos keeps their combined
    // pull, so topping up a profile adds to it rather than moving it halfway
    // towards whatever was uploaded last.
    std::vector<double> acc(dim, 0.0);
    long total_weight = 0;
    int measured = 0, quality_sum = 0;
    for (const auto* f : usable) {
        if (f->embedding.size() != dim) continue;
        const int w = std::max(1, f->weight);
        const auto e = unit(f->embedding);
        for (std::size_t i = 0; i < dim; ++i) acc[i] += static_cast<double>(e[i]) * w;
        total_weight += w;
        if (!f->image_path.empty()) { quality_sum += f->quality; ++measured; }
    }
    if (total_weight == 0) return p;

    std::vector<float> mean(dim);
    for (std::size_t i = 0; i < dim; ++i)
        mean[i] = static_cast<float>(acc[i] / static_cast<double>(total_weight));
    p.embedding = unit(std::move(mean));
    p.image_count = static_cast<int>(total_weight);
    p.quality = measured ? quality_sum / measured : usable.front()->quality;

    // Coherence: the median pairwise cosine between the profile's OWN images.
    // This is the number that says which profile is the one handing its name
    // to other people - a profile can be full of individually high-scoring
    // photos and still be of two different faces.
    std::vector<std::vector<float>> vecs;
    for (const auto* f : usable)
        if (f->embedding.size() == dim && !f->image_path.empty())
            vecs.push_back(unit(f->embedding));
    if (vecs.size() >= 2) {
        std::vector<float> sims;
        for (std::size_t i = 0; i < vecs.size(); ++i)
            for (std::size_t j = i + 1; j < vecs.size(); ++j)
                sims.push_back(cosine_of(vecs[i], vecs[j]));
        std::sort(sims.begin(), sims.end());
        const std::size_t mid = sims.size() / 2;
        const float med = (sims.size() % 2) ? sims[mid] : (sims[mid - 1] + sims[mid]) / 2.0f;
        // Three decimals. Seventeen is not more accurate, it is just harder to read.
        p.coherence = std::round(med * 1000.0f) / 1000.0f;
    }

    std::vector<std::optional<std::string>> params{
        t.str(), S(employee_id),
        to_bytea(p.embedding.data(), p.embedding.size() * sizeof(float)),
        S(static_cast<std::int64_t>(p.embedding.size())), S(p.image_count), S(p.quality),
        p.coherence >= 0 ? std::optional<std::string>(std::to_string(p.coherence))
                         : std::nullopt};
    db_.query_opt(
        "INSERT INTO face_profiles(tenant_id,employee_id,embedding,dim,image_count,"
        "quality,coherence) VALUES ($1,$2,$3::bytea,$4,$5,$6,$7) "
        "ON CONFLICT (tenant_id,employee_id) DO UPDATE SET "
        "embedding=EXCLUDED.embedding,dim=EXCLUDED.dim,"
        "image_count=EXCLUDED.image_count,quality=EXCLUDED.quality,"
        "coherence=EXCLUDED.coherence,updated_at=now()",
        params);
    return p;
}

std::optional<FaceProfile> Repos::profile(TenantId t, std::int64_t employee_id) {
    const auto r = db_.query(
        std::string("SELECT ") + kProfileCols +
            " FROM face_profiles WHERE tenant_id=$1 AND employee_id=$2",
        {t.str(), S(employee_id)});
    if (r.rows() == 0) return std::nullopt;
    return profile_row(r, 0);
}

std::vector<FaceProfile> Repos::profiles(TenantId t) {
    const auto r = db_.query(std::string("SELECT ") + kProfileCols +
                                 " FROM face_profiles WHERE tenant_id=$1",
                             {t.str()});
    std::vector<FaceProfile> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back(profile_row(r, i));
    return out;
}

std::vector<NearNeighbour> Repos::too_close(TenantId t, std::int64_t employee_id,
                                            float threshold) {
    const auto me = profile(t, employee_id);
    std::vector<NearNeighbour> out;
    if (!me || me->embedding.empty()) return out;

    const auto r = db_.query(
        "SELECT p.employee_id,p.embedding,e.name FROM face_profiles p "
        "JOIN employees e ON e.id=p.employee_id "
        "WHERE p.tenant_id=$1 AND p.employee_id<>$2",
        {t.str(), S(employee_id)});
    for (int i = 0; i < r.rows(); ++i) {
        const auto other = floats_from(r, i, 1);
        if (other.size() != me->embedding.size()) continue;
        const float sim = cosine_of(me->embedding, other);
        if (sim >= threshold)
            out.push_back(NearNeighbour{r.integer(i, 0), r.text(i, 2), sim});
    }
    std::sort(out.begin(), out.end(),
              [](const NearNeighbour& a, const NearNeighbour& b) {
                  return a.similarity > b.similarity;
              });
    return out;
}

std::vector<EnrolledProfile> Repos::gallery(TenantId t) {
    // ONE row per person. Previously every photo was its own gallery entry, so
    // somebody with five photos appeared five times and topping up a profile
    // made them appear more OFTEN rather than more accurately.
    const auto r = db_.query(
        "SELECT e.id,e.name,e.emp_code,p.embedding FROM face_profiles p "
        "JOIN employees e ON e.id=p.employee_id "
        "WHERE p.tenant_id=$1 AND e.active ORDER BY e.id",
        {t.str()});
    std::vector<EnrolledProfile> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back(profile_from(r, i, 3));
    return out;
}

std::vector<std::pair<std::int64_t, EnrolledProfile>> Repos::gallery_all() {
    const auto r = db_.query(
        "SELECT e.id,e.name,e.emp_code,p.embedding,p.tenant_id FROM face_profiles p "
        "JOIN employees e ON e.id=p.employee_id "
        "JOIN tenants t ON t.id=p.tenant_id "
        "WHERE e.active AND t.status='active' ORDER BY p.tenant_id, e.id");
    std::vector<std::pair<std::int64_t, EnrolledProfile>> out;
    for (int i = 0; i < r.rows(); ++i)
        out.emplace_back(r.integer(i, 4), profile_from(r, i, 3));
    return out;
}

// ── settings ───────────────────────────────────────────────────────────────

Settings Repos::settings(TenantId t) {
    const auto r = db_.query(
        "SELECT timezone,office_start::text,office_end::text,grace_minutes,"
        "early_leave_grace_minutes,telegram_bot_token,telegram_chat_id,smtp_host,"
        "smtp_port,smtp_user,smtp_password,smtp_from,smtp_from_name,email_to,"
        "min_break_seconds "
        "FROM tenant_settings WHERE tenant_id=$1",
        {t.str()});
    Settings s;
    if (r.rows() == 0) return s;
    s.timezone = r.text(0, 0);
    s.office_start = r.text(0, 1);
    s.office_end = r.text(0, 2);
    s.grace_minutes = static_cast<int>(r.integer(0, 3, 15));
    s.early_leave_grace_minutes = static_cast<int>(r.integer(0, 4, 15));
    s.telegram_bot_token = r.text(0, 5);
    s.telegram_chat_id = r.text(0, 6);
    s.smtp_host = r.text(0, 7);
    s.smtp_port = static_cast<int>(r.integer(0, 8, 587));
    s.smtp_user = r.text(0, 9);
    s.smtp_password = r.text(0, 10);
    s.smtp_from = r.text(0, 11);
    s.smtp_from_name = r.text(0, 12);
    s.email_to = r.text(0, 13);
    s.min_break_seconds = static_cast<int>(r.integer(0, 14, 60));
    return s;
}

void Repos::save_settings(TenantId t, const Settings& s) {
    db_.query(
        "INSERT INTO tenant_settings(tenant_id,timezone,office_start,office_end,"
        "grace_minutes,early_leave_grace_minutes,telegram_bot_token,telegram_chat_id,"
        "smtp_host,smtp_port,smtp_user,smtp_password,smtp_from,smtp_from_name,email_to,"
        "min_break_seconds) "
        "VALUES ($1,$2,$3::time,$4::time,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16) "
        "ON CONFLICT (tenant_id) DO UPDATE SET timezone=EXCLUDED.timezone,"
        "office_start=EXCLUDED.office_start,office_end=EXCLUDED.office_end,"
        "grace_minutes=EXCLUDED.grace_minutes,"
        "early_leave_grace_minutes=EXCLUDED.early_leave_grace_minutes,"
        "telegram_bot_token=EXCLUDED.telegram_bot_token,"
        "telegram_chat_id=EXCLUDED.telegram_chat_id,smtp_host=EXCLUDED.smtp_host,"
        "smtp_port=EXCLUDED.smtp_port,smtp_user=EXCLUDED.smtp_user,"
        "smtp_password=EXCLUDED.smtp_password,smtp_from=EXCLUDED.smtp_from,"
        "smtp_from_name=EXCLUDED.smtp_from_name,email_to=EXCLUDED.email_to,"
        "min_break_seconds=EXCLUDED.min_break_seconds,"
        "updated_at=now()",
        {t.str(), s.timezone, s.office_start, s.office_end, S(s.grace_minutes),
         S(s.early_leave_grace_minutes), s.telegram_bot_token, s.telegram_chat_id,
         s.smtp_host, S(s.smtp_port), s.smtp_user, s.smtp_password, s.smtp_from,
         s.smtp_from_name, s.email_to, S(s.min_break_seconds)});
}

// ── attendance ─────────────────────────────────────────────────────────────

void Repos::record_attendance(TenantId t, std::int64_t employee_id,
                              std::int64_t camera_id, const std::string& direction,
                              const std::string& ts_iso, const std::string& work_date,
                              float confidence, int track_id,
                              const std::string& snapshot_path) {
    std::vector<std::optional<std::string>> p{
        t.str(), S(employee_id),
        camera_id ? std::optional<std::string>(S(camera_id)) : std::nullopt,
        direction, ts_iso, work_date, std::to_string(confidence), S(track_id),
        snapshot_path};
    db_.query_opt(
        "INSERT INTO attendance_events(tenant_id,employee_id,camera_id,direction,ts,"
        "work_date,confidence,track_id,snapshot_path) "
        "VALUES ($1,$2,$3,$4,$5::timestamptz,$6::date,$7,$8,$9)",
        p);
    recompute_day(t, employee_id, work_date);
}

void Repos::recompute_day(TenantId t, std::int64_t employee_id,
                          const std::string& work_date) {
    // The day row is a CACHE. It is always rebuilt from the events, never
    // edited, so a mis-recorded punch is fixed by deleting the event and
    // re-running this rather than by adjusting a total nobody can trace.
    const auto ev = db_.query(
        "SELECT direction, EXTRACT(EPOCH FROM (ts AT TIME ZONE COALESCE("
        "  (SELECT timezone FROM tenant_settings WHERE tenant_id=$1),'UTC'))"
        "  - date_trunc('day', ts AT TIME ZONE COALESCE("
        "  (SELECT timezone FROM tenant_settings WHERE tenant_id=$1),'UTC')))::int, ts "
        "FROM attendance_events "
        "WHERE tenant_id=$1 AND employee_id=$2 AND work_date=$3::date ORDER BY ts",
        {t.str(), S(employee_id), work_date});

    std::vector<Punch> punches;
    punches.reserve(static_cast<std::size_t>(ev.rows()));
    for (int i = 0; i < ev.rows(); ++i)
        punches.push_back(Punch{ev.text(i, 0) == "in" ? Direction::In : Direction::Out,
                                ev.integer(i, 1, 0)});

    const Settings st = settings(t);
    OfficeHours hours;
    auto parse_hm = [](const std::string& hhmmss) {
        int h = 0, m = 0;
        if (hhmmss.size() >= 5) {
            h = std::stoi(hhmmss.substr(0, 2));
            m = std::stoi(hhmmss.substr(3, 2));
        }
        return TimeOfDay::from_hm(h, m);
    };
    hours.start = parse_hm(st.office_start);
    hours.end = parse_hm(st.office_end);
    hours.grace_minutes = st.grace_minutes;
    hours.early_leave_grace_minutes = st.early_leave_grace_minutes;
    hours.min_break_seconds = st.min_break_seconds;

    // HOW FAR THE CLOCK HAS GOT, in the tenant's own timezone, and only for a
    // day that is still running. A finished day keeps closing at the office
    // end; today stops at this moment, so nobody is credited with hours they
    // have not worked yet.
    std::int64_t as_of = 24 * 3600;
    {
        const auto r = db_.query(
            "SELECT work_date = (now() AT TIME ZONE COALESCE("
            "  (SELECT timezone FROM tenant_settings WHERE tenant_id=$1),'UTC'))::date,"
            "       EXTRACT(EPOCH FROM (now() AT TIME ZONE COALESCE("
            "  (SELECT timezone FROM tenant_settings WHERE tenant_id=$1),'UTC'))::time)::int"
            "  FROM (SELECT $2::date AS work_date) q",
            {t.str(), work_date});
        if (r.rows() > 0 && r.boolean(0, 0)) as_of = r.integer(0, 1, 24 * 3600);
    }

    const DayRollup d = so::rollup(punches, hours, as_of);

    std::vector<std::optional<std::string>> p{
        t.str(), S(employee_id), work_date,
        d.has_activity ? std::optional<std::string>(S(d.first_in)) : std::nullopt,
        // NULL when they were never recorded leaving, so the column is empty
        // rather than showing midnight — or, as it did before, showing the
        // moment the report ran as though it were a departure.
        (d.has_activity && d.last_out > 0) ? std::optional<std::string>(S(d.last_out))
                                          : std::nullopt,
        S(d.gross_seconds), S(d.inside_seconds), S(d.break_seconds), S(d.entries),
        to_string(d.status), B(d.assumed_out)};
    db_.query_opt(
        "INSERT INTO attendance_days(tenant_id,employee_id,work_date,first_in,last_out,"
        "gross_seconds,inside_seconds,break_seconds,entries,status,assumed_out) "
        "VALUES ($1,$2,$3::date,"
        "  $3::date + ($4||' seconds')::interval, $3::date + ($5||' seconds')::interval,"
        "  $6,$7,$8,$9,$10,$11) "
        "ON CONFLICT (tenant_id,employee_id,work_date) DO UPDATE SET "
        "first_in=EXCLUDED.first_in,last_out=EXCLUDED.last_out,"
        "gross_seconds=EXCLUDED.gross_seconds,inside_seconds=EXCLUDED.inside_seconds,"
        "break_seconds=EXCLUDED.break_seconds,entries=EXCLUDED.entries,"
        "status=EXCLUDED.status,assumed_out=EXCLUDED.assumed_out,updated_at=now()",
        p);
}

namespace {
const char* kDayCols =
    "e.id,e.emp_code,e.name,e.department,"
    "COALESCE(to_char(d.first_in,'HH24:MI'),''),"
    "COALESCE(to_char(d.last_out,'HH24:MI'),''),"
    "COALESCE(d.status,'absent'),COALESCE(d.gross_seconds,0),"
    "COALESCE(d.inside_seconds,0),COALESCE(d.break_seconds,0),"
    "COALESCE(d.entries,0),COALESCE(d.assumed_out,false)";

DayRow day_from(const Result& r, int i) {
    DayRow d;
    d.employee_id = r.integer(i, 0);
    d.emp_code = r.text(i, 1);
    d.name = r.text(i, 2);
    d.department = r.text(i, 3);
    d.first_in = r.text(i, 4);
    d.last_out = r.text(i, 5);
    d.status = r.text(i, 6);
    d.gross_seconds = static_cast<int>(r.integer(i, 7));
    d.inside_seconds = static_cast<int>(r.integer(i, 8));
    d.break_seconds = static_cast<int>(r.integer(i, 9));
    d.entries = static_cast<int>(r.integer(i, 10));
    d.assumed_out = r.boolean(i, 11);
    return d;
}
}  // namespace

std::vector<std::string> Repos::work_dates(TenantId t, const std::string& from,
                                           const std::string& to) {
    const bool ranged = !from.empty() && !to.empty();
    const auto r = ranged
        ? db_.query("SELECT DISTINCT work_date::text FROM attendance_events "
                    "WHERE tenant_id=$1 AND work_date BETWEEN $2::date AND $3::date "
                    "ORDER BY 1",
                    {t.str(), from, to})
        : db_.query("SELECT DISTINCT work_date::text FROM attendance_events "
                    "WHERE tenant_id=$1 AND work_date = (now() AT TIME ZONE COALESCE("
                    "  (SELECT timezone FROM tenant_settings WHERE tenant_id=$1),'UTC'))::date "
                    "ORDER BY 1",
                    {t.str()});
    std::vector<std::string> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back(r.text(i, 0));
    return out;
}

void Repos::record_exit_review(TenantId t, std::int64_t camera_id, std::int64_t zone_id,
                               const std::string& ts_iso, const std::string& work_date,
                               int track_id, std::int64_t guess_employee_id,
                               float score, float margin, float weight,
                               const std::string& crop_path,
                               const std::string& frame_path) {
    db_.query_opt(
        "INSERT INTO exit_reviews(tenant_id,camera_id,zone_id,ts,work_date,track_id,"
        "guess_employee_id,guess_score,guess_margin,guess_weight,crop_path,frame_path) "
        "VALUES ($1,$2,$3,$4::timestamptz,$5::date,$6,$7,$8,$9,$10,$11,$12)",
        {t.str(), S(camera_id), zone_id ? std::optional<std::string>(S(zone_id))
                                        : std::nullopt,
         ts_iso, work_date, S(track_id),
         guess_employee_id ? std::optional<std::string>(S(guess_employee_id))
                           : std::nullopt,
         F(score), F(margin), F(weight), crop_path, frame_path});
}

std::vector<ExitReviewRow> Repos::open_exit_reviews(TenantId t,
                                                    const std::string& work_date) {
    const auto r = db_.query(
        "SELECT x.id, COALESCE(x.camera_id,0), COALESCE(c.name,''), "
        "       to_char(x.ts AT TIME ZONE COALESCE("
        "         (SELECT timezone FROM tenant_settings WHERE tenant_id=$1),'UTC'),"
        "         'HH24:MI'), x.work_date::text, COALESCE(x.guess_employee_id,0), "
        "       COALESCE(e.name,''), COALESCE(e.emp_code,''), "
        "       x.guess_score, x.guess_margin, x.guess_weight, x.crop_path, x.frame_path "
        "  FROM exit_reviews x "
        "  LEFT JOIN cameras c ON c.id = x.camera_id "
        "  LEFT JOIN employees e ON e.id = x.guess_employee_id "
        " WHERE x.tenant_id=$1 AND x.resolved_employee_id IS NULL AND NOT x.dismissed "
        "   AND ($2 = '' OR x.work_date = $2::date) "
        " ORDER BY x.ts DESC LIMIT 500",
        {t.str(), work_date});
    std::vector<ExitReviewRow> out;
    for (int i = 0; i < r.rows(); ++i) {
        ExitReviewRow x;
        x.id = r.integer(i, 0);
        x.camera_id = r.integer(i, 1);
        x.camera_name = r.text(i, 2);
        x.ts = r.text(i, 3);
        x.work_date = r.text(i, 4);
        x.guess_employee_id = r.integer(i, 5);
        x.guess_name = r.text(i, 6);
        x.guess_code = r.text(i, 7);
        x.guess_score = static_cast<float>(std::atof(r.text(i, 8).c_str()));
        x.guess_margin = static_cast<float>(std::atof(r.text(i, 9).c_str()));
        x.guess_weight = static_cast<float>(std::atof(r.text(i, 10).c_str()));
        x.crop_path = r.text(i, 11);
        x.frame_path = r.text(i, 12);
        out.push_back(std::move(x));
    }
    return out;
}

std::optional<ExitReviewRow> Repos::resolve_exit_review(TenantId t, std::int64_t id,
                                                        std::int64_t employee_id,
                                                        std::int64_t user_id) {
    const auto r = db_.query_opt(
        // $3 IS CAST EXPLICITLY, and that is not decoration. It appears twice —
        // once as the column value and once in the dismissed test — and when it
        // is NULL Postgres has nothing to infer a type from, so the whole
        // statement fails with "could not determine data type of parameter $3".
        // Every answer a reviewer gave came back as "internal error".
        "UPDATE exit_reviews SET resolved_employee_id=$3::bigint, "
        "       dismissed=($3::bigint IS NULL), "
        "       resolved_by=$4::bigint, resolved_at=now() "
        " WHERE tenant_id=$1 AND id=$2 AND resolved_employee_id IS NULL AND NOT dismissed "
        " RETURNING COALESCE(camera_id,0), to_char(ts,'YYYY-MM-DD\"T\"HH24:MI:SSOF'), "
        "           work_date::text, guess_score",
        {t.str(), S(id),
         employee_id ? std::optional<std::string>(S(employee_id)) : std::nullopt,
         user_id ? std::optional<std::string>(S(user_id)) : std::nullopt});
    if (r.rows() == 0) return std::nullopt;
    ExitReviewRow x;
    x.id = id;
    x.camera_id = r.integer(0, 0);
    x.ts = r.text(0, 1);
    x.work_date = r.text(0, 2);
    x.guess_score = static_cast<float>(std::atof(r.text(0, 3).c_str()));
    return x;
}

std::vector<std::pair<std::int64_t, std::string>> Repos::open_days_today(TenantId t) {
    const auto r = db_.query(
        "SELECT employee_id, work_date::text FROM attendance_days "
        "WHERE tenant_id=$1 AND assumed_out "
        "  AND work_date = (now() AT TIME ZONE COALESCE("
        "    (SELECT timezone FROM tenant_settings WHERE tenant_id=$1),'UTC'))::date",
        {t.str()});
    std::vector<std::pair<std::int64_t, std::string>> out;
    for (int i = 0; i < r.rows(); ++i) out.emplace_back(r.integer(i, 0), r.text(i, 1));
    return out;
}

std::vector<std::int64_t> Repos::employees_with_events(TenantId t,
                                                       const std::string& work_date) {
    const auto r = db_.query(
        "SELECT DISTINCT employee_id FROM attendance_events "
        "WHERE tenant_id=$1 AND work_date=$2::date",
        {t.str(), work_date});
    std::vector<std::int64_t> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back(r.integer(i, 0));
    return out;
}

std::vector<DayRow> Repos::attendance_day(TenantId t, const std::string& work_date) {
    // A LEFT JOIN from employees, so somebody who never appeared is listed as
    // absent rather than simply missing from the report.
    const auto r = db_.query(
        std::string("SELECT ") + kDayCols +
            " FROM employees e LEFT JOIN attendance_days d"
            "  ON d.employee_id=e.id AND d.tenant_id=e.tenant_id AND d.work_date=$2::date "
            "WHERE e.tenant_id=$1 AND e.active ORDER BY e.name",
        {t.str(), work_date});
    std::vector<DayRow> out;
    for (int i = 0; i < r.rows(); ++i) out.push_back(day_from(r, i));
    return out;
}

std::vector<DayRow> Repos::attendance_range(TenantId t, std::int64_t employee_id,
                                            const std::string& from,
                                            const std::string& to) {
    const auto r = db_.query(
        "SELECT e.id,e.emp_code,e.name,e.department,"
        "COALESCE(to_char(d.first_in,'HH24:MI'),''),"
        "COALESCE(to_char(d.last_out,'HH24:MI'),''),"
        "d.status,d.gross_seconds,d.inside_seconds,d.break_seconds,d.entries,"
        "d.assumed_out,to_char(d.work_date,'YYYY-MM-DD') "
        "FROM attendance_days d JOIN employees e ON e.id=d.employee_id "
        "WHERE d.tenant_id=$1 AND d.employee_id=$2 "
        "  AND d.work_date BETWEEN $3::date AND $4::date ORDER BY d.work_date",
        {t.str(), S(employee_id), from, to});
    std::vector<DayRow> out;
    for (int i = 0; i < r.rows(); ++i) {
        DayRow d = day_from(r, i);
        d.department = r.text(i, 12);   // the date rides in this slot for a range view
        out.push_back(d);
    }
    return out;
}

// ── desk ───────────────────────────────────────────────────────────────────

void Repos::upsert_desk_day(TenantId t, std::int64_t zone_id, std::int64_t employee_id,
                            const std::string& work_date, int occupied_delta,
                            int absent_delta, int mismatch_delta,
                            int sessions_delta, int longest_absence) {
    std::vector<std::optional<std::string>> p{
        t.str(), S(zone_id),
        employee_id ? std::optional<std::string>(S(employee_id)) : std::nullopt,
        work_date, S(occupied_delta), S(absent_delta), S(mismatch_delta),
        S(sessions_delta), S(longest_absence)};
    // The seconds ACCUMULATE; the longest absence is a MAXIMUM, not a sum -
    // adding them would report a day of short breaks as one long disappearance.
    db_.query_opt(
        "INSERT INTO desk_daily(tenant_id,zone_id,employee_id,work_date,"
        "occupied_seconds,absent_seconds,mismatch_seconds,sessions_count,"
        "longest_absence_seconds) "
        "VALUES ($1,$2,$3,$4::date,$5,$6,$7,$8,$9) "
        "ON CONFLICT (tenant_id,zone_id,work_date) DO UPDATE SET "
        "occupied_seconds=desk_daily.occupied_seconds+EXCLUDED.occupied_seconds,"
        "absent_seconds=desk_daily.absent_seconds+EXCLUDED.absent_seconds,"
        "mismatch_seconds=desk_daily.mismatch_seconds+EXCLUDED.mismatch_seconds,"
        "sessions_count=desk_daily.sessions_count+EXCLUDED.sessions_count,"
        "longest_absence_seconds=GREATEST(desk_daily.longest_absence_seconds,"
        "                                 EXCLUDED.longest_absence_seconds),"
        "employee_id=EXCLUDED.employee_id,updated_at=now()",
        p);
}

std::vector<DeskRow> Repos::desk_day(TenantId t, const std::string& work_date) {
    const auto r = db_.query(
        // ONE ROW PER DESK, NOT PER ZONE.
        //
        // A desk is drawn once per camera that can see it, so a desk two
        // cameras cover was two rows for the same person — Ramim showed up
        // twice here, with 27 and 9 minutes that were the SAME minutes from two
        // angles. desk_key is what those zones share.
        //
        // MAX, not SUM, for the time. Adding them double-counts whenever both
        // cameras watch at once, which is the normal case for a desk two
        // cameras were pointed at on purpose. The exact answer is the union of
        // the two occupied periods, and that cannot be computed from here:
        // desk_daily stores totals for the day, not when they happened. MAX is
        // the right approximation — it is exact while the views overlap, and
        // understates only if one camera saw the morning and the other the
        // afternoon, which is not what a second angle on one desk is for.
        //
        // Counts behave differently and so are aggregated differently: a
        // session seen twice is one session (MAX), while a mismatch seen by
        // either camera is worth reporting (MAX keeps the one that saw it).
        "SELECT MIN(z.id), MAX(COALESCE(d.employee_id,z.employee_id)), "
        "       COALESCE(z.desk_key,z.name), MAX(COALESCE(e.name,'')), "
        "       MAX(COALESCE(d.occupied_seconds,0)), MAX(COALESCE(d.absent_seconds,0)), "
        "       MAX(COALESCE(d.longest_absence_seconds,0)), "
        "       MAX(COALESCE(d.sessions_count,0)), MAX(COALESCE(d.mismatch_seconds,0)), "
        // Was this person recorded as present at all today? Desk time for
        // somebody who never arrived is somebody ELSE at their desk, and
        // crediting it to them in silence is the one thing this report must
        // not do.
        "       EXISTS (SELECT 1 FROM attendance_events a "
        "               WHERE a.tenant_id=z.tenant_id AND a.employee_id=z.employee_id "
        "                 AND a.ts::date=$2::date) "
        "FROM zones z LEFT JOIN desk_daily d"
        "  ON d.zone_id=z.id AND d.tenant_id=z.tenant_id AND d.work_date=$2::date "
        "LEFT JOIN employees e ON e.id=z.employee_id "
        "WHERE z.tenant_id=$1 AND z.type='desk' "
        "GROUP BY z.tenant_id, COALESCE(z.desk_key,z.name), z.employee_id "
        "ORDER BY 3",
        {t.str(), work_date});
    std::vector<DeskRow> out;
    for (int i = 0; i < r.rows(); ++i) {
        DeskRow d;
        d.zone_id = r.integer(i, 0);
        d.employee_id = r.integer(i, 1, 0);
        d.zone_name = r.text(i, 2);
        d.employee_name = r.text(i, 3);
        d.occupied_seconds = static_cast<int>(r.integer(i, 4));
        d.absent_seconds = static_cast<int>(r.integer(i, 5));
        d.longest_absence_seconds = static_cast<int>(r.integer(i, 6));
        d.sessions_count = static_cast<int>(r.integer(i, 7));
        d.mismatch_seconds = static_cast<int>(r.integer(i, 8));
        d.employee_present = r.boolean(i, 9, false);
        out.push_back(std::move(d));
    }
    return out;
}

// ── events ─────────────────────────────────────────────────────────────────

std::int64_t Repos::record_event(TenantId t, const EventRow& e) {
    std::vector<std::optional<std::string>> p{
        t.str(),
        e.camera_id ? std::optional<std::string>(S(e.camera_id)) : std::nullopt,
        e.module, e.severity, e.title, e.detail, e.ts,
        e.zone_id ? std::optional<std::string>(S(e.zone_id)) : std::nullopt,
        e.employee_id ? std::optional<std::string>(S(e.employee_id)) : std::nullopt,
        e.snapshot_path, e.crop_path};
    const auto r = db_.query_opt(
        "INSERT INTO events(tenant_id,camera_id,module,severity,title,detail,ts,zone_id,"
        "employee_id,snapshot_path,crop_path) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7::timestamptz,$8,$9,$10,$11) RETURNING id",
        p);
    return r.integer(0, 0);
}

std::vector<EventRow> Repos::events(TenantId t, const std::string& module,
                                    const std::string& from, const std::string& to,
                                    int limit) {
    // The filters are optional and are expressed IN SQL rather than by building
    // a WHERE clause out of strings, so there is no path where a request value
    // reaches the parser as syntax.
    const auto r = db_.query(
        "SELECT ev.id,COALESCE(ev.camera_id,0),COALESCE(ev.zone_id,0),"
        "COALESCE(ev.employee_id,0),ev.module,ev.severity,ev.title,ev.detail,"
        "to_char(ev.ts,'YYYY-MM-DD\"T\"HH24:MI:SSOF'),ev.snapshot_path,ev.crop_path,"
        "COALESCE(c.name,''),(ev.acknowledged_at IS NOT NULL),"
        "COALESCE(c.site,''),COALESCE(c.location,'') "
        "FROM events ev LEFT JOIN cameras c ON c.id=ev.camera_id "
        "WHERE ev.tenant_id=$1 "
        "  AND ($2 = '' OR ev.module = $2) "
        "  AND ($3 = '' OR ev.ts >= $3::timestamptz) "
        "  AND ($4 = '' OR ev.ts <= $4::timestamptz) "
        "ORDER BY ev.ts DESC LIMIT $5",
        {t.str(), module, from, to, S(limit > 0 && limit <= 1000 ? limit : 200)});
    std::vector<EventRow> out;
    for (int i = 0; i < r.rows(); ++i) {
        EventRow e;
        e.id = r.integer(i, 0);
        e.camera_id = r.integer(i, 1);
        e.zone_id = r.integer(i, 2);
        e.employee_id = r.integer(i, 3);
        e.module = r.text(i, 4);
        e.severity = r.text(i, 5);
        e.title = r.text(i, 6);
        e.detail = r.text(i, 7);
        e.ts = r.text(i, 8);
        e.snapshot_path = r.text(i, 9);
        e.crop_path = r.text(i, 10);
        e.camera_name = r.text(i, 11);
        e.acknowledged = r.boolean(i, 12);
        e.camera_site = r.text(i, 13);
        e.camera_location = r.text(i, 14);
        out.push_back(std::move(e));
    }
    return out;
}

void Repos::acknowledge_event(TenantId t, std::int64_t id, std::int64_t user_id) {
    db_.query(
        "UPDATE events SET acknowledged_by=$3, acknowledged_at=now() "
        "WHERE tenant_id=$1 AND id=$2",
        {t.str(), S(id), S(user_id)});
}

void Repos::enqueue_notification(TenantId t, std::int64_t event_id,
                                 const std::string& channel) {
    db_.query(
        "INSERT INTO notification_outbox(tenant_id,event_id,channel) VALUES ($1,$2,$3)",
        {t.str(), S(event_id), channel});
}

}  // namespace so::db
