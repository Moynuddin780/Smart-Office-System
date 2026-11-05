#include "modules/perimeter.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <set>

namespace so {

PerimeterModule::PerimeterModule(PerimeterConfig cfg) : cfg_(cfg) {}

void PerimeterModule::on_frame(const FrameContext& ctx, EventSink& sink) {
    if (!ctx.cfg) return;
    const auto now = std::chrono::steady_clock::now();
    std::set<std::pair<std::int64_t, int>> seen;

    for (const auto& z : ctx.cfg->zones) {
        if (!z.enabled) continue;
        if (z.type != ZoneType::Perimeter && z.type != ZoneType::Restricted) continue;
        if (z.polygon.size() < 3) continue;

        for (const auto& t : ctx.tracks) {
            if (!point_in_polygon(foot_point(t.box), z.polygon)) continue;

            const auto key = std::make_pair(z.id, t.id);
            seen.insert(key);
            auto& p = inside_[key];
            if (p.since.time_since_epoch().count() == 0) p.since = now;
            if (p.alerted || now - p.since < cfg_.confirm) continue;

            const auto cd = zone_cooldown_.find(z.id);
            if (cd != zone_cooldown_.end() && now - cd->second < cfg_.zone_cooldown) continue;

            p.alerted = true;
            zone_cooldown_[z.id] = now;

            Event e;
            e.tenant_id = ctx.tenant_id;
            e.camera_id = ctx.camera_id;
            e.module = id();
            e.severity = Severity::Critical;
            e.title = "Person in restricted zone";
            e.detail = "A person entered \"" + z.name + "\" and stayed for over " +
                       std::to_string(cfg_.confirm.count() / 1000) + " second(s).";
            e.ts = ctx.ts;
            e.zone_id = z.id;
            e.track_id = t.id;

            if (ctx.frame && !ctx.frame->empty()) {
                e.snapshot = ctx.frame->clone();
                std::vector<cv::Point> poly;
                poly.reserve(z.polygon.size());
                for (const auto& q : z.polygon)
                    poly.emplace_back(static_cast<int>(q.x), static_cast<int>(q.y));
                cv::polylines(e.snapshot, poly, true, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
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
    }

    // Anyone who left a zone is re-armed, so a second entry alerts again.
    for (auto it = inside_.begin(); it != inside_.end();) {
        if (seen.count(it->first) == 0) it = inside_.erase(it);
        else ++it;
    }
}

}  // namespace so
