#include "modules/attendance.hpp"

#include "core/log.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace so {

AttendanceModule::AttendanceModule(AttendanceModuleConfig cfg) : cfg_(cfg) {}

void AttendanceModule::on_frame(const FrameContext& ctx, EventSink& sink) {
    if (!ctx.cfg) return;
    const auto now = std::chrono::steady_clock::now();

    for (const auto& t : ctx.tracks) {
        TrackPath& p = paths_[t.id];
        p.last_seen = now;

        // Identity, once settled, sticks to the track: the person may cross the
        // line with their back to the camera, and the name was earned earlier.
        for (const auto& f : ctx.faces) {
            if (f.track_id == t.id && f.employee_id != 0) {
                p.employee_id = f.employee_id;
                p.employee_name = f.employee_name;
                p.match_score = f.match_score;
                break;
            }
        }


        // BEING RECOGNISED IS THE RECORD. There is no line to cross.
        //
        // Direction used to come from a drawn line: A->B an entry, B->A an
        // exit. It is gone because the deployment does not need it — one
        // camera watches the front door, and anybody it recognises has
        // arrived. Asking an operator to draw a line on that camera added a
        // step that could be forgotten, and until it was drawn the camera
        // recognised people all day and recorded nothing.
        //
        // The day is still closed without an exit punch: see assumed_out in
        // the day roll-up, which was already there for the person who leaves
        // past a camera that never saw them.
        if (p.employee_id != 0) {
            const auto last = last_event_.find(p.employee_id);
            if (last == last_event_.end() || now - last->second >= cfg_.sighting_debounce) {
                last_event_[p.employee_id] = now;

                AttendanceEvent e;
                e.tenant_id = ctx.tenant_id;
                e.camera_id = ctx.camera_id;
                e.employee_id = p.employee_id;
                e.direction = Direction::In;
                e.ts = ctx.ts;
                e.confidence = p.match_score;
                e.track_id = t.id;
                if (ctx.frame && !ctx.frame->empty()) {
                    e.snapshot = ctx.frame->clone();
                    cv::rectangle(e.snapshot,
                                  cv::Point(static_cast<int>(t.box[0]), static_cast<int>(t.box[1])),
                                  cv::Point(static_cast<int>(t.box[2]), static_cast<int>(t.box[3])),
                                  cv::Scalar(0, 200, 0), 2);
                }
                sink.emit(std::move(e));
            }
        }

    }

    for (auto it = paths_.begin(); it != paths_.end();) {
        if (now - it->second.last_seen > std::chrono::seconds(30)) it = paths_.erase(it);
        else ++it;
    }
}

}  // namespace so
