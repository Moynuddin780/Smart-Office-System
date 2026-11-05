#include "core/log.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace so {
namespace {

LogLevel parse_level(const char* s) {
    if (!s) return LogLevel::Info;
    if (std::strcmp(s, "debug") == 0) return LogLevel::Debug;
    if (std::strcmp(s, "warn") == 0) return LogLevel::Warn;
    if (std::strcmp(s, "error") == 0) return LogLevel::Error;
    return LogLevel::Info;
}

LogLevel g_level = parse_level(std::getenv("LOG_LEVEL"));
std::mutex g_mutex;

const char* tag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// Only the basename is useful in a log line; the full build path is noise.
const char* short_file(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

}  // namespace

LogLevel log_level() { return g_level; }
void set_log_level(LogLevel l) { g_level = l; }

void log_write(LogLevel l, const char* file, int line, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    std::tm tm{};
    localtime_r(&ts.tv_sec, &tm);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);

    // One locked fprintf so concurrent camera threads cannot interleave
    // halfway through a line.
    std::lock_guard<std::mutex> lk(g_mutex);
    std::fprintf(stderr, "%s.%03ld [%s] %s:%d  %s\n", when, ts.tv_nsec / 1000000,
                 tag(l), short_file(file), line, msg);
    std::fflush(stderr);
}

}  // namespace so
