#include "identity/gallery.hpp"

#include "core/log.hpp"
#include "vision/arcface.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace so {

Gallery::Gallery() { snap_.version = 0; }

Gallery::SetResult Gallery::set(const std::vector<EnrolledProfile>& profiles) {
    // Group by embedding width; keep the majority.
    std::map<std::size_t, std::vector<const EnrolledProfile*>> by_dim;
    int empty = 0;
    for (const auto& p : profiles) {
        if (p.embedding.empty()) {
            ++empty;
            continue;
        }
        by_dim[p.embedding.size()].push_back(&p);
    }

    SetResult r;
    r.skipped = empty;

    std::vector<const EnrolledProfile*> kept;
    std::size_t keep_dim = 0;
    for (const auto& [d, v] : by_dim) {
        if (v.size() > kept.size()) {
            kept = v;
            keep_dim = d;
        }
    }
    for (const auto& [d, v] : by_dim) {
        if (d == keep_dim) continue;
        r.skipped += static_cast<int>(v.size());
        for (const auto* p : v) r.skipped_ids.push_back(p->employee_id);
    }

    GallerySnapshot next;
    next.dim = static_cast<int>(keep_dim);

    if (!kept.empty()) {
        // Centroid of the L2-normalised rows.
        std::vector<double> acc(keep_dim, 0.0);
        std::vector<std::vector<float>> rows;
        rows.reserve(kept.size());
        for (const auto* p : kept) {
            std::vector<float> v = p->embedding;
            l2_normalize(v);
            for (std::size_t i = 0; i < keep_dim; ++i) acc[i] += v[i];
            rows.push_back(std::move(v));
        }
        std::vector<float> centroid(keep_dim);
        for (std::size_t i = 0; i < keep_dim; ++i)
            centroid[i] = static_cast<float>(acc[i] / static_cast<double>(rows.size()));
        l2_normalize(centroid);

        // With fewer than kCentroidMinProfiles the centroid is not a shared
        // direction, it is the data. Subtracting it would annihilate a
        // single-profile gallery entirely. See the header.
        const bool centre = static_cast<int>(rows.size()) >= kCentroidMinProfiles;
        next.centroid = centre ? centroid : std::vector<float>(keep_dim, 0.0f);
        next.centred.reserve(rows.size());
        for (auto& v : rows) {
            if (centre) {
                for (std::size_t i = 0; i < keep_dim; ++i) v[i] -= centroid[i];
                l2_normalize(v);
            }
            next.centred.push_back(std::move(v));
        }
        if (!centre)
            SO_LOG_INFO("[gallery] %zu profile(s): comparing uncentred, because a "
                        "centroid over so few is the data rather than a shared "
                        "direction", rows.size());
        for (const auto* p : kept) {
            next.employee_ids.push_back(p->employee_id);
            next.names.push_back(p->name);
        }
    } else {
        next.centroid.assign(kEmbeddingDim, 0.0f);
        next.dim = kEmbeddingDim;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        next.version = snap_.version + 1;   // in place, so live readers see it
        snap_ = std::move(next);
        r.version = snap_.version;
        r.kept = static_cast<int>(snap_.centred.size());
        r.dim = snap_.dim;
    }

    if (!r.skipped_ids.empty())
        SO_LOG_WARN("[gallery] dropped %zu profile(s) whose embedding width differs "
                 "from the majority (%d) — cross-model cosine gives confident "
                 "WRONG matches, not no match",
                 r.skipped_ids.size(), r.dim);
    SO_LOG_INFO("[gallery] v%llu — %d identit%s live",
             static_cast<unsigned long long>(r.version), r.kept,
             r.kept == 1 ? "y" : "ies");
    return r;
}

GallerySnapshot Gallery::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_;
}

std::uint64_t Gallery::version() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_.version;
}

int Gallery::count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return static_cast<int>(snap_.centred.size());
}

int Gallery::dim() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_.dim;
}

std::vector<float> Gallery::centroid() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_.centroid;
}

}  // namespace so
