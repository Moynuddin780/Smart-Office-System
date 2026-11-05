#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace so {

/**
 * A small pool of worker threads with a BOUNDED queue.
 *
 * WHY IT EXISTS
 * -------------
 * DX-RT fires its completion callback on its own worker thread. Decoding nine
 * tensors, running a tracker and evaluating five modules inside that callback
 * would stall the NPU job queue — and serialise exactly the three-core
 * parallelism EnginePool exists to create. So the callback does one thing: post
 * the work here.
 *
 * WHY THE QUEUE IS BOUNDED AND DROPS
 * ----------------------------------
 * If postprocessing falls behind, the honest response is to do less of it, not
 * to accumulate a backlog. An unbounded queue turns a transient CPU spike into
 * permanently growing latency and eventually into memory exhaustion, and every
 * frame it holds is more stale than the last. Dropping is visible via
 * `dropped()` so the condition can be reported rather than merely survived.
 */
class PostprocessPool {
public:
    explicit PostprocessPool(int workers = 3, int max_queue = 32);
    ~PostprocessPool();

    PostprocessPool(const PostprocessPool&) = delete;
    PostprocessPool& operator=(const PostprocessPool&) = delete;

    /// Queue a job. Returns false if the queue was full and the job was dropped.
    bool post(std::function<void()> job);

    /// Block until the queue is empty and no job is running. For tests and
    /// shutdown, never on the frame path.
    void drain();

    void stop();

    std::size_t queued() const;
    std::int64_t completed() const { return completed_.load(); }
    std::int64_t dropped() const { return dropped_.load(); }
    int workers() const { return static_cast<int>(threads_.size()); }

private:
    void loop();

    std::vector<std::thread> threads_;
    std::deque<std::function<void()>> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::atomic<bool> stop_{false};
    std::atomic<std::int64_t> completed_{0};
    std::atomic<std::int64_t> dropped_{0};
    int active_{0};
    std::size_t max_queue_;
};

}  // namespace so
