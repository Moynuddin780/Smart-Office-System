#include "notify/snapshot.hpp"

#include "core/log.hpp"

#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace so::notify {
namespace {

std::string today_utc() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string stamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream s;
    s << std::put_time(&tm, "%H%M%S") << '-' << std::setfill('0') << std::setw(3)
      << ms.count();
    static thread_local std::mt19937 rng{std::random_device{}()};
    s << '-' << std::hex << (rng() & 0xFFFF);
    return s.str();
}

/// Keep a path segment to characters that cannot climb out of the root.
std::string safe(const std::string& in) {
    std::string out;
    for (char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            out.push_back(c);
    }
    return out.empty() ? "unknown" : out;
}

}  // namespace

SnapshotStore::SnapshotStore(std::string root, int jpeg_quality)
    : root_(std::move(root)), quality_(jpeg_quality) {
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    if (ec) SO_LOG_WARN("[snapshot] cannot create %s: %s", root_.c_str(), ec.message().c_str());
}

std::string SnapshotStore::save(std::int64_t tenant_id, const std::string& tenant_slug,
                                const std::string& kind, const cv::Mat& image) {
    if (image.empty()) return "";

    const std::string tenant_dir =
        safe(tenant_slug.empty() ? ("t" + std::to_string(tenant_id)) : tenant_slug);
    const std::string rel =
        tenant_dir + "/" + today_utc() + "/" + safe(kind) + "-" + stamp() + ".jpg";

    const std::filesystem::path abs = std::filesystem::path(root_) / rel;
    std::error_code ec;
    std::filesystem::create_directories(abs.parent_path(), ec);

    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, quality_};
    if (!cv::imwrite(abs.string(), image, params)) {
        SO_LOG_WARN("[snapshot] failed to write %s", abs.string().c_str());
        return "";
    }
    return rel;
}

std::string SnapshotStore::put_bytes(std::int64_t tenant_id, const std::string& tenant_slug,
                                     const std::string& kind, const void* data,
                                     std::size_t len, const std::string& extension) {
    if (!data || len == 0) return "";
    const std::string tenant_dir =
        safe(tenant_slug.empty() ? ("t" + std::to_string(tenant_id)) : tenant_slug);
    const std::string rel =
        tenant_dir + "/" + safe(kind) + "/" + stamp() + "." + safe(extension);

    const std::filesystem::path abs = std::filesystem::path(root_) / rel;
    std::error_code ec;
    std::filesystem::create_directories(abs.parent_path(), ec);

    std::ofstream f(abs, std::ios::binary);
    if (!f) {
        SO_LOG_WARN("[snapshot] cannot write %s", abs.string().c_str());
        return "";
    }
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
    return f.good() ? rel : "";
}

void SnapshotStore::remove(const std::string& relative) const {
    const std::string abs = resolve(relative);
    if (abs.empty()) return;
    std::error_code ec;
    std::filesystem::remove(abs, ec);
}

std::string SnapshotStore::resolve(const std::string& relative) const {
    if (relative.empty()) return "";
    // Refuse anything that could walk out of the root. The path in the database
    // came from save(), but a request parameter could reach here one day.
    if (relative.find("..") != std::string::npos || relative.front() == '/') return "";

    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(root_, ec);
    const auto abs = std::filesystem::weakly_canonical(
        std::filesystem::path(root_) / relative, ec);
    if (ec) return "";
    const std::string a = abs.string(), r = root.string();
    if (a.rfind(r, 0) != 0) return "";
    return a;
}

}  // namespace so::notify
