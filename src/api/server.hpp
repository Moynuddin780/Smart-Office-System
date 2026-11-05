#pragma once

#include "api/auth.hpp"
#include "app/engine.hpp"
#include "core/config.hpp"
#include "db/repos.hpp"
#include "notify/snapshot.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <functional>
#include <optional>
#include <string>

namespace so::api {

using drogon::HttpRequestPtr;
using drogon::HttpResponsePtr;
using Callback = std::function<void(const HttpResponsePtr&)>;

/// Everything a handler needs. Passed by reference; lives for the process.
struct Context {
    Config* cfg{nullptr};
    db::Repos* repos{nullptr};
    app::Engine* engine{nullptr};
    notify::SnapshotStore* snapshots{nullptr};
};

// ── responses ──────────────────────────────────────────────────────────────

HttpResponsePtr json_ok(const Json::Value& v);
HttpResponsePtr json_error(drogon::HttpStatusCode code, const std::string& message);

// ── authentication ─────────────────────────────────────────────────────────

/**
 * The caller's claims, or nothing.
 *
 * There is no fallback branch. The sibling project's shared dependency returned
 * a synthetic admin whenever no token was present, so every route in that API
 * answered an anonymous caller with real data while looking protected. A route
 * here that forgets to call this returns nothing at all rather than data.
 *
 * The token is read from the Authorization header, or - for endpoints a browser
 * loads as an <img> or <video> source, which cannot carry headers - from a
 * `token` query parameter.
 */
std::optional<Claims> authenticate(const Context& ctx, const HttpRequestPtr& req);

/// True when the caller may act inside `tenant_id`. A SuperAdmin may not: they
/// administer tenants, they do not read inside them, and making that explicit
/// keeps "the vendor can see every client's faces" from being an accident.
bool may_access_tenant(const Claims& c, std::int64_t tenant_id);

/**
 * Wrap a handler so it only runs for an authenticated caller of an allowed role,
 * and hand it the tenant it is scoped to.
 *
 * The tenant comes from the TOKEN. No handler takes it from a body or a query
 * string, which is the only way to be sure none of them can be talked into
 * reading another client's data.
 */
using TenantHandler =
    std::function<void(const HttpRequestPtr&, Callback&&, const Claims&, db::TenantId)>;

std::function<void(const HttpRequestPtr&, Callback&&)>
guard(Context& ctx, std::initializer_list<Role> roles, TenantHandler h);

/// Same, for routes that are not tenant-scoped (SuperAdmin only).
using AdminHandler = std::function<void(const HttpRequestPtr&, Callback&&, const Claims&)>;

std::function<void(const HttpRequestPtr&, Callback&&)>
guard_admin(Context& ctx, AdminHandler h);

// ── helpers shared by the controllers ──────────────────────────────────────

/// Parsed JSON body, or nothing when it is absent or malformed.
std::optional<Json::Value> body_of(const HttpRequestPtr& req);
std::string query(const HttpRequestPtr& req, const std::string& key,
                  const std::string& dflt = "");
std::int64_t path_id(const HttpRequestPtr& req, std::size_t segment_from_end = 0);
std::string today_local();

// ── registration ───────────────────────────────────────────────────────────

void register_auth(Context& ctx);
void register_admin(Context& ctx);
void register_config(Context& ctx);
void register_people(Context& ctx);
void register_reports(Context& ctx);
void register_stream(Context& ctx);

/// Register everything and start Drogon. Blocks.
void run_server(Context& ctx);

}  // namespace so::api
