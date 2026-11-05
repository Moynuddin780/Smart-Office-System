#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <string>

namespace so::notify {

/**
 * Where an alert's images live on disk.
 *
 * Partitioned by tenant and then by date: a client's images never share a
 * directory with another client's, and a retention job can drop a whole day
 * without walking the tree.
 *
 * These are NOT served as static files. They go out through an authorised
 * handler, because a snapshot of an unidentified person inside somebody's
 * office is exactly the kind of thing that must not be readable by guessing a
 * URL.
 */
class SnapshotStore {
public:
    SnapshotStore(std::string root, int jpeg_quality = 80);

    /// Returns the path RELATIVE to the root, which is what goes in the database.
    std::string save(std::int64_t tenant_id, const std::string& tenant_slug,
                     const std::string& kind, const cv::Mat& image);

    /// Store bytes verbatim, without re-encoding. Enrolment photos go through
    /// here: re-encoding an operator's upload would quietly change the pixels
    /// the embedding was computed from, so what is kept is what was sent.
    std::string put_bytes(std::int64_t tenant_id, const std::string& tenant_slug,
                          const std::string& kind, const void* data, std::size_t len,
                          const std::string& extension = "jpg");

    /// Absolute path for a stored relative path, or empty if it escapes the root.
    std::string resolve(const std::string& relative) const;

    /// Remove a stored file. Missing is not an error.
    void remove(const std::string& relative) const;

    const std::string& root() const { return root_; }

private:
    std::string root_;
    int quality_;
};

}  // namespace so::notify
