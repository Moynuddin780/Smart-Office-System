#include "db/database.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace so::db {
namespace {

std::string pg_error(PGconn* c) {
    const char* m = PQerrorMessage(c);
    return m ? m : "unknown libpq error";
}

}  // namespace

Result::Result(PGresult* r) : r_(r) {}

Result::~Result() {
    if (r_) PQclear(r_);
}

Result::Result(Result&& o) noexcept : r_(o.r_) { o.r_ = nullptr; }

int Result::rows() const { return r_ ? PQntuples(r_) : 0; }
int Result::cols() const { return r_ ? PQnfields(r_) : 0; }

bool Result::is_null(int row, int col) const {
    return !r_ || PQgetisnull(r_, row, col) == 1;
}

std::string Result::text(int row, int col) const {
    if (is_null(row, col)) return "";
    const char* v = PQgetvalue(r_, row, col);
    return v ? std::string(v) : std::string();
}

std::int64_t Result::integer(int row, int col, std::int64_t dflt) const {
    if (is_null(row, col)) return dflt;
    try {
        return std::stoll(text(row, col));
    } catch (const std::exception&) {
        return dflt;
    }
}

double Result::number(int row, int col, double dflt) const {
    if (is_null(row, col)) return dflt;
    try {
        return std::stod(text(row, col));
    } catch (const std::exception&) {
        return dflt;
    }
}

bool Result::boolean(int row, int col, bool dflt) const {
    if (is_null(row, col)) return dflt;
    const std::string v = text(row, col);
    return v == "t" || v == "true" || v == "1";
}

std::vector<unsigned char> Result::bytes(int row, int col) const {
    std::vector<unsigned char> out;
    if (is_null(row, col)) return out;
    std::size_t len = 0;
    unsigned char* raw = PQunescapeBytea(
        reinterpret_cast<const unsigned char*>(PQgetvalue(r_, row, col)), &len);
    if (!raw) return out;
    out.assign(raw, raw + len);
    PQfreemem(raw);
    return out;
}

int Result::column(const std::string& name) const {
    return r_ ? PQfnumber(r_, name.c_str()) : -1;
}

Database::Database(std::string url, int pool_size) : url_(std::move(url)) {
    const int n = std::max(1, pool_size);
    for (int i = 0; i < n; ++i) {
        auto c = std::make_unique<Conn>();
        c->pg = PQconnectdb(url_.c_str());
        if (PQstatus(c->pg) != CONNECTION_OK)
            throw Error("database connection failed: " + pg_error(c->pg));
        free_.push_back(c.get());
        conns_.push_back(std::move(c));
    }
    SO_LOG_INFO("[db] connected, pool of %d", n);
}

Database::~Database() {
    for (auto& c : conns_)
        if (c->pg) PQfinish(c->pg);
}

Database::Conn* Database::acquire() {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [&] { return !free_.empty(); });
    Conn* c = free_.back();
    free_.pop_back();
    return c;
}

void Database::release(Conn* c) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        free_.push_back(c);
    }
    cv_.notify_one();
}

void Database::reconnect(Conn* c) {
    PQreset(c->pg);
    if (PQstatus(c->pg) != CONNECTION_OK)
        throw Error("database reconnect failed: " + pg_error(c->pg));
    SO_LOG_WARN("[db] connection reset");
}

Result Database::query(const std::string& sql, const std::vector<std::string>& params) {
    std::vector<std::optional<std::string>> opt;
    opt.reserve(params.size());
    for (const auto& p : params) opt.emplace_back(p);
    return query_opt(sql, opt);
}

Result Database::query_opt(const std::string& sql,
                           const std::vector<std::optional<std::string>>& params) {
    Conn* c = acquire();
    struct Release {
        Database* db;
        Conn* c;
        ~Release() { db->release(c); }
    } guard{this, c};

    if (PQstatus(c->pg) != CONNECTION_OK) reconnect(c);

    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) values.push_back(p ? p->c_str() : nullptr);

    PGresult* r = PQexecParams(c->pg, sql.c_str(), static_cast<int>(params.size()),
                               nullptr, values.data(), nullptr, nullptr, 0);
    const ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        const std::string msg = PQresultErrorMessage(r);
        PQclear(r);
        // The statement text is included, the parameter VALUES deliberately are
        // not: they are the one part of a query that routinely holds a password
        // or a face embedding.
        throw Error("query failed: " + msg + " [sql: " + sql.substr(0, 200) + "]");
    }
    return Result(r);
}

void Database::exec_script(const std::string& sql) {
    Conn* c = acquire();
    struct Release {
        Database* db;
        Conn* c;
        ~Release() { db->release(c); }
    } guard{this, c};

    if (PQstatus(c->pg) != CONNECTION_OK) reconnect(c);

    PGresult* r = PQexec(c->pg, sql.c_str());
    const ExecStatusType st = PQresultStatus(r);
    const std::string msg = PQresultErrorMessage(r);
    PQclear(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
        throw Error("script failed: " + msg);
}

int Database::migrate(const std::string& dir) {
    query(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        " version TEXT PRIMARY KEY,"
        " applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");

    std::vector<std::string> files;
    if (!std::filesystem::exists(dir))
        throw Error("migrations directory not found: " + dir);
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".sql")
            files.push_back(e.path().filename().string());
    std::sort(files.begin(), files.end());

    int applied = 0;
    for (const auto& f : files) {
        const auto seen = query("SELECT 1 FROM schema_migrations WHERE version = $1", {f});
        if (seen.rows() > 0) continue;

        std::ifstream in(std::filesystem::path(dir) / f);
        const std::string sql((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        if (sql.empty()) continue;

        // One transaction per file, so a failure leaves the database at the
        // last good version rather than half-migrated. The script itself goes
        // through the simple protocol (see exec_script); only the bookkeeping
        // row is parameterised.
        transaction([&] {
            exec_script(sql);
            query("INSERT INTO schema_migrations(version) VALUES ($1)", {f});
        });
        SO_LOG_INFO("[db] applied migration %s", f.c_str());
        ++applied;
    }
    if (applied == 0) SO_LOG_INFO("[db] schema is up to date");
    return applied;
}

bool Database::healthy() {
    try {
        return query("SELECT 1").rows() == 1;
    } catch (const std::exception& e) {
        SO_LOG_WARN("[db] health check failed: %s", e.what());
        return false;
    }
}

std::string to_bytea(const void* data, std::size_t len) {
    // The \x hex form: unambiguous and independent of standard_conforming_strings.
    static const char* hex = "0123456789abcdef";
    const auto* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(len * 2 + 2);
    out += "\\x";
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(hex[p[i] >> 4]);
        out.push_back(hex[p[i] & 0x0F]);
    }
    return out;
}

}  // namespace so::db
