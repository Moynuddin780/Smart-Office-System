#include "api/server.hpp"

#include "core/log.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/MultiPart.h>

#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace so::api {
namespace {

// ── enrolment thresholds ──────────────────────────────────────────────────
// Every one of these was measured on the sibling project's real registry
// (11 profiles, 203 images) rather than chosen.

// A new profile below this is refused. An EXISTING one can be topped up with a
// single photo, because what it already holds is not thrown away.
constexpr int kMinImages = 5;
constexpr int kMaxImages = 30;

// Two DIFFERENT people whose profile vectors sit this close cannot be told
// apart by any threshold - the match flips between them frame to frame. A
// healthy pair sits near 0.2; every pair past ~0.5 produced the "one person,
// several names" behaviour. Warn at enrolment, where it is still cheap to fix.
constexpr float kDuplicateWarn = 0.50f;

// A profile is only as good as the agreement between its OWN photos. Healthy
// profiles agreed with themselves at a median cosine of 0.64-0.74; three sat at
// 0.32, 0.38 and 0.40 - one built from downcast frames, two from group shots
// where the largest face was not always the same person. Averaging disagreeing
// vectors produces a profile pointing at the MIDDLE of the gallery, which then
// wins the argmax against any face too poor to look like anybody in particular:
// the 0.40 profile was proposed for 6 of 8 tracks on a real recording and
// outscored the right person on his own face, 0.59 to 0.54. Every per-image
// quality score in it was 80-97, so nothing visible said anything was wrong.
constexpr float kIncoherentWarn = 0.50f;

std::string tenant_slug(const Context& ctx, db::TenantId t) {
    for (const auto& x : ctx.repos->all_tenants())
        if (x.id == t.value) return x.slug;
    return "";
}

Json::Value photo_json(const std::string& filename, const so::app::Engine::EnrolResult& r) {
    Json::Value v;
    v["filename"] = filename;
    v["accepted"] = r.ok;
    v["quality"] = r.quality;
    v["yaw"] = r.yaw;
    v["face_px"] = r.face_px;
    v["det_score"] = r.score;
    v["other_faces"] = r.other_faces;
    if (!r.ok) v["reason"] = r.reason;
    return v;
}

Json::Value profile_json(const db::FaceProfile& p) {
    Json::Value v;
    v["images"] = p.image_count;
    v["quality"] = p.quality;
    // Rounded HERE rather than in the database: a float4 cannot hold 0.815
    // exactly, so storing a rounded value and reading it back still serialises
    // as 0.8149999976158142. Three decimals is what the number means.
    v["coherence"] = p.coherence >= 0
        ? Json::Value(std::round(p.coherence * 1000.0) / 1000.0)
        : Json::Value(Json::nullValue);
    v["incoherent"] = p.coherence >= 0 && p.coherence < kIncoherentWarn;
    v["understrength"] = p.image_count > 0 && p.image_count < kMinImages;
    return v;
}

/// The things an operator should know about a profile, in words they can act on.
/// Surfaced rather than enforced: the useful repair for a thin profile is "add
/// two more photos", not "start again".
Json::Value warnings_for(const Context& ctx, db::TenantId t, std::int64_t employee_id,
                         const db::FaceProfile& p) {
    Json::Value out(Json::arrayValue);
    if (p.image_count > 0 && p.image_count < kMinImages) {
        Json::Value w;
        w["kind"] = "understrength";
        w["text"] = "This profile holds " + std::to_string(p.image_count) +
                    " photos. Below " + std::to_string(kMinImages) +
                    " it describes one viewing angle well and every other angle "
                    "badly, so the person is recognised at the door and missed "
                    "down a corridor.";
        out.append(w);
    }
    if (p.coherence >= 0 && p.coherence < kIncoherentWarn) {
        Json::Value w;
        w["kind"] = "incoherent";
        std::ostringstream coh;
        coh << std::fixed << std::setprecision(2) << p.coherence;
        w["text"] = "This profile's own photos only agree with each other at " + coh.str() +
                    ". A healthy profile sits near 0.65. Below 0.5 the averaged "
                    "vector points at the middle of the registry rather than at "
                    "this person, and it will start being proposed for other "
                    "people. Check the photos for a second face or a run of "
                    "downcast shots, and delete those.";
        out.append(w);
    }
    for (const auto& n : ctx.repos->too_close(t, employee_id, kDuplicateWarn)) {
        Json::Value w;
        w["kind"] = "duplicate";
        w["employee_id"] = static_cast<Json::Int64>(n.employee_id);
        std::ostringstream sim;
        sim << std::fixed << std::setprecision(2) << n.similarity;
        w["text"] = "This profile sits at " + sim.str() + " cosine from " + n.name +
                    ". Two profiles this close cannot be told apart by any "
                    "threshold - the match will flip between them frame to frame. "
                    "Usually one of them holds a photo of the other person.";
        out.append(w);
    }
    return out;
}

Json::Value employee_json(const db::Employee& e) {
    Json::Value v;
    v["id"] = static_cast<Json::Int64>(e.id);
    v["emp_code"] = e.emp_code;
    v["name"] = e.name;
    v["department"] = e.department;
    v["designation"] = e.designation;
    v["email"] = e.email;
    v["phone"] = e.phone;
    v["active"] = e.active;
    v["face_profiles"] = e.face_profiles;
    return v;
}

db::Employee employee_from(const Json::Value& b, const db::Employee& base = {}) {
    db::Employee e = base;
    if (b.isMember("emp_code")) e.emp_code = b["emp_code"].asString();
    if (b.isMember("name")) e.name = b["name"].asString();
    if (b.isMember("department")) e.department = b["department"].asString();
    if (b.isMember("designation")) e.designation = b["designation"].asString();
    if (b.isMember("email")) e.email = b["email"].asString();
    if (b.isMember("phone")) e.phone = b["phone"].asString();
    if (b.isMember("active")) e.active = b["active"].asBool();
    return e;
}

}  // namespace

