#include "api/server.hpp"

#include "core/log.hpp"

#include <drogon/HttpAppFramework.h>

namespace so::api {

void register_auth(Context& ctx) {
    // Public. The one route that has to be.
    drogon::app().registerHandler(
        "/api/auth/login",
        [&ctx](const HttpRequestPtr& req, Callback&& cb) {
            const auto body = body_of(req);
            if (!body) {
                cb(json_error(drogon::k400BadRequest, "a JSON body is required"));
                return;
            }
            const std::string email = (*body)["email"].asString();
            const std::string password = (*body)["password"].asString();

            const auto user = ctx.repos->find_user_by_email(email);
            // One message for both "no such account" and "wrong password".
            // Distinguishing them turns the login form into a way to find out
            // who works here.
            const bool ok = user && user->active &&
                            verify_password(password, user->password_hash);
            if (!ok) {
                SO_LOG_WARN("[auth] failed login for %s", email.c_str());
                cb(json_error(drogon::k401Unauthorized, "invalid email or password"));
                return;
            }

            // Transparent upgrade: if this hash was made at a lower cost than we
            // issue today, re-hash now that we have the plaintext in hand.
            if (needs_rehash(user->password_hash))
                ctx.repos->update_password(user->id, hash_password(password));

            const auto role = role_from_string(user->role);
            if (!role) {
                cb(json_error(drogon::k500InternalServerError, "account role is invalid"));
                return;
            }

            Claims c;
            c.user_id = user->id;
            c.tenant_id = user->tenant_id;
            c.role = *role;
            c.employee_id = user->employee_id;

            Json::Value v;
            v["token"] = issue_token(c, ctx.cfg->jwt_secret, 3600);
            v["expires_in"] = 3600;
            Json::Value u;
            u["id"] = static_cast<Json::Int64>(user->id);
            u["email"] = user->email;
            u["name"] = user->name;
            u["role"] = user->role;
            u["tenant_id"] = static_cast<Json::Int64>(user->tenant_id);
            v["user"] = u;
            cb(json_ok(v));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/auth/me",
        [&ctx](const HttpRequestPtr& req, Callback&& cb) {
            const auto claims = authenticate(ctx, req);
            if (!claims) {
                cb(json_error(drogon::k401Unauthorized, "not authenticated"));
                return;
            }
            const auto user = ctx.repos->find_user_by_id(claims->user_id);
            if (!user) {
                // The token is valid but the account is gone. Rejecting rather
                // than trusting the token's copy of the claims is what makes
                // deleting a user take effect immediately.
                cb(json_error(drogon::k401Unauthorized, "account no longer exists"));
                return;
            }
            Json::Value v;
            v["id"] = static_cast<Json::Int64>(user->id);
            v["email"] = user->email;
            v["name"] = user->name;
            v["role"] = user->role;
            v["tenant_id"] = static_cast<Json::Int64>(user->tenant_id);
            v["employee_id"] = static_cast<Json::Int64>(user->employee_id);
            cb(json_ok(v));
        },
        {drogon::Get});
}

}  // namespace so::api
