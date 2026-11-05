#pragma once

#include <libpq-fe.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace so::db {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

/// One result set. Values come back as strings; the typed accessors do the
/// conversion and tell you when a column is NULL rather than returning 0.
class Result {
public:
    explicit Result(PGresult* r);
    ~Result();
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result(Result&& o) noexcept;

    int rows() const;
    int cols() const;
    bool is_null(int row, int col) const;

    std::string text(int row, int col) const;
    std::int64_t integer(int row, int col, std::int64_t dflt = 0) const;
    double number(int row, int col, double dflt = 0) const;
    bool boolean(int row, int col, bool dflt = false) const;
    /// bytea, already un-escaped.
    std::vector<unsigned char> bytes(int row, int col) const;

    int column(const std::string& name) const;

private:
    PGresult* r_{nullptr};
};

/**
 * A tiny fixed-size connection pool over libpq.
 *
 * Every query is parameterised — there is no interface here that takes a
 * composed SQL string with values in it, because the moment one exists somebody
 * will build a WHERE clause out of a query parameter.
 */
class Database {
public:
    Database(std::string url, int pool_size = 8);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// Parameterised query. Every value is bound, never interpolated.
    Result query(const std::string& sql, const std::vector<std::string>& params = {});

    /// Same, but a null optional binds SQL NULL.
    Result query_opt(const std::string& sql,
                     const std::vector<std::optional<std::string>>& params);

    /**
     * Run a MULTI-STATEMENT script through the simple query protocol.
     *
     * `query()` uses PQexecParams, which is the right default — every value is
     * bound rather than interpolated — but the extended protocol accepts only
     * ONE statement per call, so a migration file sent through it fails with
     * "cannot insert multiple commands into a prepared statement".
     *
     * This takes no parameters and never will: the only thing that reaches it
     * is a .sql file shipped with the application. Anything carrying a value
     * from outside goes through query() and gets bound.
     */
    void exec_script(const std::string& sql);

    /// Run `fn` inside a transaction, rolling back if it throws.
    template <typename Fn>
    auto transaction(Fn&& fn) -> decltype(fn()) {
        query("BEGIN");
        try {
            if constexpr (std::is_void_v<decltype(fn())>) {
                fn();
                query("COMMIT");
            } else {
                auto out = fn();
                query("COMMIT");
                return out;
            }
        } catch (...) {
            try { query("ROLLBACK"); } catch (...) {}
            throw;
        }
    }

    /// Apply every .sql file in `dir` that is not already recorded, in name
    /// order, each in its own transaction.
    int migrate(const std::string& dir);

    bool healthy();

private:
    struct Conn {
        PGconn* pg{nullptr};
    };
    Conn* acquire();
    void release(Conn* c);
    void reconnect(Conn* c);

    std::string url_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::vector<Conn*> free_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

/// Escape a value for a bytea parameter.
std::string to_bytea(const void* data, std::size_t len);

}  // namespace so::db
