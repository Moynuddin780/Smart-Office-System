#include "modules/fall.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace so {
namespace {

bool visible(const Keypoint& k, float thr) { return k.conf >= thr; }

/// Midpoint of two keypoints, falling back to whichever one is visible. A
/// person seen side-on legitimately shows one shoulder or one hip, and that is
/// still usable — requiring both would blind the module to anyone in profile.
bool midpoint(const std::array<Keypoint, kNumKeypoints>& k, int a, int b, float thr,
              float& ox, float& oy) {
    const bool va = visible(k[static_cast<std::size_t>(a)], thr);
    const bool vb = visible(k[static_cast<std::size_t>(b)], thr);
    if (va && vb) {
        ox = (k[static_cast<std::size_t>(a)].x + k[static_cast<std::size_t>(b)].x) * 0.5f;
        oy = (k[static_cast<std::size_t>(a)].y + k[static_cast<std::size_t>(b)].y) * 0.5f;
        return true;
    }
    if (va) { ox = k[static_cast<std::size_t>(a)].x; oy = k[static_cast<std::size_t>(a)].y; return true; }
    if (vb) { ox = k[static_cast<std::size_t>(b)].x; oy = k[static_cast<std::size_t>(b)].y; return true; }
    return false;
}

}  // namespace

PostureVerdict posture(const std::array<Keypoint, kNumKeypoints>& kps,
                       const FallConfig& cfg) {
    PostureVerdict v;

    float sx, sy, hx, hy;
    if (!midpoint(kps, LEFT_SHOULDER, RIGHT_SHOULDER, cfg.kp_conf, sx, sy) ||
        !midpoint(kps, LEFT_HIP, RIGHT_HIP, cfg.kp_conf, hx, hy)) {
        v.why = "no torso";
        return v;
    }

    const float dx = std::abs(hx - sx);
    const float dy = std::abs(hy - sy);
    const float torso = std::hypot(dx, dy);
    if (torso < 1.0f) {
        v.why = "torso too short";
        return v;
    }
    // 0 degrees is vertical; a body lying flat approaches 90.
    v.torso_angle = std::atan2(dx, dy) * 180.0f / 3.14159265358979f;

    // Head drop, measured over the person's OWN span so it scales with distance.
    float ax, ay;
    const bool have_ankle = midpoint(kps, LEFT_ANKLE, RIGHT_ANKLE, cfg.kp_conf, ax, ay);
    // Only the vertical position of the head matters for the drop test.
    float hey = sy;
    if (visible(kps[NOSE], cfg.kp_conf)) hey = kps[NOSE].y;
    if (have_ankle) {
        const float span = std::max(std::abs(ay - hey), 1e-6f);
        const float body = std::max(torso * 2.2f, 1e-6f);   // torso ~45% of standing height
        v.head_low = (span / body) <= cfg.head_low_frac;
    }

    // Seated: knees near hip level while the ankles are well below.
    float kx, ky;
    if (midpoint(kps, LEFT_KNEE, RIGHT_KNEE, cfg.kp_conf, kx, ky) && have_ankle) {
        const float hip_to_ankle = std::max(std::abs(ay - hy), 1e-6f);
        v.knees_high = (std::abs(ky - hy) / hip_to_ankle) <= cfg.knee_hip_frac;
    }

    if (v.knees_high && v.torso_angle < cfg.torso_down_deg) {
        v.verdict = Posture::Up;
        v.why = "seated";
        return v;
    }
    if (v.torso_angle >= cfg.torso_down_deg) {
        v.verdict = Posture::Down;
        v.why = "torso horizontal";
        return v;
    }
    if (v.head_low) {
        v.verdict = Posture::Down;
        v.why = "head dropped";
        return v;
    }
    v.verdict = Posture::Up;
    v.why = "upright";
    return v;
}

FallTracker::FallTracker(FallConfig cfg) : cfg_(cfg) {}

