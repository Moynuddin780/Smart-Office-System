#include "core/postprocess_pool.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <exception>

namespace so {

PostprocessPool::PostprocessPool(int workers, int max_queue)
    : max_queue_(static_cast<std::size_t>(std::max(1, max_queue))) {
    const int n = std::max(1, workers);
    threads_.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) threads_.emplace_back(&PostprocessPool::loop, this);
}

PostprocessPool::~PostprocessPool() { stop(); }

bool PostprocessPool::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_.load()) return false;
        if (queue_.size() >= max_queue_) {
            // Drop the NEWEST rather than the oldest: the queued jobs are
            // already in flight for frames that have been decoded, and finishing
            // them keeps the event stream contiguous. Shedding the newest loses
            // one frame; shedding the oldest loses a frame AND reorders events.
            dropped_.fetch_add(1);
            return false;
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void PostprocessPool::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return stop_.load() || !queue_.empty(); });
            if (stop_.load() && queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        try {
            job();
        } catch (const std::exception& e) {
            // A module throwing must not take the pool down with it: the other
            // cameras are unaffected and should keep running.
            SO_LOG_ERROR("[postproc] job threw: %s", e.what());
        } catch (...) {
            SO_LOG_ERROR("[postproc] job threw a non-standard exception");
        }
        completed_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            --active_;
        }
        idle_cv_.notify_all();
    }
}

void PostprocessPool::drain() {
    std::unique_lock<std::mutex> lk(mtx_);
    idle_cv_.wait(lk, [&] { return queue_.empty() && active_ == 0; });
}

void PostprocessPool::stop() {
    if (stop_.exchange(true)) return;
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

std::size_t PostprocessPool::queued() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.size();
}

}  // namespace so
