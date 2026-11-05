#pragma once
#include <cstdio>
#include <string>

namespace so {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// The macros are SO_LOG_* rather than LOG_*: trantor (which Drogon pulls in)
// defines LOG_ERROR and friends as STREAM-style macros, and a printf-style
// macro of the same name silently breaks every one of its call sites inside
// Drogon's own headers.

// Level is read once from the LOG_LEVEL environment variable (debug|info|warn|
// error, default info). Reading it per call would put a getenv on the frame
// path, which is called thousands of times a second.
LogLevel log_level();
void set_log_level(LogLevel l);
void log_write(LogLevel l, const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

}  // namespace so

#define SO_LOG_DEBUG(...) \
    do { if (::so::log_level() <= ::so::LogLevel::Debug) \
        ::so::log_write(::so::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define SO_LOG_INFO(...) \
    do { if (::so::log_level() <= ::so::LogLevel::Info) \
        ::so::log_write(::so::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define SO_LOG_WARN(...) \
    do { if (::so::log_level() <= ::so::LogLevel::Warn) \
        ::so::log_write(::so::LogLevel::Warn, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define SO_LOG_ERROR(...) \
    do { ::so::log_write(::so::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__); } while (0)
