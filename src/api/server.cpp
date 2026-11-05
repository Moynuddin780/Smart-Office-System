#include "api/server.hpp"

#include "core/host_cpu.hpp"

#include <cmath>

#include "core/log.hpp"

#include <drogon/HttpAppFramework.h>

#include <algorithm>
#include <thread>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace so::api {

HttpResponsePtr json_ok(const Json::Value& v) {
    auto r = drogon::HttpResponse::newHttpJsonResponse(v);
    r->setStatusCode(drogon::k200OK);
    return r;
}

HttpResponsePtr json_error(drogon::HttpStatusCode code, const std::string& message) {
    Json::Value v;
    v["error"] = message;
    auto r = drogon::HttpResponse::newHttpJsonResponse(v);
    r->setStatusCode(code);
    return r;
}

std::optional<Claims> authenticate(const Context& ctx, const HttpRequestPtr& req) {
    std::string token;
    const std::string h = req->getHeader("authorization");
    if (h.rfind("Bearer ", 0) == 0) {
        token = h.substr(7);
    } else {
        // An <img src> cannot send a header, so the MJPEG and snapshot routes
        // accept the token here instead. It is the same short-lived token; the
        // alternative (an unauthenticated image route) is how a snapshot of
        // somebody's office becomes readable by guessing a URL.
        token = req->getParameter("token");
    }
    if (token.empty()) return std::nullopt;
    return verify_token(token, ctx.cfg->jwt_secret);
}

bool may_access_tenant(const Claims& c, std::int64_t tenant_id) {
    // Deliberately NOT "a SuperAdmin may do anything". The vendor administers
    // tenants; they do not read inside them. Anything else makes "the vendor can
    // see every client's faces" a property of the system rather than a decision.
    if (c.role == Role::SuperAdmin) return false;
    return c.tenant_id != 0 && c.tenant_id == tenant_id;
}

std::function<void(const HttpRequestPtr&, Callback&&)>
guard(Context& ctx, std::initializer_list<Role> roles, TenantHandler h) {
    const std::vector<Role> allowed(roles);
    Context* c = &ctx;
    return [c, allowed, h](const HttpRequestPtr& req, Callback&& cb) {
        const auto claims = authenticate(*c, req);
        if (!claims) {
            cb(json_error(drogon::k401Unauthorized, "not authenticated"));
            return;
        }
        if (std::find(allowed.begin(), allowed.end(), claims->role) == allowed.end()) {
            cb(json_error(drogon::k403Forbidden, "insufficient permissions"));
            return;
        }
        if (claims->tenant_id == 0) {
            cb(json_error(drogon::k403Forbidden, "this route is scoped to a tenant"));
            return;
        }
        try {
            h(req, std::move(cb), *claims, db::TenantId{claims->tenant_id});
        } catch (const std::exception& e) {
            SO_LOG_ERROR("[api] %s failed: %s", req->getPath().c_str(), e.what());
            cb(json_error(drogon::k500InternalServerError, "internal error"));
        }
    };
}

std::function<void(const HttpRequestPtr&, Callback&&)>
guard_admin(Context& ctx, AdminHandler h) {
    Context* c = &ctx;
    return [c, h](const HttpRequestPtr& req, Callback&& cb) {
        const auto claims = authenticate(*c, req);
        if (!claims) {
            cb(json_error(drogon::k401Unauthorized, "not authenticated"));
            return;
        }
        if (claims->role != Role::SuperAdmin) {
            cb(json_error(drogon::k403Forbidden, "superadmin only"));
            return;
        }
        try {
            h(req, std::move(cb), *claims);
        } catch (const std::exception& e) {
            SO_LOG_ERROR("[api] %s failed: %s", req->getPath().c_str(), e.what());
            cb(json_error(drogon::k500InternalServerError, "internal error"));
        }
    };
}

std::optional<Json::Value> body_of(const HttpRequestPtr& req) {
    const auto j = req->getJsonObject();
    if (!j) return std::nullopt;
    return *j;
}

