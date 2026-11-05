#include "identity/face_identifier.hpp"

#include "vision/arcface.hpp"

#include <algorithm>
#include <cmath>

namespace so {

FacePose face_pose(const std::array<Landmark, kNumLandmarks>& lm) {
    const auto& le = lm[LM_LEFT_EYE];
    const auto& re = lm[LM_RIGHT_EYE];
    const auto& nose = lm[LM_NOSE];
    const auto& lmo = lm[LM_LEFT_MOUTH];
    const auto& rmo = lm[LM_RIGHT_MOUTH];

    const float ex = (le.x + re.x) * 0.5f;
    const float ey = (le.y + re.y) * 0.5f;
    const float vx = re.x - le.x;
    const float vy = re.y - le.y;
    const float eye_d = std::max(1e-6f, std::hypot(vx, vy));
    const float ang = std::atan2(vy, vx);

    // Rotate into the eye-line frame so roll does not contaminate yaw.
    const float ca = std::cos(-ang), sa = std::sin(-ang);
    const float nx = ca * (nose.x - ex) - sa * (nose.y - ey);
    const float ny = sa * (nose.x - ex) + ca * (nose.y - ey);
    const float mx_ = (lmo.x + rmo.x) * 0.5f - ex;
    const float my_ = (lmo.y + rmo.y) * 0.5f - ey;
    const float my = sa * mx_ + ca * my_;

    FacePose p;
    p.yaw = nx / eye_d;
    p.pitch = ny / std::max(std::abs(my), 1e-6f);
    p.roll = ang * 180.0f / 3.14159265358979f;
    return p;
}

FaceIdentifier::FaceIdentifier(const Gallery& gallery, IdentifierConfig cfg)
    : gallery_(gallery), cfg_(cfg) {}

/// Another live track already settled on this employee, if any.
///
/// Stale tracks are ignored deliberately: somebody who walked out of frame and
/// comes back a minute later must not be blocked by the identity their own
/// previous track is still holding.
FaceIdentifier::TrackState* FaceIdentifier::other_holder(int track_id,
                                                         std::int64_t employee_id) {
    if (employee_id == 0) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    for (auto& [tid, other] : tracks_) {
        if (tid == track_id) continue;
        if (!other.id.settled || other.id.employee_id != employee_id) continue;
        if (now - other.last_seen > cfg_.track_ttl) continue;
        return &other;
    }
    return nullptr;
}

Identification FaceIdentifier::match(const TrackState& st, const GallerySnapshot& g) {
    Identification out;
    out.weight = st.weight;
    if (g.centred.empty() || st.best.empty()) return out;

    // Quality-weighted mean of the pooled frames, centred the same way the
    // gallery is, then L2-normalised so the comparison is a plain cosine.
    const std::size_t dim = st.best.front().second.size();
    if (static_cast<int>(dim) != g.dim) return out;

    std::vector<float> tmpl(dim, 0.0f);
    for (const auto& [w, v] : st.best)
        for (std::size_t i = 0; i < dim; ++i) tmpl[i] += w * v[i];
    l2_normalize(tmpl);

    // The query is centred only if the gallery was. A zero centroid means the
    // gallery declined to centre (too few profiles), and subtracting it anyway
    // would be a no-op here but a silent inconsistency waiting to be introduced.
    bool centred = false;
    for (float c : g.centroid) if (c != 0.0f) { centred = true; break; }
    if (centred) {
        for (std::size_t i = 0; i < dim; ++i) tmpl[i] -= g.centroid[i];
        l2_normalize(tmpl);
    }

    float best = -2.0f, second = -2.0f;
    std::size_t best_i = 0;
    for (std::size_t r = 0; r < g.centred.size(); ++r) {
        const float s = cosine(tmpl, g.centred[r]);
        if (s > best) {
            second = best;
            best = s;
            best_i = r;
        } else if (s > second) {
            second = s;
        }
    }
    out.employee_id = g.employee_ids[best_i];
    out.name = g.names[best_i];
    out.score = best;
    out.margin = (second <= -2.0f) ? best : best - second;
    return out;
}

Identification FaceIdentifier::best_guess(int track_id) const {
    const auto it = tracks_.find(track_id);
    if (it == tracks_.end()) return {};
    // `settled` stays false by construction: this is evidence, never a verdict.
    return match(it->second, gallery_.snapshot());
}

void FaceIdentifier::rescore(int track_id, TrackState& st, const GallerySnapshot& g) {
    st.id.weight = st.weight;
    if (g.centred.empty() || st.best.empty() || st.weight < cfg_.min_weight) return;

    const Identification m = match(st, g);
    if (m.employee_id == 0 && m.score == 0.0f) return;
    const float best = m.score;
    const float margin = m.margin;
    std::size_t best_i = 0;
    for (std::size_t r = 0; r < g.employee_ids.size(); ++r)
        if (g.employee_ids[r] == m.employee_id) { best_i = r; break; }

    if (best < cfg_.min_score || margin < cfg_.min_margin) {
        // Not enough to name anyone. A previously settled name is kept — a
        // moment of bad evidence should not un-name somebody.
        if (!st.id.settled) st.id = Identification{0, "", best, margin, st.weight, false};
        return;
    }

    const std::int64_t candidate = g.employee_ids[best_i];
    if (st.id.settled && st.id.employee_id != candidate &&
        margin < cfg_.rename_margin) {
        return;   // a challenger must clearly beat the incumbent
    }

    // One person cannot be in two places at once.
    //
    // Each track is scored on its own, so without this nothing stops two
    // different people in the same frame from both being called Moynuddin —
    // which is exactly what happened on the live wall, twice in one picture.
    // It is not a near miss to tolerate: whoever is looking at the screen now
    // has to guess which label is the real one, and attendance would take both.
    //
    // Resolution is by strength of evidence, not by who got there first: the
    // better-scoring track keeps the name and the other is un-named. The loser
    // is not silenced — it keeps its weight and rescores on the next frame, so
    // if it is really somebody else it settles on its own identity shortly.
    if (TrackState* holder = other_holder(track_id, candidate)) {
        if (holder->id.score >= best) {
            // Someone else has a better claim. Keep the score so the overlay can
            // say "? 0.73" rather than pretending the face was never weighed.
            if (!st.id.settled) st.id = Identification{0, "", best, margin, st.weight, false};
            return;
        }
        // This track's claim is stronger — release the weaker one.
        holder->id = Identification{0, "", holder->id.score, holder->id.margin,
                                    holder->weight, false};
    }

    st.id.employee_id = candidate;
    st.id.name = g.names[best_i];
    st.id.score = best;
    st.id.margin = margin;
    st.id.weight = st.weight;
    st.id.settled = true;
}

Identification FaceIdentifier::observe(int track_id, const std::vector<float>& embedding,
                                       float yaw, float face_px, float det_score,
                                       std::chrono::steady_clock::time_point now) {
    TrackState& st = tracks_[track_id];
    st.last_seen = now;

    const bool usable = det_score >= cfg_.min_det_score &&
                        face_px >= cfg_.min_face_px &&
                        std::abs(yaw) <= cfg_.max_yaw &&
                        !embedding.empty();
    if (usable) {
        st.judged = true;

        // IS THIS STILL THE SAME FACE?
        //
        // A box can change who is inside it. Two people cross, the tracker
        // hands the box to the wrong one, and because the name is bound to the
        // TRACK it goes with it — the label sits on a stranger, and their
        // frames then pool into the template of somebody they are not. On this
        // site a name moved onto a passer-by exactly this way.
        //
        // The faces themselves give it away. Within one person, frames agree at
        // around 0.6 and up; two different people in this gallery sit at 0.18
        // on average and never pass 0.44. So a frame that disagrees with
        // everything pooled so far is not a bad look at the same person, it is
        // a different person, and the right response is to forget what was
        // learnt rather than average the two together.
        //
        // The identity goes too. Keeping it would leave a confident name on a
        // face that has just contradicted it, which is the worst of the three
        // possible outcomes — worse than a wrong name briefly, and far worse
        // than no name.
        if (!st.best.empty()) {
            std::vector<float> pooled(embedding.size(), 0.0f);
            for (const auto& [w_, v] : st.best)
                for (std::size_t i = 0; i < pooled.size() && i < v.size(); ++i)
                    pooled[i] += w_ * v[i];
            l2_normalize(pooled);
            // SUSTAINED disagreement, not a single frame. One blurred or
            // half-turned look can fall below the line on anybody, and wiping
            // a good identity for it would be its own bug — worse than the one
            // being fixed, because it would fire constantly. A box that has
            // genuinely changed hands disagrees on every frame from then on.
            if (cosine(pooled, embedding) < cfg_.same_face_min) {
                if (++st.disagreements < cfg_.swap_frames) {
                    // Still deciding. The frame is NOT pooled while we decide,
                    // and that is the whole trick: pooling it would drag the
                    // template towards the newcomer, the next frame would agree
                    // with the template it just moved, and the count would
                    // never reach the limit. The swap would complete silently
                    // one frame at a time — which is what happened when this
                    // was first written.
                    return st.id;
                }
                // Sustained. Forget this track's learning and let the frame in
                // front of the camera start it again.
                st.best.clear();
                st.weight = 0.0f;
                st.id = Identification{};
                st.disagreements = 0;
            } else {
                st.disagreements = 0;
            }
        }

        const float yw = std::exp(-(yaw / cfg_.yaw_sigma) * (yaw / cfg_.yaw_sigma));
        const float sw = std::min(1.0f, face_px / cfg_.full_px);
        // How sure the detector was, as a weight rather than a gate. A frame it
        // was hesitant about is usually blurred or half-occluded, and the
        // embedding is correspondingly less trustworthy — but it is not
        // worthless, and treating it as such threw away every small face.
        const float dw = std::min(1.0f, det_score);
        const float w = yw * sw * dw;

        st.best.emplace_back(w, embedding);
        std::sort(st.best.begin(), st.best.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        if (static_cast<int>(st.best.size()) > cfg_.template_frames)
            st.best.resize(static_cast<std::size_t>(cfg_.template_frames));

        st.weight = 0.0f;
        for (const auto& [ww, _] : st.best) st.weight += ww;

        const GallerySnapshot g = gallery_.snapshot();
        rescore(track_id, st, g);
    }
    return st.id;
}

Identification FaceIdentifier::current(int track_id) const {
    const auto it = tracks_.find(track_id);
    return it == tracks_.end() ? Identification{} : it->second.id;
}

bool FaceIdentifier::was_judged(int track_id) const {
    const auto it = tracks_.find(track_id);
    return it != tracks_.end() && it->second.judged;
}

bool FaceIdentifier::needs_more_looks(int track_id) const {
    const auto it = tracks_.find(track_id);
    if (it == tracks_.end()) return true;   // never looked at: a stranger
    const auto& st = it->second;
    return !st.id.settled && st.weight < cfg_.min_weight;
}

void FaceIdentifier::forget(int track_id) { tracks_.erase(track_id); }

void FaceIdentifier::prune(std::chrono::steady_clock::time_point now) {
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (now - it->second.last_seen > cfg_.track_ttl) it = tracks_.erase(it);
        else ++it;
    }
}

}  // namespace so
