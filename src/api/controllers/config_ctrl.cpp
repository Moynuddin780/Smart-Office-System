#include "api/camera_rules.hpp"
#include "api/server.hpp"
#include "core/onvif.hpp"
#include "core/stream_probe.hpp"
#include "identity/face_identifier.hpp"

#include <drogon/HttpAppFramework.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <thread>

namespace so::api {
namespace {

/// The five modules, described once, so the UI and the engine cannot drift
/// apart about what exists or what each one needs.
/// One AI module, described once.
///
/// `draws` is what the operator has to put on the frame before the module can
/// do anything — "" for a module that needs nothing. The UI keys off it so a
/// module that needs no geometry never offers a drawing tool, and one that does
/// can link straight to it. Keeping it in the catalogue rather than in the page
/// means the two cannot disagree about which modules need a shape.
struct ModuleInfo {
    const char* id;
    const char* name;
    const char* blurb;
    const char* needs;
    const char* draws;      ///< "zone" | "desk" | "line" | "" (nothing to draw)
    const char* wants_face; ///< "1" when the module is pointless without faces
};
constexpr ModuleInfo kModules[] = {
    {"perimeter", "Perimeter & restricted zones",
     "Raises a critical alert when a person stands inside a drawn zone. Decided on the "
     "foot point, so somebody walking past the zone is not counted as inside it.",
     "A perimeter or restricted zone", "zone", ""},
    {"fall", "Fall detection",
     "Reads posture from 17 body keypoints rather than the box. Warns once the posture "
     "has held, then escalates to a critical alert if the person is still down and "
     "barely moving.",
     "Nothing — it works on any camera", "", ""},
    {"desk", "Desk on-duty",
     "Accumulates time at a desk zone bound to one employee. A short absence does not "
     "end the session; a longer one does.",
     "A desk zone bound to an employee", "desk", ""},
    {"unauthorised", "Unauthorised person",
     "Alerts only when a face was actually seen and matched nobody. A person the camera "
     "never got a face on is recorded as unidentified and raises nothing.",
     "Face recognition enabled", "", "1"},
    {"out_count", "Out count & breaks",
     "Records an employee LEAVING, through a drawn exit zone, so the day has an out to "
     "pair with each in. Breaks are the gaps between them. Nothing is recorded for "
     "somebody who has not already been counted in today.",
     "An exit zone, and face recognition", "zone", "1"},
    {"attendance", "Attendance",
     "Records a person as present the moment their face is recognised. Put it on the "
     "camera that watches the way in; nothing has to be drawn.",
     "Face recognition", "", "1"},
};
constexpr const char* kModuleIds[] = {"perimeter", "fall", "desk", "unauthorised", "attendance"};


Json::Value camera_json(const db::CameraRow& c) {
    Json::Value v;
    v["id"] = static_cast<Json::Int64>(c.id);
    v["name"] = c.name;
    v["rtsp_main"] = c.rtsp_main;
    v["rtsp_sub"] = c.rtsp_sub;
    v["stream_pref"] = c.stream_pref;
    v["role"] = c.role;
    v["site"] = c.site;
    v["location"] = c.location;
    v["face_enabled"] = c.face_enabled;
    v["enabled"] = c.enabled;
    v["detect_hz"] = c.detect_hz;
    v["face_hz"] = c.face_hz;
    v["max_px"] = c.max_px;
    v["max_fps"] = c.max_fps;
    v["probed_width"] = c.probed_width;
    v["probed_height"] = c.probed_height;
    return v;
}

db::CameraRow camera_from(const Json::Value& b, const db::CameraRow& base = {}) {
    db::CameraRow c = base;
    if (b.isMember("name")) c.name = b["name"].asString();
    if (b.isMember("rtsp_main")) c.rtsp_main = b["rtsp_main"].asString();
    if (b.isMember("rtsp_sub")) c.rtsp_sub = b["rtsp_sub"].asString();
    if (b.isMember("stream_pref")) c.stream_pref = b["stream_pref"].asString();
    if (b.isMember("role")) c.role = b["role"].asString();
    if (b.isMember("site")) c.site = b["site"].asString();
    if (b.isMember("location")) c.location = b["location"].asString();
    if (b.isMember("face_enabled")) c.face_enabled = b["face_enabled"].asBool();
    if (b.isMember("enabled")) c.enabled = b["enabled"].asBool();
    if (b.isMember("detect_hz")) c.detect_hz = b["detect_hz"].asInt();
    if (b.isMember("face_hz")) c.face_hz = b["face_hz"].asInt();
    if (b.isMember("probed_width")) c.probed_width = b["probed_width"].asInt();
    if (b.isMember("probed_height")) c.probed_height = b["probed_height"].asInt();
    if (b.isMember("max_px")) c.max_px = b["max_px"].asInt();
    // Clamped here as well as in the database, so a bad value is corrected
    // rather than turned into a 500 from a constraint the operator never sees.
    c.max_px = std::clamp(c.max_px, 320, 4096);
    if (b.isMember("max_fps")) c.max_fps = b["max_fps"].asInt();
    // 0 stays 0 — it is "no limit", not a small limit — and anything else is
    // pulled into the range the tracker can still follow a person across.
    if (c.max_fps != 0) c.max_fps = std::clamp(c.max_fps, 5, 60);
    if (c.stream_pref.empty()) c.stream_pref = "sub";
    if (c.role.empty()) c.role = "area";
    return c;
}

/// Bridge the stored row to the rule set in api/camera_rules.hpp.
///
/// The rules are kept off db::CameraRow on purpose so they can be tested
/// without a database; this is the one line that pays for that.
/// Apply a {"perimeter":true,"fall":false,...} object to one camera.
///
/// Assignment lives on the camera form, the way the operator thinks about it —
/// "this camera watches for X" — so create and update both accept it inline
/// rather than making a second round trip to a separate endpoint. Unknown ids
/// are ignored rather than rejected: a newer UI naming a module this build does
/// not have should not fail the whole save.
void apply_modules(Context& ctx, db::TenantId t, std::int64_t camera_id,
                   const Json::Value& mods) {
    if (!mods.isObject()) return;
    for (const auto& id : kModuleIds)
        if (mods.isMember(id))
            ctx.repos->set_camera_module(t, camera_id, id, mods[id].asBool());
}

CameraDraft draft_of(const db::CameraRow& c) {
    return CameraDraft{c.name, c.rtsp_main, c.rtsp_sub, c.stream_pref, c.role,
                       c.face_enabled, c.detect_hz, c.face_hz,
                       c.probed_width, c.probed_height};
}

Json::Value zone_json(const Zone& z) {
    Json::Value v;
    v["id"] = static_cast<Json::Int64>(z.id);
    v["type"] = z.type == ZoneType::Desk ? "desk"
              : z.type == ZoneType::Exit ? "exit"
              : z.type == ZoneType::Restricted ? "restricted" : "perimeter";
    v["name"] = z.name;
    v["employee_id"] = static_cast<Json::Int64>(z.employee_id);
    v["employee_name"] = z.employee_name;
    v["employee_code"] = z.employee_code;
    v["enabled"] = z.enabled;
    Json::Value poly(Json::arrayValue);
    for (const auto& p : z.polygon) {
        Json::Value pt(Json::arrayValue);
        pt.append(p.x);
        pt.append(p.y);
        poly.append(pt);
    }
    v["polygon"] = poly;
    return v;
}

Zone zone_from(const Json::Value& b) {
    Zone z;
    const std::string t = b["type"].asString();
    z.type = t == "desk" ? ZoneType::Desk
           : t == "exit" ? ZoneType::Exit
           : t == "restricted" ? ZoneType::Restricted : ZoneType::Perimeter;
    z.name = b["name"].asString();
    z.employee_id = b.isMember("employee_id") ? b["employee_id"].asInt64() : 0;
    z.enabled = b.isMember("enabled") ? b["enabled"].asBool() : true;
    for (const auto& pt : b["polygon"])
        if (pt.isArray() && pt.size() == 2)
            z.polygon.emplace_back(pt[0].asFloat(), pt[1].asFloat());
    return z;
}

}  // namespace

void register_config(Context& ctx) {
    // ── cameras ────────────────────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/cameras",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  if (req->method() == drogon::Get) {
                      Json::Value out(Json::arrayValue);
                      for (const auto& c : ctx.repos->cameras(t)) out.append(camera_json(c));
                      cb(json_ok(out));
                      return;
                  }
                  const auto body = body_of(req);
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  const auto c = camera_from(*body);
                  const std::string problem = camera_problem(draft_of(c));
                  if (!problem.empty()) {
                      cb(json_error(drogon::k400BadRequest, problem));
                      return;
                  }
                  const auto id = ctx.repos->create_camera(t, c);
                  // Before reload(), not after: the worker is built with the
                  // module set it is given, so applying assignment afterwards
                  // would start the camera with the defaults and only correct
                  // it on the next reload.
                  if (body->isMember("modules")) apply_modules(ctx, t, id, (*body)["modules"]);
                  ctx.engine->reload();
                  Json::Value v = camera_json(c);
                  v["id"] = static_cast<Json::Int64>(id);
                  cb(json_ok(v));
              }),
        {drogon::Get, drogon::Post});

    drogon::app().registerHandler(
        "/api/cameras/{id}",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req);
                  const auto existing = ctx.repos->camera(t, id);
                  if (!existing) {
                      // 404 rather than 403: whether a camera exists in another
                      // tenant is itself information.
                      cb(json_error(drogon::k404NotFound, "no such camera"));
                      return;
                  }
                  if (req->method() == drogon::Delete) {
                      ctx.repos->delete_camera(t, id);
                      ctx.engine->reload();
                      Json::Value v;
                      v["deleted"] = static_cast<Json::Int64>(id);
                      cb(json_ok(v));
                      return;
                  }
                  const auto body = body_of(req);
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  const auto c = camera_from(*body, *existing);
                  const std::string problem = camera_problem(draft_of(c));
                  if (!problem.empty()) {
                      cb(json_error(drogon::k400BadRequest, problem));
                      return;
                  }
                  ctx.repos->update_camera(t, id, c);
                  if (body->isMember("modules")) apply_modules(ctx, t, id, (*body)["modules"]);
                  ctx.engine->reload();
                  cb(json_ok(camera_json(c)));
              }),
        {drogon::Patch, drogon::Delete});

    // ── zones ──────────────────────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/cameras/{id}/zones",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t camera_id = path_id(req, 1);
                  if (!ctx.repos->camera(t, camera_id)) {
                      cb(json_error(drogon::k404NotFound, "no such camera"));
                      return;
                  }
                  if (req->method() == drogon::Get) {
                      Json::Value out(Json::arrayValue);
                      for (const auto& z : ctx.repos->zones(t, camera_id))
                          out.append(zone_json(z));
                      cb(json_ok(out));
                      return;
                  }
                  const auto body = body_of(req);
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  const Zone z = zone_from(*body);
                  if (z.polygon.size() < 3) {
                      cb(json_error(drogon::k400BadRequest,
                                    "a zone needs at least three points"));
                      return;
                  }
                  if (z.type == ZoneType::Desk && z.employee_id == 0) {
                      cb(json_error(drogon::k400BadRequest,
                                    "a desk zone must be assigned to an employee"));
                      return;
                  }
                  const auto id = ctx.repos->create_zone(t, camera_id, z);
                  // The zone applies to the running camera at once — an operator
                  // drawing a zone expects it to be live, not live after a restart.
                  ctx.engine->reload();
                  Json::Value v = zone_json(z);
                  v["id"] = static_cast<Json::Int64>(id);
                  cb(json_ok(v));
              }),
        {drogon::Get, drogon::Post});

    drogon::app().registerHandler(
        "/api/zones/{id}",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req);
                  if (req->method() == drogon::Delete) {
                      // This is the side panel's Reset. Nothing else ever clears
                      // a zone: an assignment persists until an operator says so.
                      ctx.repos->delete_zone(t, id);
                      ctx.engine->reload();
                      Json::Value v;
                      v["deleted"] = static_cast<Json::Int64>(id);
                      cb(json_ok(v));
                      return;
                  }
                  const auto body = body_of(req);
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  const Zone z = zone_from(*body);
                  if (z.polygon.size() < 3) {
                      cb(json_error(drogon::k400BadRequest,
                                    "a zone needs at least three points"));
                      return;
                  }
                  ctx.repos->update_zone(t, id, z);
                  ctx.engine->reload();
                  cb(json_ok(zone_json(z)));
              }),
        {drogon::Patch, drogon::Delete});

    // ── gate lines ─────────────────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/cameras/{id}/gate-lines",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t camera_id = path_id(req, 1);
                  if (!ctx.repos->camera(t, camera_id)) {
                      cb(json_error(drogon::k404NotFound, "no such camera"));
                      return;
                  }
                  if (req->method() == drogon::Get) {
                      Json::Value out(Json::arrayValue);
                      for (const auto& g : ctx.repos->gate_lines(t, camera_id)) {
                          Json::Value v;
                          v["id"] = static_cast<Json::Int64>(g.id);
                          v["name"] = g.name;
                          v["ax"] = g.line.a.x;
                          v["ay"] = g.line.a.y;
                          v["bx"] = g.line.b.x;
                          v["by"] = g.line.b.y;
                          v["enabled"] = g.enabled;
                          out.append(v);
                      }
                      cb(json_ok(out));
                      return;
                  }
                  const auto body = body_of(req);
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  GateLineCfg g;
                  g.name = (*body)["name"].asString().empty() ? "Entrance"
                                                              : (*body)["name"].asString();
                  g.line.a = {(*body)["ax"].asFloat(), (*body)["ay"].asFloat()};
                  g.line.b = {(*body)["bx"].asFloat(), (*body)["by"].asFloat()};
                  if (g.line.length() < 5.0f) {
                      cb(json_error(drogon::k400BadRequest,
                                    "the line is too short to be crossed"));
                      return;
                  }
                  const auto id = ctx.repos->create_gate_line(t, camera_id, g);
                  ctx.engine->reload();
                  Json::Value v;
                  v["id"] = static_cast<Json::Int64>(id);
                  cb(json_ok(v));
              }),
        {drogon::Get, drogon::Post});

    drogon::app().registerHandler(
        "/api/gate-lines/{id}",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  ctx.repos->delete_gate_line(t, path_id(req));
                  ctx.engine->reload();
                  Json::Value v;
                  v["deleted"] = true;
                  cb(json_ok(v));
              }),
        {drogon::Delete});

    // ── AI modules per camera ──────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/cameras/{id}/modules",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t camera_id = path_id(req, 1);
                  const auto cam = ctx.repos->camera(t, camera_id);
                  if (!cam) {
                      cb(json_error(drogon::k404NotFound, "no such camera"));
                      return;
                  }
                  if (req->method() == drogon::Put) {
                      const auto body = body_of(req);
                      if (!body) {
                          cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                          return;
                      }
                      for (const auto& id : kModuleIds)
                          if (body->isMember(id))
                              ctx.repos->set_camera_module(t, camera_id, id,
                                                           (*body)[id].asBool());
                      ctx.engine->reload();
                  }

                  const auto stored = ctx.repos->camera_modules(t, camera_id);
                  const auto zones = ctx.repos->zones(t, camera_id);
                  const auto lines = ctx.repos->gate_lines(t, camera_id);

                  int perimeter_zones = 0, desk_zones = 0;
                  for (const auto& z : zones) {
                      if (z.type == ZoneType::Desk) ++desk_zones;
                      else ++perimeter_zones;
                  }

                  Json::Value out(Json::arrayValue);
                  for (const auto& m : kModules) {
                      const auto it = stored.find(m.id);
                      const bool enabled = it == stored.end() || it->second;

                      // What a module NEEDS in order to do anything, checked
                      // rather than assumed. A module that is on but has no
                      // zone drawn is running and silent, and saying so is the
                      // difference between a configuration page and a list of
                      // switches.
                      std::string blocker;
                      if (std::string(m.id) == "perimeter" && perimeter_zones == 0)
                          blocker = "no perimeter or restricted zone has been drawn on this camera";
                      else if (std::string(m.id) == "desk" && desk_zones == 0)
                          blocker = "no desk zone has been drawn on this camera";
                      else if (std::string(m.id) == "unauthorised" && !cam->face_enabled)
                          blocker = "face recognition is off for this camera";
                      // Attendance is judged on what it actually needs — a face
                      // and a line to cross — NOT on the camera's role.
                      //
                      // The role used to be checked here, and once a site could
                      // name its own roles that check became a trap: a camera
                      // called "reception gate" was fully configured, had its
                      // line drawn, and reported "this camera's role is not
                      // 'gate'" forever. The role is a label for people to read;
                      // the gate LINE is the thing the module counts across.
                      else if (std::string(m.id) == "attendance" && !cam->face_enabled)
                          blocker = "face recognition is off for this camera";

                      // A camera whose faces are simply too small identifies
                      // nobody, silently. That is correct behaviour and a
                      // terrible experience, so it is reported as a blocker
                      // like any other.
                      if (blocker.empty() &&
                          (std::string(m.id) == "attendance" ||
                           std::string(m.id) == "unauthorised")) {
                          // The floor comes from the identifier itself. It was
                          // written out as "60" here and then the identifier
                          // moved to 24, so this went on telling operators to
                          // move cameras that were already working — a hardcoded
                          // copy of somebody else's constant, drifting quietly.
                          const IdentifierConfig gates;
                          for (const auto& w : ctx.engine->worker_stats()) {
                              if (w.camera_id != camera_id || w.faces_seen < 30) continue;
                              if (w.face_px_mean < gates.min_face_px) {
                                  std::ostringstream o;
                                  o << "faces on this camera average "
                                    << static_cast<int>(w.face_px_mean)
                                    << " px and matching starts at "
                                    << static_cast<int>(gates.min_face_px)
                                    << " — most faces here are never compared at all, "
                                       "so the camera is too far from where people pass";
                                  blocker = o.str();
                              }
                              break;
                          }
                      }

                      Json::Value v;
                      v["id"] = m.id;
                      v["name"] = m.name;
                      v["blurb"] = m.blurb;
                      v["needs"] = m.needs;
                      v["draws"] = m.draws;             // "zone" | "desk" | "line" | ""
                      v["wants_face"] = std::string(m.wants_face) == "1";
                      v["enabled"] = enabled;
                      v["active"] = enabled && blocker.empty();
                      v["blocker"] = blocker;
                      out.append(v);
                  }
                  cb(json_ok(out));
              }),
        {drogon::Get, drogon::Put});

    // ── what each camera is actually doing, right now ──────────────────────
    //
    // The live wall showed which cameras were up but not how fast any of them
    // was running, and "up" is not the same thing: a camera the board cannot
    // keep up with stays online and simply drifts further behind real time.
    // Both numbers are reported so the wall can show the gap rather than hide
    // it — decoded, and what the stream says it is sending.
    // Ask a camera what it actually streams, before anybody commits to it.
    //
    // "Sub-stream (recommended)" was a recommendation made without ever opening
    // the URL, and the sizes stored beside it were wrong for every camera on
    // this site — one recorded as 640x360 was sending 640x480, another recorded
    // as 720x576 was sending 864x480. Choosing a stream decides whether faces
    // are big enough to recognise and whether the board can keep up, so it
    // should not be guesswork.
    //
    // This opens the REAL pipeline, so what it reports is what the product will
    // get. It takes a few seconds per URL and runs off the event loop, because
    // an RTSP handshake must never block the API.
    drogon::app().registerHandler(
        "/api/cameras/probe",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const auto body = req->getJsonObject();
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  const std::string main_url = (*body)["rtsp_main"].asString();
                  const std::string sub_url = (*body)["rtsp_sub"].asString();
                  if (main_url.empty() && sub_url.empty()) {
                      cb(json_error(drogon::k400BadRequest,
                                    "give rtsp_main, rtsp_sub, or both"));
                      return;
                  }

                  // IF THE CAMERA IS ALREADY RUNNING, ASK THE WORKER, NOT THE CAMERA.
                  //
                  // These cameras serve ONE RTSP session per stream. Dialling a
                  // second connection to a camera this appliance is already
                  // streaming gets no frame at all — and it looks exactly like a
                  // wrong password, so the operator is sent to debug something
                  // that is working perfectly. The running worker already knows
                  // the answer, and it costs nothing to read it.
                  const std::int64_t cam_id = (*body)["camera_id"].asInt64();
                  if (cam_id != 0) {
                      for (const auto& w : ctx.engine->worker_stats()) {
                          if (w.camera_id != cam_id || w.width <= 0) continue;
                          const bool on_sub = (*body)["stream_pref"].asString() != "main";

                          // THE CAMERA'S OWN SIZE, NOT THE SIZE WE CUT IT DOWN TO.
                          //
                          // w.width/w.height is what comes OUT of the pipeline.
                          // With a frame limit in force that is our number, not
                          // the camera's: a 3200x1800 stream under a 1000 px
                          // limit reported "1000x562", and the dropdown then
                          // offered that back as if it were a camera setting.
                          // The question being answered is what the camera can
                          // give, so the answer has to be the source size.
                          //
                          // cameras.probed_width is kept true by the engine's
                          // periodic sync, and w.native_size says whether the
                          // pipeline is resizing at all.
                          int sw = w.width, sh = w.height;
                          if (!w.native_size) {
                              const auto row = ctx.repos->camera(t, cam_id);
                              if (row && row->probed_width > 0) {
                                  sw = row->probed_width;
                                  sh = row->probed_height;
                              }
                          }

                          Json::Value live;
                          live["ok"] = true;
                          live["width"] = sw;
                          live["height"] = sh;
                          live["fps"] = std::round(w.stream_fps * 10.0) / 10.0;
                          live["decode_path"] = w.decode_path;
                          live["live"] = true;
                          // Only the stream actually open can be reported; the
                          // other one is still held by nobody and can be dialled.
                          Json::Value v;
                          v[on_sub ? "sub" : "main"] = live;
                          const std::string other = on_sub ? main_url : sub_url;
                          if (!other.empty()) {
                              std::thread([other, on_sub, v, cb = std::move(cb)]() mutable {
                                  const auto f = probe_stream(other, 12);
                                  Json::Value o;
                                  o["ok"] = f.ok;
                                  if (f.ok) {
                                      o["width"] = f.width;
                                      o["height"] = f.height;
                                      o["fps"] = std::round(f.fps * 10.0) / 10.0;
                                      o["decode_path"] = f.decode_path;
                                  } else {
                                      o["error"] = f.error;
                                  }
                                  v[on_sub ? "main" : "sub"] = o;
                                  cb(json_ok(v));
                              }).detach();
                              return;
                          }
                          cb(json_ok(v));
                          return;
                      }
                  }

                  std::thread([main_url, sub_url, cb = std::move(cb)]() mutable {
                      const auto one = [](const std::string& u) {
                          Json::Value v;
                          if (u.empty()) return v;
                          const auto f = probe_stream(u, 12);
                          v["ok"] = f.ok;
                          if (f.ok) {
                              v["width"] = f.width;
                              v["height"] = f.height;
                              v["fps"] = std::round(f.fps * 10.0) / 10.0;
                              v["decode_path"] = f.decode_path;
                              v["label"] = std::to_string(f.width) + "x" +
                                           std::to_string(f.height);
                          } else {
                              v["error"] = f.error;
                          }
                          return v;
                      };
                      Json::Value v;
                      v["main"] = one(main_url);
                      v["sub"] = one(sub_url);
                      cb(json_ok(v));
                  }).detach();
              }),
        {drogon::Post});

    // ── what a camera CAN do, not only what it is doing ────────────────────
    //
    // The probe above opens the stream and reports what is coming out of it.
    // That answers "which of these two URLs is better" and nothing else. The
    // question an operator actually asks at the wall is "what else can this
    // camera give me", and the only honest source for that is the camera.
    //
    // ONVIF is read-only here ON PURPOSE. Writing a setting back is a separate
    // and much less trustworthy operation: the VARITO at the front door accepts
    // SetVideoEncoderConfiguration with FrameRateLimit=10, answers with no
    // fault, and goes on sending 25 — so a page that writes and then reports
    // success would be lying. Showing what the camera supports, and what it is
    // currently set to, is information that can be relied on.
    drogon::app().registerHandler(
        "/api/cameras/onvif",
        guard(ctx, {Role::Admin},
              [](const HttpRequestPtr& req, Callback&& cb, const Claims&, db::TenantId) {
                  const auto body = req->getJsonObject();
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  // Either URL identifies the same device, so whichever is
                  // filled in is enough — an operator who has typed only the
                  // main URL should still be offered the sub-stream's sizes.
                  std::string url = (*body)["rtsp_main"].asString();
                  if (url.empty()) url = (*body)["rtsp_sub"].asString();
                  const auto target = onvif_target_from_rtsp(url);
                  if (!target.ok) {
                      cb(json_error(drogon::k400BadRequest,
                                    "give an rtsp:// URL with the camera's address in it"));
                      return;
                  }

                  // OFF THE EVENT LOOP. Two or three SOAP round trips to a
                  // camera on a slow link is seconds, and a Drogon event loop
                  // blocked for seconds stops serving every other request on
                  // that loop — including the live view the operator is
                  // watching while they edit.
                  std::thread([target, cb = std::move(cb)]() mutable {
                      std::string err;
                      const auto streams = onvif_streams(target, &err);
                      Json::Value v;
                      Json::Value arr(Json::arrayValue);
                      for (const auto& s : streams) {
                          Json::Value o;
                          o["profile"] = s.profile;
                          // The handle a change is addressed to. Without it the
                          // page can show what a camera supports and never ask
                          // for any of it.
                          o["token"] = s.token;
                          o["encoding"] = s.encoding;
                          o["width"] = s.width;
                          o["height"] = s.height;
                          o["fps"] = s.fps;
                          o["bitrate"] = s.bitrate;
                          o["gov_length"] = s.gov_length;
                          o["fps_min"] = s.fps_min;
                          o["fps_max"] = s.fps_max;
                          Json::Value rs(Json::arrayValue);
                          for (const auto& r : s.resolutions) {
                              Json::Value e;
                              e["width"] = r.width;
                              e["height"] = r.height;
                              rs.append(e);
                          }
                          o["resolutions"] = rs;
                          arr.append(o);
                      }
                      v["streams"] = arr;
                      v["ok"] = !streams.empty();
                      if (!err.empty()) v["error"] = err;
                      cb(json_ok(v));
                  }).detach();
              }),
        {drogon::Post});

    // ── change a camera's resolution, and report what it REALLY did ────────
    //
    // Writing is separated from reading above because the two deserve different
    // amounts of trust. A read is a fact. A write to this hardware is a
    // request: the VARITO honours a resolution change and silently ignores a
    // frame-rate one, both answered with the same cheerful success. So this
    // writes, reads the camera back, and reports the READ — never the write's
    // own answer.
    drogon::app().registerHandler(
        "/api/cameras/onvif/apply",
        guard(ctx, {Role::Admin},
              [](const HttpRequestPtr& req, Callback&& cb, const Claims&, db::TenantId) {
                  const auto body = req->getJsonObject();
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  std::string url = (*body)["rtsp_main"].asString();
                  if (url.empty()) url = (*body)["rtsp_sub"].asString();
                  const auto target = onvif_target_from_rtsp(url);
                  const std::string token = (*body)["token"].asString();
                  const int w = (*body)["width"].asInt();
                  const int h = (*body)["height"].asInt();
                  if (!target.ok || token.empty() || w <= 0 || h <= 0) {
                      cb(json_error(drogon::k400BadRequest,
                                    "give the camera's rtsp URL, the stream token, "
                                    "and the width and height to set"));
                      return;
                  }

                  std::thread([target, token, w, h, cb = std::move(cb)]() mutable {
                      std::string err;
                      OnvifStream after;
                      const bool ok = onvif_apply_resolution(target, token, w, h, &after, &err);
                      Json::Value v;
                      v["ok"] = ok;
                      if (!err.empty()) v["error"] = err;
                      // The camera's state afterwards, whether or not the change
                      // took: a page that failed to change anything still needs
                      // to show what is actually there.
                      if (after.width > 0) {
                          v["width"] = after.width;
                          v["height"] = after.height;
                          v["fps"] = after.fps;
                          v["encoding"] = after.encoding;
                      }
                      cb(json_ok(v));
                  }).detach();
              }),
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/camera-stats",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr&, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  // Scoped to the tenant's own cameras: worker_stats() covers
                  // the whole appliance, which serves every client company.
                  std::set<std::int64_t> mine;
                  for (const auto& c : ctx.repos->cameras(t)) mine.insert(c.id);

                  Json::Value out(Json::arrayValue);
                  for (const auto& w : ctx.engine->worker_stats()) {
                      if (mine.find(w.camera_id) == mine.end()) continue;
                      Json::Value v;
                      v["id"] = static_cast<Json::Int64>(w.camera_id);
                      v["online"] = w.online;
                      v["decode_fps"] = w.decode_fps;
                      v["stream_fps"] = w.stream_fps;
                      v["width"] = w.width;
                      v["height"] = w.height;
                      v["decode_path"] = w.decode_path;
                      // The judgement, made once here rather than repeated in
                      // every page that wants to show it.
                      v["behind"] = w.stream_fps > 1.0 &&
                                    w.decode_fps < w.stream_fps * 0.7;
                      out.append(v);
                  }
                  cb(json_ok(out));
              }),
        {drogon::Get});

    // ── the module catalogue, with no camera in the question ───────────────
    //
    // The per-camera endpoint above answers "what is this camera doing and why
    // is it not doing it", which needs a camera that already exists. The ADD
    // form has no camera yet and still has to offer the same list, so the
    // catalogue is served on its own rather than duplicated in the frontend —
    // a hardcoded copy in the UI is how a page ends up offering a module the
    // build does not have.
    drogon::app().registerHandler(
        "/api/modules",
        guard(ctx, {Role::Admin},
              [](const HttpRequestPtr&, Callback&& cb, const Claims&, db::TenantId) {
                  Json::Value out(Json::arrayValue);
                  for (const auto& m : kModules) {
                      Json::Value v;
                      v["id"] = m.id;
                      v["name"] = m.name;
                      v["blurb"] = m.blurb;
                      v["needs"] = m.needs;
                      v["draws"] = m.draws;
                      v["wants_face"] = std::string(m.wants_face) == "1";
                      // No camera, so nothing can be said about these two. They
                      // are present so the shape matches the per-camera reply
                      // and one component can render both.
                      v["enabled"] = true;
                      v["active"] = false;
                      v["blocker"] = "";
                      out.append(v);
                  }
                  cb(json_ok(out));
              }),
        {drogon::Get});

    // ── settings ───────────────────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/settings",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  if (req->method() == drogon::Get) {
                      const auto s = ctx.repos->settings(t);
                      Json::Value v;
                      v["timezone"] = s.timezone;
                      v["office_start"] = s.office_start.substr(0, 5);
                      v["office_end"] = s.office_end.substr(0, 5);
                      v["grace_minutes"] = s.grace_minutes;
                      v["early_leave_grace_minutes"] = s.early_leave_grace_minutes;
                      v["telegram_chat_id"] = s.telegram_chat_id;
                      v["smtp_host"] = s.smtp_host;
                      v["smtp_port"] = s.smtp_port;
                      v["smtp_user"] = s.smtp_user;
                      v["smtp_from"] = s.smtp_from;
                      v["smtp_from_name"] = s.smtp_from_name;
                      v["email_to"] = s.email_to;
                      // Secrets are never returned, only whether they are set.
                      // A settings page that echoes a bot token turns one XSS
                      // into a permanently compromised channel.
                      v["telegram_configured"] = !s.telegram_bot_token.empty();
                      v["smtp_password_set"] = !s.smtp_password.empty();
                      cb(json_ok(v));
                      return;
                  }
                  const auto body = body_of(req);
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  auto s = ctx.repos->settings(t);
                  const auto& b = *body;
                  if (b.isMember("timezone")) s.timezone = b["timezone"].asString();
                  if (b.isMember("office_start")) s.office_start = b["office_start"].asString();
                  if (b.isMember("office_end")) s.office_end = b["office_end"].asString();
                  if (b.isMember("grace_minutes")) s.grace_minutes = b["grace_minutes"].asInt();
                  if (b.isMember("early_leave_grace_minutes"))
                      s.early_leave_grace_minutes = b["early_leave_grace_minutes"].asInt();
                  if (b.isMember("telegram_chat_id"))
                      s.telegram_chat_id = b["telegram_chat_id"].asString();
                  // A blank secret means "leave it alone", not "clear it" — the
                  // GET above cannot return it, so a round-trip would otherwise
                  // wipe it.
                  if (b.isMember("telegram_bot_token") &&
                      !b["telegram_bot_token"].asString().empty())
                      s.telegram_bot_token = b["telegram_bot_token"].asString();
                  if (b.isMember("smtp_host")) s.smtp_host = b["smtp_host"].asString();
                  if (b.isMember("smtp_port")) s.smtp_port = b["smtp_port"].asInt();
                  if (b.isMember("smtp_user")) s.smtp_user = b["smtp_user"].asString();
                  if (b.isMember("smtp_password") && !b["smtp_password"].asString().empty())
                      s.smtp_password = b["smtp_password"].asString();
                  if (b.isMember("smtp_from")) s.smtp_from = b["smtp_from"].asString();
                  if (b.isMember("smtp_from_name"))
                      s.smtp_from_name = b["smtp_from_name"].asString();
                  if (b.isMember("email_to")) s.email_to = b["email_to"].asString();
                  ctx.repos->save_settings(t, s);
                  Json::Value v;
                  v["saved"] = true;
                  cb(json_ok(v));
              }),
        {drogon::Get, drogon::Put});
}

}  // namespace so::api
