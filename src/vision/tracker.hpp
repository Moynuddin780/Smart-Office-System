#pragma once

#include "vision/geometry.hpp"
#include "vision/kalman_box.hpp"
#include "vision/yolo26_pose.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace so {

/// What the tracker was handed for one frame.
struct TrackInput {
    Box box{};
    float score{0};
    std::array<Keypoint, kNumKeypoints> kps{};
};

/// A person the tracker is following.
struct Track {
    int id{0};
    Box box{};                                   ///< the filtered estimate
    float score{0};
    std::array<Keypoint, kNumKeypoints> kps{};   ///< carried through from the detection
    int age{0};                                  ///< frames since the track was created
    int hits{0};                                 ///< frames it has been matched on
    int time_since_update{0};
    bool confirmed{false};
};

struct TrackerConfig {
    float high_thresh = 0.50f;   ///< detections above this open new tracks
    float low_thresh = 0.20f;    ///< ...and between low and high, only sustain them
    float match_iou = 0.20f;     ///< minimum IoU to associate (cost = 1 - IoU)
    int max_age = 30;            ///< frames a track survives unmatched
    int min_hits = 3;            ///< matched frames before a track is reported
};

/**
 * Two-stage IoU tracker in the ByteTrack shape, over a per-track Kalman filter.
 *
 * WHY TWO STAGES
 * --------------
 * A person who is briefly occluded — walking behind a colleague, turning away
 * from the camera — does not vanish; their detection score drops. Discarding
 * everything below the confidence floor therefore breaks exactly the tracks
 * that matter most for attendance and desk time, because it breaks them at the
 * moment the person was doing something. So: high-confidence detections are
 * matched first and may create new tracks; low-confidence ones are matched
 * afterwards against whatever is still unmatched, and may only SUSTAIN a track,
 * never start one.
 *
 * WHY NOT OC_SORT
 * ---------------
 * dx_stream ships a good OC_SORT implementation, but under a DEEPX licence
 * granting use to "customers who are supplied with DEEPX NPU" — a question
 * worth answering deliberately before vendoring it into a product that is
 * sold on. This tracker is clean-room and carries no such condition. The
 * interface is small enough that swapping OC_SORT in later means implementing
 * `update()`; nothing above this file knows which tracker it is talking to.
 */
class Tracker {
public:
    explicit Tracker(TrackerConfig cfg = {});

    /// Advance one frame. Returns the confirmed tracks.
    std::vector<Track> update(const std::vector<TrackInput>& detections);

    /// Every track including tentative ones — for tests and diagnostics.
    const std::vector<Track>& all_tracks() const { return reported_; }

    void reset();

private:
    struct Internal {
        Track t;
        KalmanBox kf;
        Internal(int id, const TrackInput& d);
    };

    TrackerConfig cfg_;
    std::vector<Internal> tracks_;
    std::vector<Track> reported_;
    int next_id_{1};
};

/**
 * Minimum-cost assignment on a rectangular cost matrix (Jonker-Volgenant style
 * augmenting-path solver, O(n^2 m)).
 *
 * Greedy nearest-IoU matching is the obvious shortcut and it is wrong in the
 * exact case this system cares about: two people passing each other swap
 * identities, because greedy commits to the first good pair it sees rather than
 * to the best set of pairs. For attendance that is somebody else's working day.
 *
 * @param cost      rows x cols, row-major
 * @param max_cost  pairs at or above this are not allowed to match
 * @return for each row, the column it is assigned to, or -1
 */
std::vector<int> solve_assignment(const std::vector<float>& cost, int rows, int cols,
                                  float max_cost);

}  // namespace so
