#include "modules/out_count.hpp"

#include "core/log.hpp"

#include <cmath>

#include <opencv2/imgproc.hpp>

namespace so {

OutCountModule::OutCountModule(OutCountConfig cfg) : cfg_(cfg) {}

void OutCountModule::on_frame(const FrameContext& ctx, EventSink& sink) {
    if (!ctx.cfg) return;
    const auto now = std::chrono::steady_clock::now();

    // ── remember every name this camera reads, on whichever track wore it ──
    //
    // THE DOORWAY IS THE WORST PLACE ON THE CAMERA TO READ A FACE, and it is
    // the only place this module watches. A person at the exit here is about
    // eighty pixels tall, so their face is about twelve — half of what the
    // matcher needs. Waiting to recognise them where they are smallest, with
    // their back turning to the camera, would mean never recognising them.
    //
    // They walked to that door from somewhere nearer the camera, where the face
    // was big enough, and the tracker held the same track the whole way. So the
    // name is taken wherever it is legible and carried to the door on the track
    // that earned it — never across tracks, or a lost-and-recreated track would
    // book out whoever was recognised last.
    for (const auto& f : ctx.faces) {
        if (f.track_id < 0 || f.employee_id == 0) continue;
        Named& n = named_[f.track_id];
        n.employee_id = f.employee_id;
        n.employee_name = f.employee_name;
        n.match_score = f.match_score;
        n.last_seen = now;
    }

    // ── who is standing in an exit zone right now ──────────────────────────
    for (const auto& t : ctx.tracks) {
        // THE FOOT POINT, not box overlap. A tall person walking PAST the
        // doorway has a box that covers the doorway; their feet do not. The
        // perimeter module learned this and the same reasoning applies here,
        // with a worse failure: an overlap test would book out everybody who
        // walks across the far side of the room.
        const cv::Point2f foot = foot_point(t.box);

        std::int64_t in_zone = 0;
        for (const auto& z : ctx.cfg->zones) {
            if (!z.enabled || z.type != ZoneType::Exit) continue;
            if (point_in_polygon(foot, z.polygon)) { in_zone = z.id; break; }
        }

        auto it = seen_.find(t.id);
        if (in_zone == 0) {
            // In the room. Remembered for this track, so that when it later
            // reaches the doorway we know the person came from inside.
            in_room_[t.id] = now;

            // OUT OF THE ZONE, STILL ON CAMERA. This used to delete the
            // candidate outright, which quietly lost every real departure: the
            // zone is in front of the doors, so leaving MEANS stepping out of
            // it towards them and being visible there for a moment.
            //
            // So the candidate is kept and the clock started. If they vanish
            // soon after, they went through the door. If they are still here
            // when `linger` is up, they changed their mind.
            if (it != seen_.end()) {
                Candidate& c = it->second;
                if (c.left_zone.time_since_epoch().count() == 0) c.left_zone = now;
                if (now - c.left_zone >= cfg_.linger) seen_.erase(it);
                else c.last_seen = now;
            }
            continue;
        }

        Candidate& c = (it == seen_.end()) ? seen_[t.id] : it->second;
        if (c.inside_since.time_since_epoch().count() == 0) c.inside_since = now;
        c.last_seen = now;
        c.last_seen_wall = ctx.ts;
        c.left_zone = {};          // back inside; the door-side clock resets
        c.zone_id = in_zone;
        c.last_box = t.box;
        if (now - c.inside_since >= cfg_.confirm) c.confirmed = true;
        if (in_room_.count(t.id)) c.came_from_room = true;

        // The evidence, kept whether or not it named anybody. The best face
        // is the biggest one seen — a crop a person can actually recognise.
        for (const auto& f : ctx.faces) {
            if (f.track_id != t.id) continue;
            if (f.guess_employee_id != 0 && f.face_px >= c.crop_px) {
                c.guess_employee_id = f.guess_employee_id;
                c.guess_score = f.guess_score;
                c.guess_margin = f.guess_margin;
                c.guess_weight = f.guess_weight;
            }
            if (ctx.frame && !ctx.frame->empty() && f.face_px > c.crop_px) {
                const cv::Rect r = cv::Rect(
                    cv::Point(static_cast<int>(f.box[0]), static_cast<int>(f.box[1])),
                    cv::Point(static_cast<int>(f.box[2]), static_cast<int>(f.box[3]))) &
                    cv::Rect(0, 0, ctx.frame->cols, ctx.frame->rows);
                if (r.width > 8 && r.height > 8) {
                    c.crop = (*ctx.frame)(r).clone();
                    c.crop_px = f.face_px;
                }
            }
        }

        // The name this track earned, wherever on the camera it was read.
        const auto n = named_.find(t.id);
        if (n != named_.end()) {
            c.employee_id = n->second.employee_id;
            c.employee_name = n->second.employee_name;
            c.match_score = n->second.match_score;
        }

        // Kept for the snapshot. By the time the exit is believed the person
        // has been gone for seconds, so the frame that proves it is this one.
        // Kept for ANY confirmed candidate, named or not: the unnamed ones are
        // precisely the exits a person will have to look at.
        if (c.confirmed && ctx.frame && !ctx.frame->empty())
            c.last_frame = ctx.frame->clone();
    }

    // ── who has been gone long enough to call it an exit ───────────────────
    for (auto it = seen_.begin(); it != seen_.end();) {
        Candidate& c = it->second;
        if (now - c.last_seen < cfg_.gone) { ++it; continue; }

        // IS SOMEBODY STILL STANDING THERE?
        //
        // "The track is gone" and "the person is gone" are not the same thing,
        // and conflating them is what books out people standing at the desk:
        // their track is dropped and re-opened under a new id, and the old
        // candidate times out unchallenged. Someone who really left takes
        // their body with them, so the frame answers it — if a track is
        // standing where this one vanished, it is the same person and nobody
        // has left.
        const cv::Point2f was = foot_point(c.last_box);
        const float reach = cfg_.same_spot * std::max(1.0f, c.last_box[2] - c.last_box[0]);
        bool still_there = false;
        for (const auto& t : ctx.tracks) {
            if (t.id == it->first) continue;          // the vanished one itself
            const cv::Point2f p = foot_point(t.box);
            if (std::hypot(p.x - was.x, p.y - was.y) <= reach) { still_there = true; break; }
        }
        if (still_there) {
            // Carry the evidence across to whoever is standing there now, so a
            // real exit a moment later is not left nameless by the churn.
            for (const auto& t : ctx.tracks) {
                if (t.id == it->first) continue;
                const cv::Point2f p = foot_point(t.box);
                if (std::hypot(p.x - was.x, p.y - was.y) > reach) continue;
                if (c.employee_id != 0 && !named_.count(t.id)) {
                    Named& n = named_[t.id];
                    n.employee_id = c.employee_id;
                    n.employee_name = c.employee_name;
                    n.match_score = c.match_score;
                    n.last_seen = now;
                }
                if (c.came_from_room) in_room_[t.id] = now;
                break;
            }
            it = seen_.erase(it);
            continue;
        }

        const bool eligible = c.confirmed && c.employee_id != 0 && c.came_from_room;

        // ONLY SOMEBODY ALREADY RECORDED IN TODAY.
        //
        // A person the roster has never seen arrive has no `in` for this `out`
        // to pair with, and the roll-up would read the result as a departure
        // from a building they were never in. A null roster means the engine
        // did not supply one, and nothing may be assumed from that either way,
        // so nothing is recorded.
        const bool present = ctx.presence && ctx.presence->present(c.employee_id);

        // A REAL EXIT WITH NO NAME IS A QUESTION, NOT A NON-EVENT.
        //
        // Everything about this exit held except the identity: they came from
        // the room, they stood in the doorway, they are gone. Dropping it
        // silently loses both the exit and the only look the system will get at
        // that person. It is kept for a human instead — never promoted to
        // attendance on the strength of a guess.
        if (c.confirmed && c.came_from_room && c.employee_id == 0) {
            ExitReview r;
            r.tenant_id = ctx.tenant_id;
            r.camera_id = ctx.camera_id;
            r.zone_id = c.zone_id;
            r.ts = c.last_seen_wall;
            r.track_id = it->first;
            r.guess_employee_id = c.guess_employee_id;
            r.guess_score = c.guess_score;
            r.guess_margin = c.guess_margin;
            r.guess_weight = c.guess_weight;
            r.crop = std::move(c.crop);
            r.snapshot = std::move(c.last_frame);
            sink.emit(std::move(r));
            it = seen_.erase(it);
            continue;
        }

        // NOT SO SOON AFTER ARRIVING. See OutCountConfig::min_stay.
        const bool settled_in =
            !ctx.presence ||
            ctx.presence->seconds_since_in(c.employee_id) >= cfg_.min_stay.count();
        if (eligible && present && !settled_in) {
            SO_LOG_INFO("[out_count] ignoring an exit for %s on camera %lld: "
                        "they were recorded arriving %lld s ago",
                        c.employee_name.c_str(),
                        static_cast<long long>(ctx.camera_id),
                        static_cast<long long>(
                            ctx.presence->seconds_since_in(c.employee_id)));
        }

        if (eligible && present && settled_in) {
            const auto last = last_out_.find(c.employee_id);
            if (last == last_out_.end() || now - last->second >= cfg_.debounce) {
                last_out_[c.employee_id] = now;

                AttendanceEvent e;
                e.tenant_id = ctx.tenant_id;
                e.camera_id = ctx.camera_id;
                e.employee_id = c.employee_id;
                e.direction = Direction::Out;
                // TIMED TO WHEN THEY WERE LAST SEEN, not to now. `now` is
                // `gone` milliseconds after they actually left, and that error
                // lands directly in the break total.
                e.ts = c.last_seen_wall;
                e.confidence = c.match_score;
                e.track_id = it->first;
                if (!c.last_frame.empty()) {
                    e.snapshot = std::move(c.last_frame);
                    cv::rectangle(e.snapshot,
                                  cv::Point(static_cast<int>(c.last_box[0]),
                                            static_cast<int>(c.last_box[1])),
                                  cv::Point(static_cast<int>(c.last_box[2]),
                                            static_cast<int>(c.last_box[3])),
                                  cv::Scalar(0, 140, 255), 2);
                }
                sink.emit(std::move(e));

                Event ev;
                ev.tenant_id = ctx.tenant_id;
                ev.camera_id = ctx.camera_id;
                ev.module = id();
                ev.severity = Severity::Info;
                ev.title = c.employee_name + " left";
                ev.detail = "walked out through the exit zone";
                ev.ts = c.last_seen_wall;
                ev.zone_id = c.zone_id;
                ev.employee_id = c.employee_id;
                ev.track_id = it->first;
                ev.notify = false;   // bookkeeping; nobody is paged because somebody left
                sink.emit(std::move(ev));
            }
        }
        it = seen_.erase(it);
    }

    // Names outlive the exit decision by design — a track may be recognised
    // early and reach the door a minute later — but not for ever, or a long
    // day leaks one entry per person who ever walked past.
    for (auto it = named_.begin(); it != named_.end();) {
        if (now - it->second.last_seen > std::chrono::minutes(5)) it = named_.erase(it);
        else ++it;
    }
    for (auto it = in_room_.begin(); it != in_room_.end();) {
        if (now - it->second > std::chrono::minutes(5)) it = in_room_.erase(it);
        else ++it;
    }
}

}  // namespace so
