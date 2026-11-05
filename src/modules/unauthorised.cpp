#include "modules/unauthorised.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace so {

UnauthorisedModule::UnauthorisedModule(UnauthorisedConfig cfg) : cfg_(cfg) {}

void UnauthorisedModule::on_frame(const FrameContext& ctx, EventSink& sink) {
    if (!ctx.cfg) return;
    const auto now = std::chrono::steady_clock::now();

    for (const auto& t : ctx.tracks) {
        const float person_px = t.box[3] - t.box[1];

        if (cfg_.restricted_zones_only) {
            bool in_restricted = false;
            for (const auto& z : ctx.cfg->zones) {
                if (!z.enabled || z.type != ZoneType::Restricted) continue;
                if (point_in_polygon(foot_point(t.box), z.polygon)) {
                    in_restricted = true;
                    break;
                }
            }
            if (!in_restricted) continue;
        }

        // Did this track produce a face at all this frame, and did it match?
        bool face_seen = false;
        bool matched = false;
        const FaceObs* obs = nullptr;
        for (const auto& f : ctx.faces) {
            if (f.track_id != t.id) continue;
            face_seen = true;
            obs = &f;
            if (f.employee_id != 0) matched = true;
            break;
        }

        if (matched) {
            unmatched_.erase(t.id);
            unidentified_.erase(t.id);
            continue;
        }

        if (!face_seen) {
            // No face this frame. Below the pre-filter we did not even look, so
            // the honest record is "unidentified" either way — never an alert.
            if (person_px < cfg_.min_person_px) unidentified_.insert(t.id);
            else if (unmatched_.find(t.id) == unmatched_.end()) unidentified_.insert(t.id);
            continue;
        }

        // A face was found and it matched nobody: this is the defensible case.
        unidentified_.erase(t.id);
        auto& s = unmatched_[t.id];
        s.last_seen = now;
        if (s.first_unmatched.time_since_epoch().count() == 0) s.first_unmatched = now;
        if (s.alerted || now - s.first_unmatched < cfg_.unknown_grace) continue;

        const auto cd = cooldown_.find(t.id);
        if (cd != cooldown_.end() && now - cd->second < cfg_.cooldown) continue;

        s.alerted = true;
        cooldown_[t.id] = now;

        Event e;
        e.tenant_id = ctx.tenant_id;
        e.camera_id = ctx.camera_id;
        e.module = id();
        e.severity = Severity::Critical;
        e.title = "Unrecognised person";
        e.detail = "A face was detected and matched no enrolled employee for over " +
                   std::to_string(cfg_.unknown_grace.count() / 1000) + " seconds.";
        e.ts = ctx.ts;
        e.track_id = t.id;

        if (ctx.frame && !ctx.frame->empty()) {
            e.snapshot = ctx.frame->clone();
            cv::rectangle(e.snapshot,
                          cv::Point(static_cast<int>(t.box[0]), static_cast<int>(t.box[1])),
                          cv::Point(static_cast<int>(t.box[2]), static_cast<int>(t.box[3])),
                          cv::Scalar(0, 0, 255), 3);
            // The face is the useful thumbnail here, not the whole body.
            const Box& b = obs ? obs->box : t.box;
            const cv::Rect roi(
                cv::Point(std::max(0, static_cast<int>(b[0])),
                          std::max(0, static_cast<int>(b[1]))),
                cv::Point(std::min(ctx.frame->cols, static_cast<int>(b[2])),
                          std::min(ctx.frame->rows, static_cast<int>(b[3]))));
            if (roi.width > 4 && roi.height > 4) e.crop = (*ctx.frame)(roi).clone();
        }
        sink.emit(std::move(e));
    }

    for (auto it = unmatched_.begin(); it != unmatched_.end();) {
        if (now - it->second.last_seen > std::chrono::seconds(60)) it = unmatched_.erase(it);
        else ++it;
    }
}

}  // namespace so
