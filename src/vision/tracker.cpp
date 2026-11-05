#include "vision/tracker.hpp"

#include <algorithm>
#include <limits>
#include <numeric>

namespace so {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

}  // namespace

std::vector<int> solve_assignment(const std::vector<float>& cost, int rows, int cols,
                                  float max_cost) {
    std::vector<int> row_of_col(static_cast<std::size_t>(cols), -1);
    std::vector<int> col_of_row(static_cast<std::size_t>(rows), -1);
    if (rows == 0 || cols == 0) return col_of_row;

    // Dual potentials, one per row and per column.
    std::vector<float> u(static_cast<std::size_t>(rows) + 1, 0.0f);
    std::vector<float> v(static_cast<std::size_t>(cols) + 1, 0.0f);

    auto c = [&](int r, int col) {
        const float x = cost[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) +
                             static_cast<std::size_t>(col)];
        return x >= max_cost ? kInf : x;
    };

    for (int r = 0; r < rows; ++r) {
        // Shortest augmenting path from row r.
        std::vector<float> dist(static_cast<std::size_t>(cols), kInf);
        std::vector<int> prev(static_cast<std::size_t>(cols), -1);
        std::vector<bool> done(static_cast<std::size_t>(cols), false);

        int cur_row = r;
        int sink = -1;
        while (sink < 0) {
            int best_col = -1;
            float best = kInf;
            for (int col = 0; col < cols; ++col) {
                if (done[static_cast<std::size_t>(col)]) continue;
                const float w = c(cur_row, col) - u[static_cast<std::size_t>(cur_row)] -
                                v[static_cast<std::size_t>(col)];
                if (w < dist[static_cast<std::size_t>(col)]) {
                    dist[static_cast<std::size_t>(col)] = w;
                    prev[static_cast<std::size_t>(col)] = cur_row;
                }
                if (dist[static_cast<std::size_t>(col)] < best) {
                    best = dist[static_cast<std::size_t>(col)];
                    best_col = col;
                }
            }
            if (best_col < 0 || best == kInf) break;   // row r cannot be assigned
            done[static_cast<std::size_t>(best_col)] = true;
            if (row_of_col[static_cast<std::size_t>(best_col)] < 0) {
                sink = best_col;
            } else {
                cur_row = row_of_col[static_cast<std::size_t>(best_col)];
            }
        }
        if (sink < 0) continue;

        // Walk the path back, flipping the assignments along it.
        int col = sink;
        while (col >= 0) {
            const int pr = prev[static_cast<std::size_t>(col)];
            const int prev_col = col_of_row[static_cast<std::size_t>(pr)];
            row_of_col[static_cast<std::size_t>(col)] = pr;
            col_of_row[static_cast<std::size_t>(pr)] = col;
            col = prev_col;
        }
    }

    // Drop any pair the caller forbade.
    for (int r = 0; r < rows; ++r) {
        const int col = col_of_row[static_cast<std::size_t>(r)];
        if (col >= 0 && c(r, col) >= max_cost) {
            col_of_row[static_cast<std::size_t>(r)] = -1;
            row_of_col[static_cast<std::size_t>(col)] = -1;
        }
    }
    return col_of_row;
}

Tracker::Internal::Internal(int id, const TrackInput& d) : kf(d.box) {
    t.id = id;
    t.box = d.box;
    t.score = d.score;
    t.kps = d.kps;
    t.hits = 1;
    t.age = 1;
}

Tracker::Tracker(TrackerConfig cfg) : cfg_(cfg) {}

void Tracker::reset() {
    tracks_.clear();
    reported_.clear();
    next_id_ = 1;
}

std::vector<Track> Tracker::update(const std::vector<TrackInput>& detections) {
    // ── predict ─────────────────────────────────────────────────────────────
    for (auto& tr : tracks_) {
        tr.t.box = tr.kf.predict();
        tr.t.age = tr.kf.age();
        tr.t.time_since_update = tr.kf.time_since_update();
    }

    std::vector<int> high, low;
    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (detections[i].score >= cfg_.high_thresh) high.push_back(static_cast<int>(i));
        else if (detections[i].score >= cfg_.low_thresh) low.push_back(static_cast<int>(i));
    }

    std::vector<int> free_tracks(tracks_.size());
    std::iota(free_tracks.begin(), free_tracks.end(), 0);

    auto associate = [&](const std::vector<int>& dets, std::vector<int>& avail) {
        std::vector<int> unmatched_dets;
        if (dets.empty() || avail.empty()) return dets;

        const int rows = static_cast<int>(avail.size());
        const int cols = static_cast<int>(dets.size());
        std::vector<float> cost(static_cast<std::size_t>(rows) *
                                static_cast<std::size_t>(cols));
        for (int r = 0; r < rows; ++r)
            for (int col = 0; col < cols; ++col)
                cost[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) +
                     static_cast<std::size_t>(col)] =
                    1.0f - box_iou(tracks_[static_cast<std::size_t>(
                                       avail[static_cast<std::size_t>(r)])].t.box,
                                   detections[static_cast<std::size_t>(
                                       dets[static_cast<std::size_t>(col)])].box);

        const auto assign = solve_assignment(cost, rows, cols, 1.0f - cfg_.match_iou);

        std::vector<bool> det_used(dets.size(), false);
        std::vector<int> still_free;
        for (int r = 0; r < rows; ++r) {
            const int col = assign[static_cast<std::size_t>(r)];
            const int ti = avail[static_cast<std::size_t>(r)];
            if (col < 0) {
                still_free.push_back(ti);
                continue;
            }
            const auto& d = detections[static_cast<std::size_t>(
                dets[static_cast<std::size_t>(col)])];
            auto& tr = tracks_[static_cast<std::size_t>(ti)];
            tr.kf.update(d.box);
            tr.t.box = tr.kf.state_box();
            tr.t.score = d.score;
            tr.t.kps = d.kps;          // keypoints ride along; the filter only tracks boxes
            tr.t.hits += 1;
            tr.t.time_since_update = 0;
            if (tr.t.hits >= cfg_.min_hits) tr.t.confirmed = true;
            det_used[static_cast<std::size_t>(col)] = true;
        }
        avail = still_free;
        for (std::size_t i = 0; i < dets.size(); ++i)
            if (!det_used[i]) unmatched_dets.push_back(dets[i]);
        return unmatched_dets;
    };

    // Stage 1: confident detections. Stage 2: the faint ones, which may only
    // sustain a track that already exists.
    const std::vector<int> unmatched_high = associate(high, free_tracks);
    associate(low, free_tracks);

    // ── new tracks, from confident detections only ──────────────────────────
    for (int di : unmatched_high)
        tracks_.emplace_back(next_id_++, detections[static_cast<std::size_t>(di)]);

    // ── retire ──────────────────────────────────────────────────────────────
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [&](const Internal& tr) {
                                     return tr.kf.time_since_update() > cfg_.max_age;
                                 }),
                  tracks_.end());

    reported_.clear();
    reported_.reserve(tracks_.size());
    std::vector<Track> out;
    for (auto& tr : tracks_) {
        tr.t.age = tr.kf.age();
        tr.t.time_since_update = tr.kf.time_since_update();
        reported_.push_back(tr.t);
        // A track is reported once it has been seen min_hits times and is
        // currently visible. Reporting a coasting track would put a person in a
        // zone they left, which for desk time is a fabricated minute.
        if (tr.t.confirmed && tr.t.time_since_update == 0) out.push_back(tr.t);
    }
    return out;
}

}  // namespace so