void register_people(Context& ctx) {
    drogon::app().registerHandler(
        "/api/employees",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  if (req->method() == drogon::Get) {
                      Json::Value out(Json::arrayValue);
                      for (const auto& e : ctx.repos->employees(t))
                          out.append(employee_json(e));
                      cb(json_ok(out));
                      return;
                  }
                  const auto body = body_of(req);
                  if (!body) {
                      cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                      return;
                  }
                  const auto e = employee_from(*body);
                  if (e.name.empty() || e.emp_code.empty()) {
                      cb(json_error(drogon::k400BadRequest,
                                    "a name and an employee code are required"));
                      return;
                  }
                  // The code is unique per tenant. Letting the index raise
                  // surfaced as a bare 500, which told whoever typed it nothing
                  // at all — least of all that the code was simply taken, and by
                  // whom. This is an ordinary conflict, so it is answered as one.
                  if (const auto clash = ctx.repos->employee_by_code(t, e.emp_code)) {
                      cb(json_error(drogon::k409Conflict,
                                    "Employee code \"" + e.emp_code + "\" is already used by " +
                                    clash->name + ". Give this person a different code."));
                      return;
                  }
                  const auto id = ctx.repos->create_employee(t, e);
                  Json::Value v = employee_json(e);
                  v["id"] = static_cast<Json::Int64>(id);
                  cb(json_ok(v));
              }),
        {drogon::Get, drogon::Post});

    drogon::app().registerHandler(
        "/api/employees/{id}",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req);
                  const auto existing = ctx.repos->employee(t, id);
                  if (!existing) {
                      cb(json_error(drogon::k404NotFound, "no such employee"));
                      return;
                  }
                  if (req->method() == drogon::Delete) {
                      ctx.repos->delete_employee(t, id);
                      // Their faces go with them, and the running gallery is
                      // refreshed so a deleted employee stops matching NOW
                      // rather than at the next restart.
                      ctx.engine->reload_gallery();
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
                  const auto e = employee_from(*body, *existing);
                  if (e.emp_code.empty()) {
                      cb(json_error(drogon::k400BadRequest, "an employee code is required"));
                      return;
                  }
                  if (e.emp_code != existing->emp_code) {
                      if (const auto clash = ctx.repos->employee_by_code(t, e.emp_code)) {
                          cb(json_error(drogon::k409Conflict,
                                        "Employee code \"" + e.emp_code + "\" is already used by " +
                                        clash->name + ". Give this person a different code."));
                          return;
                      }
                  }
                  ctx.repos->update_employee(t, id, e);
                  ctx.engine->reload_gallery();
                  cb(json_ok(employee_json(e)));
              }),
        {drogon::Patch, drogon::Delete});

    // ── the face registry ──────────────────────────────────────────────
    //
    // Ported from the VisionX enrolment model, whose thresholds were measured
    // on a real 11-profile registry rather than chosen.

    drogon::app().registerHandler(
        "/api/employees/{id}/enroll",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req, 1);
                  const auto emp = ctx.repos->employee(t, id);
                  if (!emp) {
                      cb(json_error(drogon::k404NotFound, "no such employee"));
                      return;
                  }

                  drogon::MultiPartParser parser;
                  if (parser.parse(req) != 0 || parser.getFiles().empty()) {
                      cb(json_error(drogon::k400BadRequest,
                                    "upload one or more photos as multipart form data"));
                      return;
                  }

                  const auto& params = parser.getParameters();
                  const auto mode_it = params.find("mode");
                  const std::string mode = mode_it == params.end() ? "add" : mode_it->second;
                  const auto src_it = params.find("source");
                  const std::string source =
                      (src_it != params.end() && src_it->second == "capture") ? "capture"
                                                                              : "upload";

                  auto existing = ctx.repos->face_images(t, id);
                  const bool is_new = existing.empty();
                  const int kept = (mode == "replace") ? 0 : static_cast<int>(existing.size());
                  const int incoming = static_cast<int>(parser.getFiles().size());

                  if (kept + incoming > kMaxImages) {
                      cb(json_error(drogon::k400BadRequest,
                                    "this profile would hold " +
                                        std::to_string(kept + incoming) +
                                        " images; the maximum is " +
                                        std::to_string(kMaxImages)));
                      return;
                  }

                  // Grade everything BEFORE touching the database, so a batch
                  // that fails the new-profile minimum changes nothing.
                  struct Graded {
                      std::string filename;
                      so::app::Engine::EnrolResult r;
                      std::string_view bytes;
                  };
                  std::vector<Graded> graded;
                  graded.reserve(static_cast<std::size_t>(incoming));
                  int accepted_now = 0;
                  for (const auto& f : parser.getFiles()) {
                      const std::string_view raw = f.fileContent();
                      std::vector<unsigned char> buf(raw.begin(), raw.end());
                      const cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
                      auto r = ctx.engine->enrol(img);
                      if (r.ok) ++accepted_now;
                      graded.push_back(Graded{f.getFileName(), std::move(r), raw});
                  }

                  // A NEW profile needs a real spread. An existing one can be
                  // topped up with a single photo, because the evidence it
                  // already holds is not thrown away.
                  if (is_new && mode != "replace" && accepted_now < kMinImages) {
                      Json::Value v;
                      v["error"] = "a new profile needs at least " +
                                   std::to_string(kMinImages) + " usable photos; " +
                                   std::to_string(accepted_now) + " of " +
                                   std::to_string(incoming) + " were usable";
                      Json::Value photos(Json::arrayValue);
                      for (const auto& g : graded) photos.append(photo_json(g.filename, g.r));
                      v["photos"] = photos;
                      v["accepted"] = accepted_now;
                      v["total"] = incoming;
                      auto resp = drogon::HttpResponse::newHttpJsonResponse(v);
                      resp->setStatusCode(drogon::k400BadRequest);
                      cb(resp);
                      return;
                  }

                  if (mode == "replace") {
                      for (const auto& old : existing) ctx.snapshots->remove(old.image_path);
                      ctx.repos->delete_face_images(t, id);
                  }

                  const std::string slug = tenant_slug(ctx, t);
                  Json::Value photos(Json::arrayValue);
                  int stored = 0;
                  for (auto& g : graded) {
                      Json::Value pj = photo_json(g.filename, g.r);
                      if (g.r.ok) {
                          // The ORIGINAL bytes are kept, not a re-encode: the
                          // embedding was computed from these pixels and a
                          // registry that shows different ones is lying about
                          // what the recogniser saw.
                          const std::string path = ctx.snapshots->put_bytes(
                              t.value, slug, "faces", g.bytes.data(), g.bytes.size());
                          const auto img_id = ctx.repos->add_face_image(
                              t, id, g.r.embedding, g.r.quality, g.r.yaw, g.r.face_px,
                              g.r.score, path, g.filename,
                              static_cast<int>(g.bytes.size()), source);
                          pj["id"] = static_cast<Json::Int64>(img_id);
                          ++stored;
                      }
                      photos.append(pj);
                  }

                  const auto prof = ctx.repos->recompute_profile(t, id);
                  if (stored > 0) ctx.engine->reload_gallery();

                  Json::Value v;
                  v["accepted"] = stored;
                  v["total"] = incoming;
                  v["photos"] = photos;
                  v["profile"] = profile_json(prof);
                  v["warnings"] = warnings_for(ctx, t, id, prof);
                  cb(json_ok(v));
              }),
        {drogon::Post});

    // The registry: every profile with the numbers that say whether it works.
    drogon::app().registerHandler(
        "/api/faces",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr&, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  Json::Value out(Json::arrayValue);
                  for (const auto& e : ctx.repos->employees(t)) {
                      const auto prof = ctx.repos->profile(t, e.id);
                      Json::Value v;
                      v["employee_id"] = static_cast<Json::Int64>(e.id);
                      v["emp_code"] = e.emp_code;
                      v["name"] = e.name;
                      v["department"] = e.department;
                      v["active"] = e.active;
                      v["enrolled"] = prof.has_value();
                      if (prof) {
                          v["profile"] = profile_json(*prof);
                          v["warnings"] = warnings_for(ctx, t, e.id, *prof);
                      } else {
                          v["profile"] = Json::Value(Json::nullValue);
                          v["warnings"] = Json::Value(Json::arrayValue);
                      }
                      out.append(v);
                  }
                  cb(json_ok(out));
              }),
        {drogon::Get});

    // One profile's photos, each with what it contributed.
    drogon::app().registerHandler(
        "/api/employees/{id}/faces",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req, 1);
                  if (!ctx.repos->employee(t, id)) {
                      cb(json_error(drogon::k404NotFound, "no such employee"));
                      return;
                  }
                  if (req->method() == drogon::Delete) {
                      for (const auto& f : ctx.repos->face_images(t, id))
                          ctx.snapshots->remove(f.image_path);
                      ctx.repos->delete_face_images(t, id);
                      ctx.engine->reload_gallery();
                      Json::Value v;
                      v["cleared"] = static_cast<Json::Int64>(id);
                      cb(json_ok(v));
                      return;
                  }
                  Json::Value imgs(Json::arrayValue);
                  for (const auto& f : ctx.repos->face_images(t, id)) {
                      Json::Value v;
                      v["id"] = static_cast<Json::Int64>(f.id);
                      v["quality"] = f.quality;
                      v["yaw"] = f.yaw;
                      v["face_px"] = f.face_px;
                      v["det_score"] = f.det_score;
                      v["filename"] = f.filename;
                      v["source"] = f.source;
                      v["created_at"] = f.created_at;
                      v["has_image"] = !f.image_path.empty();
                      imgs.append(v);
                  }
                  const auto prof = ctx.repos->profile(t, id);
                  Json::Value v;
                  v["images"] = imgs;
                  v["profile"] = prof ? profile_json(*prof) : Json::Value(Json::nullValue);
                  v["warnings"] = prof ? warnings_for(ctx, t, id, *prof)
                                       : Json::Value(Json::arrayValue);
                  cb(json_ok(v));
              }),
        {drogon::Get, drogon::Delete});

    // Remove ONE photo. This is the repair path: a bad profile is usually two
    // bad photos, and deleting them is cheaper than re-enrolling a person.
    drogon::app().registerHandler(
        "/api/faces/{id}",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req);
                  const auto img = ctx.repos->face_image(t, id);
                  if (!img) {
                      cb(json_error(drogon::k404NotFound, "no such photo"));
                      return;
                  }
                  // Which employee it belonged to, so the profile can be rebuilt.
                  std::int64_t owner = 0;
                  for (const auto& e : ctx.repos->employees(t))
                      for (const auto& f : ctx.repos->face_images(t, e.id))
                          if (f.id == id) owner = e.id;

                  ctx.snapshots->remove(img->image_path);
                  ctx.repos->delete_face_image(t, id);
                  const auto prof = owner ? ctx.repos->recompute_profile(t, owner)
                                          : db::FaceProfile{};
                  ctx.engine->reload_gallery();

                  Json::Value v;
                  v["deleted"] = static_cast<Json::Int64>(id);
                  v["profile"] = profile_json(prof);
                  if (owner) v["warnings"] = warnings_for(ctx, t, owner, prof);
                  cb(json_ok(v));
              }),
        {drogon::Delete});

    // The image bytes. A face is the one asset in this system that must never
    // be served anonymously, so this requires a real token even though the
    // browser reaches it as an <img src> and can only put it in the query.
    drogon::app().registerHandler(
        "/api/faces/{id}/image",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const auto img = ctx.repos->face_image(t, path_id(req, 1));
                  if (!img || img->image_path.empty()) {
                      cb(json_error(drogon::k404NotFound, "no such image"));
                      return;
                  }
                  const std::string abs = ctx.snapshots->resolve(img->image_path);
                  std::ifstream f(abs, std::ios::binary);
                  if (abs.empty() || !f) {
                      cb(json_error(drogon::k404NotFound,
                                    "the image bytes are no longer in storage"));
                      return;
                  }
                  const std::string data((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
                  auto r = drogon::HttpResponse::newHttpResponse();
                  r->setContentTypeCode(drogon::CT_IMAGE_JPG);
                  r->setBody(data);
                  cb(r);
              }),
        {drogon::Get});
}

}  // namespace so::api
