#include <cmath>
#include "api/server.hpp"

#include "identity/face_identifier.hpp"

#include <drogon/HttpAppFramework.h>

#include <algorithm>
#include <cctype>

namespace so::api {
namespace {

/// A URL-safe slug. Not cosmetic: it names the tenant's snapshot directory on
/// disk, so anything that could contain a path separator is stripped.
std::string slugify(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else if (!out.empty() && out.back() != '-')
            out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "tenant" : out.substr(0, 48);
}

Json::Value tenant_json(const db::Tenant& t) {
    Json::Value v;
    v["id"] = static_cast<Json::Int64>(t.id);
    v["name"] = t.name;
    v["slug"] = t.slug;
    v["status"] = t.status;
    v["plan"] = t.plan;
    v["max_cameras"] = t.max_cameras;
    v["max_employees"] = t.max_employees;
    v["created_at"] = t.created_at;
    v["admin_email"] = t.admin_email;
    v["cameras"] = t.cameras;
    v["employees"] = t.employees;
    return v;
}

}  // namespace

void register_admin(Context& ctx) {
    drogon::app().registerHandler(
        "/api/admin/tenants",
        guard_admin(ctx, [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&) {
            if (req->method() == drogon::Get) {
                Json::Value out(Json::arrayValue);
                for (const auto& t : ctx.repos->all_tenants()) out.append(tenant_json(t));
                cb(json_ok(out));
                return;
            }

            const auto body = body_of(req);
            if (!body || (*body)["name"].asString().empty()) {
                cb(json_error(drogon::k400BadRequest, "name is required"));
                return;
            }
            const std::string name = (*body)["name"].asString();
            const std::string slug = (*body)["slug"].asString().empty()
                                         ? slugify(name)
                                         : slugify((*body)["slug"].asString());

            const auto t = ctx.repos->create_tenant(name, slug);

            // A company with no way in is not provisioned. The first admin is
            // created with the tenant, in the same request.
            const std::string admin_email = (*body)["admin_email"].asString();
            const std::string admin_password = (*body)["admin_password"].asString();
            Json::Value v = tenant_json(t);
            if (!admin_email.empty() && admin_password.size() >= 8) {
                ctx.repos->create_user(t.id, admin_email, hash_password(admin_password),
                                       (*body)["admin_name"].asString(), "admin");
                v["admin_created"] = admin_email;
            } else if (!admin_email.empty()) {
                v["admin_created"] = false;
                v["admin_error"] = "the admin password must be at least 8 characters";
            }
            ctx.engine->reload();
            cb(json_ok(v));
        }),
        {drogon::Get, drogon::Post});

    drogon::app().registerHandler(
        "/api/admin/tenants/{id}",
        guard_admin(ctx, [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&) {
            const std::int64_t id = path_id(req);
            if (id == 0) {
                cb(json_error(drogon::k400BadRequest, "a tenant id is required"));
                return;
            }
            if (req->method() == drogon::Delete) {
                ctx.repos->delete_tenant(id);
                ctx.engine->reload();
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
            const std::string status = (*body)["status"].asString();
            if (status != "active" && status != "suspended") {
                cb(json_error(drogon::k400BadRequest,
                              "status must be 'active' or 'suspended'"));
                return;
            }
            ctx.repos->set_tenant_status(id, status);
            // Suspending a tenant stops their cameras immediately rather than at
            // the next restart — that is what makes it a usable lever.
            ctx.engine->reload();
            Json::Value v;
            v["id"] = static_cast<Json::Int64>(id);
            v["status"] = status;
            cb(json_ok(v));
        }),
        {drogon::Patch, drogon::Delete});

    // Rebuild every face profile from the images behind it.
    //
    // A profile is a weighted mean of a person's image vectors, and it is
    // normally recomputed by the one endpoint that changes those images. Vectors
    // can also arrive from OUTSIDE that path — a bulk import from another
    // system — and then the images are right while the profile still describes
    // whoever was there before. The recogniser reads the profile, so the import
    // would sit in the database doing nothing, with no error to explain it.
    drogon::app().registerHandler(
        "/api/admin/rebuild-profiles",
        guard_admin(ctx, [&ctx](const HttpRequestPtr&, Callback&& cb, const Claims&) {
            Json::Value rows(Json::arrayValue);
            int done = 0;
            for (const auto& t : ctx.repos->all_tenants()) {
                const db::TenantId tid{t.id};
                for (const auto& e : ctx.repos->employees(tid)) {
                    const auto prof = ctx.repos->recompute_profile(tid, e.id);
                    if (prof.image_count == 0) continue;
                    Json::Value r;
                    r["employee_id"] = static_cast<Json::Int64>(e.id);
                    r["name"] = e.name;
                    r["images"] = prof.image_count;
                    r["coherence"] = prof.coherence >= 0
                        ? Json::Value(std::round(prof.coherence * 1000.0) / 1000.0)
                        : Json::Value(Json::nullValue);
                    rows.append(r);
                    ++done;
                }
            }
            ctx.engine->reload_gallery();
            Json::Value v;
            v["rebuilt"] = done;
            v["profiles"] = rows;
            cb(json_ok(v));
        }),
        {drogon::Post});

    // Re-read stored days under the CURRENT rules.
    //
    // A day's totals are computed when an event arrives and then stored, so a
    // change to how days are counted — the minimum gap that is a break, where
    // an unfinished day is closed — reaches future events only. Every day
    // already on the books keeps the arithmetic of the rule it was written
    // under, which is how two employees end up with the same punches and
    // different totals.
    //
    // Nothing here invents or deletes an event: the punches are the record, and
    // this only asks them the question again.
    drogon::app().registerHandler(
        "/api/admin/recompute-days",
        guard_admin(ctx, [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&) {
            // Defaults to today, because that is the day somebody is looking at
            // when they notice a figure is wrong.
            const std::string from = req->getParameter("from");
            const std::string to = req->getParameter("to");
            Json::Value v;
            int days = 0, rows = 0;
            for (const auto& t : ctx.repos->all_tenants()) {
                if (t.status != "active") continue;
                const db::TenantId tid{t.id};
                for (const auto& d : ctx.repos->work_dates(tid, from, to)) {
                    ++days;
                    for (const auto& id : ctx.repos->employees_with_events(tid, d)) {
                        ctx.repos->recompute_day(tid, id, d);
                        ++rows;
                    }
                }
            }
            v["days"] = days;
            v["rows"] = rows;
            cb(json_ok(v));
        }),
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/admin/status",
        guard_admin(ctx, [&ctx](const HttpRequestPtr&, Callback&& cb, const Claims&) {
            const auto s = ctx.engine->status();
            Json::Value v;
            v["engines"] = s.engines;
            v["cameras_running"] = s.cameras_running;
            v["cameras_online"] = s.cameras_online;
            v["gallery"] = s.gallery_count;
            Json::Value cores(Json::arrayValue);
            for (double u : s.core_utilisation) cores.append(u);
            v["npu_core_utilisation"] = cores;
            Json::Value ws(Json::arrayValue);
            for (const auto& w : ctx.engine->worker_stats()) {
                Json::Value x;
                x["camera_id"] = static_cast<Json::Int64>(w.camera_id);
                x["camera"] = w.camera_name;
                x["online"] = w.online;
                x["decode"] = w.decode_path;
                x["frames_read"] = static_cast<Json::Int64>(w.frames_read);
                x["inferred"] = static_cast<Json::Int64>(w.inferred);
                x["detect_hz"] = w.detect_hz;
                x["faces_seen"] = static_cast<Json::Int64>(w.faces_seen);
                x["faces_bound"] = static_cast<Json::Int64>(w.faces_bound);
                x["faces_judged"] = static_cast<Json::Int64>(w.faces_judged);
                x["tracks_seen"] = static_cast<Json::Int64>(w.tracks_seen);
                x["face_px_mean"] = w.face_px_mean;
                x["face_px_max"] = w.face_px_max;
                // The gate the mean has to clear. Sent with the number so the
                // UI does not have to know the threshold, and cannot drift.
                //
                // TAKEN FROM THE GATE ITSELF, not typed again here. It was
                // hardcoded to 60, and when the real gate was lowered to 24
                // this went on telling operators their camera needed two and a
                // half times the face it actually needs — on the one screen
                // somebody reads while deciding whether to move a camera.
                x["face_px_required"] = IdentifierConfig{}.min_face_px;
                x["identified"] = static_cast<Json::Int64>(w.identified);
                ws.append(x);
            }
            v["workers"] = ws;
            cb(json_ok(v));
        }),
        {drogon::Get});
}

}  // namespace so::api