std::string query(const HttpRequestPtr& req, const std::string& key,
                  const std::string& dflt) {
    const std::string v = req->getParameter(key);
    return v.empty() ? dflt : v;
}

std::int64_t path_id(const HttpRequestPtr& req, std::size_t segment_from_end) {
    const std::string p = req->getPath();
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.size() <= segment_from_end) return 0;
    try {
        return std::stoll(parts[parts.size() - 1 - segment_from_end]);
    } catch (const std::exception&) {
        return 0;
    }
}

std::string today_local() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void run_server(Context& ctx) {
    register_auth(ctx);
    register_admin(ctx);
    register_config(ctx);
    register_people(ctx);
    register_reports(ctx);
    register_stream(ctx);

    // Liveness. Public on purpose: a health check that needs a credential is a
    // health check that stops working exactly when something is wrong.
    drogon::app().registerHandler(
        "/healthz",
        [&ctx](const HttpRequestPtr&, Callback&& cb) {
            Json::Value v;
            const bool db_ok = ctx.repos->raw().healthy();
            const auto s = ctx.engine->status();
            v["ok"] = db_ok && s.npu_ok;
            v["database"] = db_ok;
            v["npu"] = s.npu_ok;
            v["engines"] = s.engines;
            v["cameras_running"] = s.cameras_running;
            v["cameras_online"] = s.cameras_online;
            Json::Value cores(Json::arrayValue);
            for (double u : s.core_utilisation) cores.append(u);
            v["npu_core_utilisation"] = cores;
            v["gallery"] = s.gallery_count;

            // HOST CPU, because the NPU figure alone has been misleading.
            //
            // The NPU sits around a fifth of its capacity while cameras fall
            // behind, which reads as "plenty of headroom" and is the opposite
            // of the truth: the CPU is what cannot keep up, and it is what
            // decides how much work the NPU ever gets. Without this the only
            // way to see that was to open a shell on the board, which was done
            // by hand several times before it was worth building in.
            const auto cpu = host_cpu();
            Json::Value cpu_json;
            cpu_json["board"] = std::round(cpu.board * 10) / 10;
            cpu_json["little"] = std::round(cpu.little * 10) / 10;
            cpu_json["big"] = std::round(cpu.big * 10) / 10;
            // The busiest single core, which is the number that actually binds:
            // one thread pinned at 100% stalls a camera while the average still
            // looks comfortable.
            cpu_json["hottest"] = std::round(cpu.hottest * 10) / 10;
            Json::Value ccores(Json::arrayValue);
            for (const auto& c : cpu.cores) {
                Json::Value cc;
                cc["core"] = c.index;
                cc["busy"] = std::round(c.busy * 10) / 10;
                cc["mhz"] = c.max_mhz;
                cc["big"] = c.big;
                ccores.append(cc);
            }
            cpu_json["cores"] = ccores;
            v["cpu"] = cpu_json;
            auto r = drogon::HttpResponse::newHttpJsonResponse(v);
            r->setStatusCode(v["ok"].asBool() ? drogon::k200OK
                                              : drogon::k503ServiceUnavailable);
            cb(r);
        },
        {drogon::Get});

    drogon::app()
        .setLogLevel(trantor::Logger::kWarn)
        .addListener("0.0.0.0", ctx.cfg->http_port)
        // One loop per core, floor of 8. Live tiles, snapshots and long polls
        // all occupy a loop for as long as they run, so four was enough for a
        // dashboard with four tiles to leave nothing for the API.
        .setThreadNum(std::max(8u, std::thread::hardware_concurrency()))
        .setDocumentRoot(ctx.cfg->frontend_dir)
        .setClientMaxBodySize(32 * 1024 * 1024)
        // Any unmatched path falls through to the SPA's index, so a browser
        // reload on /attendance does not 404.
        .setCustom404Page(drogon::HttpResponse::newFileResponse(
            ctx.cfg->frontend_dir + "/index.html", "", drogon::CT_TEXT_HTML));

    SO_LOG_INFO("[api] listening on 0.0.0.0:%d", static_cast<int>(ctx.cfg->http_port));
    drogon::app().run();
}

}  // namespace so::api
