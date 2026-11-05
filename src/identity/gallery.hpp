#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace so {

struct EnrolledProfile {
    std::int64_t employee_id{0};
    std::string name;
    std::string code;
    std::vector<float> embedding;
};

struct GallerySnapshot {
    std::vector<std::vector<float>> centred;   ///< rows, L2-normalised, centroid removed
    std::vector<std::int64_t> employee_ids;
    std::vector<std::string> names;
    std::vector<float> centroid;
    std::uint64_t version{0};
    int dim{0};
};

/**
 * The enrolled face embeddings, shared by every running camera.
 *
 * SWAPPED IN PLACE, NOT REBOUND
 * -----------------------------
 * Writers replace the CONTENTS under a lock and bump a monotonic version;
 * readers hold a reference to this same object and compare versions. That is
 * not incidental. The obvious alternative — publishing a fresh object and
 * rebinding a pointer — is invisible to anybody already holding the old one, so
 * a camera worker started on Monday keeps matching Monday's roster forever.
 * Every employee enrolled afterwards is invisible to it and every employee
 * deleted keeps matching, until someone restarts that camera. That bug is worth
 * designing out rather than remembering.
 *
 * MIXED EMBEDDING WIDTHS
 * ----------------------
 * Profiles are grouped by embedding width and only the majority width is kept.
 * Cosine similarity between vectors from different models is not merely
 * meaningless — it produces confident WRONG matches rather than no match. One
 * stale 128-d profile must not be able to take down a 512-d gallery.
 *
 * THE CENTROID, AND WHEN IT MUST NOT BE SUBTRACTED
 * -------------------------------------------------
 * Every enrolled vector shares a large common component: indoor light, these
 * cameras, roughly frontal heads. A profile built from pose-varied photos keeps
 * little BUT that, sits near the middle of the gallery, and then wins the argmax
 * against any live face too poor to look like anybody in particular. Subtracting
 * the gallery's mean direction before comparing removes it.
 */
/// Profiles needed before the gallery has a "common direction" worth removing.
///
/// Below this the centroid is not a shared component, it is the data. With ONE
/// enrolled person the centroid IS that person, so `row - centroid` is the zero
/// vector and nobody can ever be matched; with two it is their midpoint, which
/// pushes them to opposite poles and makes every live face look like one or the
/// other. Found the hard way: a deployment with one employee enrolled detected
/// 1033 faces and identified none of them.
constexpr int kCentroidMinProfiles = 3;

class Gallery {
public:
    Gallery();

    /// Replace the gallery. Returns how many profiles were kept.
    struct SetResult {
        int kept{0};
        int skipped{0};
        int dim{0};
        std::uint64_t version{0};
        std::vector<std::int64_t> skipped_ids;
    };
    SetResult set(const std::vector<EnrolledProfile>& profiles);

    /// A consistent view. Cheap to hold: the vectors are never mutated after
    /// publication, so a caller may keep the snapshot across frames.
    GallerySnapshot snapshot() const;

    std::uint64_t version() const;
    int count() const;
    int dim() const;
    std::vector<float> centroid() const;

private:
    mutable std::mutex mtx_;
    GallerySnapshot snap_;
};

}  // namespace so
