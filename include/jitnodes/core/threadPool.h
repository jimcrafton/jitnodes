#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace jitnodes {

// Fixed-size pool. parallelFor runs fn(0..count-1) with the calling thread taking part, so it
// is safe to call from inside a pool task (nested parallelism) and with zero workers (inline).
// The first exception thrown by fn is rethrown after every index has finished.
class ThreadPool {
public:
    explicit ThreadPool(size_t workerCount);
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ~ThreadPool();

    size_t workerCount() const { return workers_.size(); }
    size_t concurrency() const { return workers_.size() + 1; }

    void parallelFor(size_t count, const std::function<void(size_t)>& fn);

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
};

}  // namespace jitnodes