FallState FallTracker::feed(int track_id, const Track& track,
                            std::chrono::steady_clock::time_point now) {
    State& st = tracks_[track_id];
    st.last_seen = now;

    const float h = std::max(1.0f, track.box[3] - track.box[1]);
    const float w = std::max(1.0f, track.box[2] - track.box[0]);
    const float cx = (track.box[0] + track.box[2]) * 0.5f;
    const float cy = (track.box[1] + track.box[3]) * 0.5f;
    if (st.height > 1.0f) st.moved = std::hypot(cx - st.last_cx, cy - st.last_cy) / st.height;
    st.last_cx = cx;
    st.last_cy = cy;
    st.height = h;

    const PostureVerdict pv = posture(track.kps, cfg_);
    bool down;
    if (pv.verdict == Posture::Unknown) {
        // Keypoints unusable — too far away, torso behind furniture. Falling
        // back to box geometry is worse, but it is not blind.
        down = (w / h) >= cfg_.box_aspect_down;
    } else {
        down = pv.verdict == Posture::Down;
    }

    if (!down) {
        st.down = false;
        st.state = FallState::None;
        st.down_since = {};
        return st.state;
    }

    if (!st.down) {
        st.down = true;
        st.down_since = now;
    }
    const auto held = now - st.down_since;
    if (held >= cfg_.help && st.moved < cfg_.still_frac) st.state = FallState::NeedHelp;
    else if (held >= cfg_.confirm) st.state = FallState::Fallen;
    return st.state;
}

FallState FallTracker::state(int track_id) const {
    const auto it = tracks_.find(track_id);
    return it == tracks_.end() ? FallState::None : it->second.state;
}

void FallTracker::forget(int track_id) { tracks_.erase(track_id); }

void FallTracker::prune(std::chrono::steady_clock::time_point now) {
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (now - it->second.last_seen > std::chrono::seconds(30)) it = tracks_.erase(it);
        else ++it;
    }
}

FallModule::FallModule(FallConfig cfg) : cfg_(cfg), tracker_(cfg) {}

void FallModule::on_frame(const FrameContext& ctx, EventSink& sink) {
    const auto now = std::chrono::steady_clock::now();

    for (const auto& t : ctx.tracks) {
        const FallState before = tracker_.state(t.id);
        const FallState after = tracker_.feed(t.id, t, now);
        if (after == before || after == FallState::None) continue;

        const auto last = last_alert_.find(t.id);
        if (last != last_alert_.end() && now - last->second < cfg_.cooldown &&
            after != FallState::NeedHelp)
            continue;
        last_alert_[t.id] = now;

        Event e;
        e.tenant_id = ctx.tenant_id;
        e.camera_id = ctx.camera_id;
        e.module = id();
        e.ts = ctx.ts;
        e.track_id = t.id;
        if (after == FallState::NeedHelp) {
            e.severity = Severity::Critical;
            e.title = "Person down and not moving";
            e.detail = "A person has been on the floor for over " +
                       std::to_string(cfg_.help.count() / 1000) +
                       " seconds with little movement.";
        } else {
            e.severity = Severity::Warning;
            e.title = "Possible fall";
            e.detail = "A person's posture has read as fallen for over " +
                       std::to_string(cfg_.confirm.count() / 1000) + " second(s).";
        }

        if (ctx.frame && !ctx.frame->empty()) {
            e.snapshot = ctx.frame->clone();
            cv::rectangle(e.snapshot,
                          cv::Point(static_cast<int>(t.box[0]), static_cast<int>(t.box[1])),
                          cv::Point(static_cast<int>(t.box[2]), static_cast<int>(t.box[3])),
                          cv::Scalar(0, 0, 255), 3);
            const cv::Rect roi(
                cv::Point(std::max(0, static_cast<int>(t.box[0])),
                          std::max(0, static_cast<int>(t.box[1]))),
                cv::Point(std::min(ctx.frame->cols, static_cast<int>(t.box[2])),
                          std::min(ctx.frame->rows, static_cast<int>(t.box[3]))));
            if (roi.width > 4 && roi.height > 4) e.crop = (*ctx.frame)(roi).clone();
        }
        sink.emit(std::move(e));
    }
    tracker_.prune(now);
}

}  // namespace so
