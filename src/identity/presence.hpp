#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>

namespace so {

/**
 * Who has been recorded arriving today.
 *
 * WHY A DESK NEEDS THIS: a desk zone counts whoever is sitting in it against
 * the employee the operator assigned, because recognising a face every second
 * is neither possible nor necessary — once somebody is at their own desk, they
 * stay theirs. That reasoning holds only while the employee is actually in the
 * building. On a day they never came in, the person in their seat is somebody
 * else, and crediting the hours to them is a quiet falsehood that a report
 * reader has no way to catch.
 *
 * Read on every desk tick of every camera, written once a minute, so it is a
 * plain guarded set rather than anything cleverer.
 */
class PresenceRoster {
public:
    bool present(std::int64_t employee_id) const {
        if (employee_id == 0) return false;
        std::lock_guard<std::mutex> lk(m_);
        return ids_.count(employee_id) != 0;
    }

    void set(std::set<std::int64_t> ids) {
        std::lock_guard<std::mutex> lk(m_);
        ids_ = std::move(ids);
    }

    /// Mark somebody the moment they are recognised, rather than waiting for
    /// the next refresh — the first thing a person does after arriving is walk
    /// to their desk, and a minute of their morning should not be lost to the
    /// polling interval.
    void add(std::int64_t employee_id) {
        if (employee_id == 0) return;
        std::lock_guard<std::mutex> lk(m_);
        ids_.insert(employee_id);
    }

    /// Remember that this person was recorded ARRIVING just now.
    ///
    /// Not the same thing as `add`. `add` answers "are they in the building",
    /// which a desk needs; this answers "how long have they been in", which is
    /// what tells a real departure from a mistaken one. A person recorded
    /// walking in two seconds ago has not just walked out.
    void mark_in(std::int64_t employee_id) {
        if (employee_id == 0) return;
        std::lock_guard<std::mutex> lk(m_);
        ids_.insert(employee_id);
        in_at_[employee_id] = std::chrono::steady_clock::now();
    }

    /// Seconds since this process last saw them arrive, or a large number when
    /// it never did.
    ///
    /// Large-when-unknown is the safe direction: an arrival this process did
    /// not witness — one from before a restart, or read back from the database
    /// — must not block a genuine departure hours later.
    std::int64_t seconds_since_in(std::int64_t employee_id) const {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = in_at_.find(employee_id);
        if (it == in_at_.end()) return 86400;
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - it->second).count();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return ids_.size();
    }

private:
    mutable std::mutex m_;
    std::set<std::int64_t> ids_;
    std::map<std::int64_t, std::chrono::steady_clock::time_point> in_at_;
};

}  // namespace so
