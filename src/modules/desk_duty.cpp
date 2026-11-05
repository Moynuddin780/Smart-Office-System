#include "modules/desk_duty.hpp"

namespace so {

DeskDutyModule::DeskDutyModule(DeskConfig cfg) : cfg_(cfg) {}

void DeskDutyModule::on_frame(const FrameContext& ctx, EventSink& sink) {
    if (!ctx.cfg) return;
    const auto now = std::chrono::steady_clock::now();

    for (const auto& z : ctx.cfg->zones) {
        if (!z.enabled || z.type != ZoneType::Desk) continue;
        if (z.polygon.size() < 3) continue;

        // THE OWNER HAS TO BE IN THE BUILDING.
        //
        // This desk credits whoever sits in it to the employee it is assigned
        // to, and does that without checking a face every second — once
        // somebody is at their own desk they stay theirs, which is what makes
        // the count cheap and steady. The reasoning only holds while the
        // employee actually came in: on a day they did not, the person in the
        // seat is somebody else, and a report that hands them the hours in
        // silence is worse than one that shows nothing.
        //
        // A missing roster means "not known", not "absent" — in that case the
        // old behaviour stands rather than every desk silently going quiet.
        if (ctx.presence && z.employee_id != 0 && !ctx.presence->present(z.employee_id))
            continue;

        ZoneState& st = zones_[z.id];
        if (st.last_tick.time_since_epoch().count() != 0 && now - st.last_tick < cfg_.tick)
            continue;
        st.last_tick = now;

        // A DESK ASKS A DIFFERENT QUESTION FROM A PERIMETER.
        //
        // A perimeter asks "is somebody standing in here", and the foot point
        // answers it: somebody walking PAST the edge of a zone is not inside
        // it. A desk asks "is this seat taken", and the foot point is wrong for
        // that — a seated person's feet are under the desk or behind the chair,
        // usually outside the shape drawn around the working area.
        //
        // Measured here: people clearly sitting at their desks were counted
        // away, and the flicker in and out produced 59, 67 and 41 "sessions" in
        // one day for three people who sat down once each.
        //
        // So a desk takes whichever track OVERLAPS it most, by area, and a
        // modest overlap is enough: a person leaning back, turning, or half
        // hidden behind a monitor still occupies the seat.
        int occupant = -1;
        double best = 0.0;
        for (const auto& t : ctx.tracks) {
            const double f = box_polygon_overlap(t.box, z.polygon);
            if (f > best && f >= cfg_.min_overlap) {
                best = f;
                occupant = t.id;
            }
        }

        // Somebody there now; or absent, but within the grace period, which
        // still counts as at their desk.
        bool present = occupant >= 0;
        if (present) {
            st.absent_since = {};
        } else if (st.present) {
            if (st.absent_since.time_since_epoch().count() == 0) st.absent_since = now;
            if (now - st.absent_since < cfg_.absence_grace) present = true;
        }
        st.present = present;

        DeskTick tick;
        tick.tenant_id = ctx.tenant_id;
        tick.zone_id = z.id;
        tick.employee_id = z.employee_id;
        tick.ts = ctx.ts;
        tick.present = present;

        // If a face was recognised on the occupying track, record WHO. The
        // count is unaffected — the operator's assignment decides that — but a
        // report must not credit the wrong person without saying so.
        if (occupant >= 0) {
            for (const auto& f : ctx.faces) {
                if (f.track_id == occupant && f.employee_id != 0) {
                    tick.observed_employee_id = f.employee_id;
                    break;
                }
            }
        }
        sink.emit(tick);
    }
}

}  // namespace so
