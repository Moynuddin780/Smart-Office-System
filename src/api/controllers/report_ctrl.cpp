#include "api/server.hpp"

#include <drogon/HttpAppFramework.h>

#include <sstream>

namespace so::api {
namespace {

std::string hms(int seconds) {
    if (seconds <= 0) return "00:00";
    std::ostringstream s;
    s << (seconds / 3600) << ':';
    const int m = (seconds % 3600) / 60;
    if (m < 10) s << '0';
    s << m;
    return s.str();
}

Json::Value day_json(const db::DayRow& d) {
    Json::Value v;
    v["employee_id"] = static_cast<Json::Int64>(d.employee_id);
    v["emp_code"] = d.emp_code;
    v["name"] = d.name;
    v["department"] = d.department;
    v["first_in"] = d.first_in;
    v["last_out"] = d.last_out;
    v["status"] = d.status;
    v["gross_seconds"] = d.gross_seconds;
    v["inside_seconds"] = d.inside_seconds;
    v["break_seconds"] = d.break_seconds;
    v["working_time"] = hms(d.inside_seconds);
    v["break_time"] = hms(d.break_seconds);
    v["entries"] = d.entries;
    // Surfaced, not hidden: this day's departure time was inferred because the
    // person never registered an exit. A timesheet has to be able to say so.
    v["assumed_out"] = d.assumed_out;
    return v;
}

std::string csv_escape(const std::string& in) {
    if (in.find_first_of(",\"\n") == std::string::npos) return in;
    std::string out = "\"";
    for (char c : in) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    return out + "\"";
}

}  // namespace

void register_reports(Context& ctx) {
    drogon::app().registerHandler(
        "/api/attendance/daily",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::string date = query(req, "date", today_local());
                  Json::Value rows(Json::arrayValue);
                  int present = 0, late = 0, absent = 0;
                  for (const auto& d : ctx.repos->attendance_day(t, date)) {
                      rows.append(day_json(d));
                      if (d.status == "absent") ++absent;
                      else if (d.status == "late") ++late;
                      else ++present;
                  }
                  Json::Value v;
                  v["date"] = date;
                  v["rows"] = rows;
                  Json::Value sum;
                  sum["present"] = present;
                  sum["late"] = late;
                  sum["absent"] = absent;
                  sum["total"] = present + late + absent;
                  v["summary"] = sum;
                  cb(json_ok(v));
              }),
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/attendance/employee/{id}",
        guard(ctx, {Role::Admin, Role::User},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims& c,
                     db::TenantId t) {
                  const std::int64_t id = path_id(req);
                  // A plain user may read their OWN record and nobody else's.
                  // Without this the route would be an employee directory with a
                  // full movement history attached.
                  if (c.role == Role::User && c.employee_id != id) {
                      cb(json_error(drogon::k403Forbidden,
                                    "you may only read your own attendance"));
                      return;
                  }
                  const std::string from = query(req, "from", today_local());
                  const std::string to = query(req, "to", today_local());
                  Json::Value rows(Json::arrayValue);
                  for (const auto& d : ctx.repos->attendance_range(t, id, from, to))
                      rows.append(day_json(d));
                  Json::Value v;
                  v["employee_id"] = static_cast<Json::Int64>(id);
                  v["from"] = from;
                  v["to"] = to;
                  v["rows"] = rows;
                  cb(json_ok(v));
              }),
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/me/attendance",
        guard(ctx, {Role::User, Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims& c,
                     db::TenantId t) {
                  if (c.employee_id == 0) {
                      cb(json_error(drogon::k404NotFound,
                                    "this account is not linked to an employee record"));
                      return;
                  }
                  const std::string from = query(req, "from", today_local());
                  const std::string to = query(req, "to", today_local());
                  Json::Value rows(Json::arrayValue);
                  for (const auto& d : ctx.repos->attendance_range(t, c.employee_id, from, to))
                      rows.append(day_json(d));
                  Json::Value v;
                  v["rows"] = rows;
                  cb(json_ok(v));
              }),
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/reports/attendance.csv",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::string date = query(req, "date", today_local());
                  std::ostringstream out;
                  out << "date,employee_code,name,department,first_in,last_out,status,"
                         "working_time,break_time,entries,departure_inferred\r\n";
                  for (const auto& d : ctx.repos->attendance_day(t, date)) {
                      out << date << ',' << csv_escape(d.emp_code) << ','
                          << csv_escape(d.name) << ',' << csv_escape(d.department) << ','
                          << d.first_in << ',' << d.last_out << ',' << d.status << ','
                          << hms(d.inside_seconds) << ',' << hms(d.break_seconds) << ','
                          << d.entries << ',' << (d.assumed_out ? "yes" : "no") << "\r\n";
                  }
                  auto r = drogon::HttpResponse::newHttpResponse();
                  r->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                  r->addHeader("Content-Type", "text/csv; charset=utf-8");
                  r->addHeader("Content-Disposition",
                               "attachment; filename=\"attendance-" + date + ".csv\"");
                  r->setBody(out.str());
                  cb(r);
              }),
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/desk/daily",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::string date = query(req, "date", today_local());
                  Json::Value rows(Json::arrayValue);
                  for (const auto& d : ctx.repos->desk_day(t, date)) {
                      Json::Value v;
                      v["zone_id"] = static_cast<Json::Int64>(d.zone_id);
                      v["zone_name"] = d.zone_name;
                      v["employee_id"] = static_cast<Json::Int64>(d.employee_id);
                      v["employee_name"] = d.employee_name;
                      v["occupied_seconds"] = d.occupied_seconds;
                      v["absent_seconds"] = d.absent_seconds;
                      v["longest_absence_seconds"] = d.longest_absence_seconds;
                      v["sessions"] = d.sessions_count;
                      v["at_desk"] = hms(d.occupied_seconds);
                      v["away"] = hms(d.absent_seconds);
                      // Seconds during which somebody OTHER than the assigned
                      // employee was recognised at this desk. The count above is
                      // unaffected; this exists so a report does not credit the
                      // wrong person in silence.
                      v["identity_mismatch_seconds"] = d.mismatch_seconds;
                      // Whether the assigned employee was recorded arriving at
                      // all today. Somebody sat at this desk either way — the
                      // seconds are real — but crediting them to a person who
                      // never came in would be a quiet lie, so the reader is
                      // told and can decide.
                      v["employee_present"] = d.employee_present;
                      rows.append(v);
                  }
                  Json::Value v;
                  v["date"] = date;
                  v["rows"] = rows;
                  cb(json_ok(v));
              }),
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/events",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  const std::string module = query(req, "module");
                  const std::string from = query(req, "from");
                  const std::string to = query(req, "to");
                  int limit = 200;
                  try {
                      limit = std::stoi(query(req, "limit", "200"));
                  } catch (const std::exception&) {
                  }
                  Json::Value rows(Json::arrayValue);
                  for (const auto& e : ctx.repos->events(t, module, from, to, limit)) {
                      Json::Value v;
                      v["id"] = static_cast<Json::Int64>(e.id);
                      v["module"] = e.module;
                      v["severity"] = e.severity;
                      v["title"] = e.title;
                      v["detail"] = e.detail;
                      v["ts"] = e.ts;
                      v["camera_id"] = static_cast<Json::Int64>(e.camera_id);
                      v["camera_name"] = e.camera_name;
                      v["camera_site"] = e.camera_site;
                      v["camera_location"] = e.camera_location;
                      v["zone_id"] = static_cast<Json::Int64>(e.zone_id);
                      v["employee_id"] = static_cast<Json::Int64>(e.employee_id);
                      v["acknowledged"] = e.acknowledged;
                      // Paths, not URLs, and the image route needs a token.
                      v["snapshot"] = e.snapshot_path;
                      v["crop"] = e.crop_path;
                      rows.append(v);
                  }
                  cb(json_ok(rows));
              }),
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/events/{id}/ack",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims& c,
                     db::TenantId t) {
                  ctx.repos->acknowledge_event(t, path_id(req, 1), c.user_id);
                  Json::Value v;
                  v["acknowledged"] = true;
                  cb(json_ok(v));
              }),
        {drogon::Post});


    // ── exits nobody could name ────────────────────────────────────────────
    //
    // A queue of questions, not of alerts. Each row is a person the system
    // watched walk out and could not put a name to, with the face it saw and
    // the identity that face most resembled. Nothing here has touched
    // attendance, and nothing will until somebody says so.
    drogon::app().registerHandler(
        "/api/exit-reviews",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims&,
                     db::TenantId t) {
                  Json::Value rows(Json::arrayValue);
                  for (const auto& x : ctx.repos->open_exit_reviews(
                           t, req->getParameter("date"))) {
                      Json::Value v;
                      v["id"] = static_cast<Json::Int64>(x.id);
                      v["camera_id"] = static_cast<Json::Int64>(x.camera_id);
                      v["camera"] = x.camera_name;
                      v["time"] = x.ts;
                      v["work_date"] = x.work_date;
                      v["guess_employee_id"] =
                          static_cast<Json::Int64>(x.guess_employee_id);
                      v["guess_name"] = x.guess_name;
                      v["guess_code"] = x.guess_code;
                      // Rounded for display, but kept honest: a reviewer should
                      // see HOW thin the evidence was, not just who it favoured.
                      v["score"] = std::round(x.guess_score * 100.0) / 100.0;
                      v["margin"] = std::round(x.guess_margin * 100.0) / 100.0;
                      v["weight"] = std::round(x.guess_weight * 100.0) / 100.0;
                      v["crop"] = x.crop_path;
                      v["frame"] = x.frame_path;
                      rows.append(v);
                  }
                  Json::Value out;
                  out["rows"] = rows;
                  cb(json_ok(out));
              }),
        {drogon::Get});

    // Answer one: name the person, or dismiss it as a false exit.
    //
    // Naming writes the `out` that was never written, timed to when they were
    // last seen rather than to now — the departure happened when it happened,
    // and dating it to the moment somebody got round to confirming it would put
    // the break in the wrong place.
    drogon::app().registerHandler(
        "/api/exit-reviews/{id}/resolve",
        guard(ctx, {Role::Admin},
              [&ctx](const HttpRequestPtr& req, Callback&& cb, const Claims& c,
                     db::TenantId t) {
                  // SEGMENT 1, NOT 0. path_id counts from the END, so on
                  // /api/exit-reviews/{id}/resolve segment 0 is the word
                  // "resolve" — which parses as 0, matches no row, and made
                  // every confirmation answer "that exit has already been
                  // answered".
                  const std::int64_t id = path_id(req, 1);
                  const auto body = req->getJsonObject();
                  const std::int64_t employee_id =
                      body && body->isMember("employee_id")
                          ? (*body)["employee_id"].asInt64() : 0;

                  const auto row = ctx.repos->resolve_exit_review(t, id, employee_id,
                                                                  c.user_id);
                  if (!row) {
                      cb(json_error(drogon::k404NotFound,
                                    "that exit has already been answered"));
                      return;
                  }
                  Json::Value v;
                  v["dismissed"] = employee_id == 0;
                  if (employee_id != 0) {
                      ctx.repos->record_attendance(t, employee_id, row->camera_id, "out",
                                                   row->ts, row->work_date, row->guess_score,
                                                   -1, "");
                      ctx.repos->recompute_day(t, employee_id, row->work_date);
                      v["employee_id"] = static_cast<Json::Int64>(employee_id);
                      v["work_date"] = row->work_date;
                  }
                  cb(json_ok(v));
              }),
        {drogon::Post});
}
}  // namespace so::api
