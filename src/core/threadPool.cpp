#include "jitnodes/core/threadPool.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>

namespace jitnodes {

ThreadPool::ThreadPool(size_t workerCount) {
    for (size_t i = 0; i < workerCount; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

void ThreadPool::parallelFor(size_t count, const std::function<void(size_t)>& fn) {
    if (count == 0) return;
    if (count == 1 || workers_.empty()) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }

    struct State {
        std::atomic<size_t> next{0};
        std::atomic<size_t> done{0};
        size_t count = 0;
        const std::function<void(size_t)>* fn = nullptr;
        std::mutex mutex;
        std::condition_variable finished;
        std::exception_ptr error;
    };
    auto state = std::make_shared<State>();
    state->count = count;
    state->fn = &fn;

    // fn is only dereferenced for a claimed index < count, which implies the caller is still waiting.
    auto drain = [state] {
        for (;;) {
            const size_t i = state->next.fetch_add(1);
            if (i >= state->count) return;
            try {
                (*state->fn)(i);
            } catch (...) {
                std::lock_guard lock(state->mutex);
                if (!state->error) state->error = std::current_exception();
            }
            if (state->done.fetch_add(1) + 1 == state->count) {
                std::lock_guard lock(state->mutex);
                state->finished.notify_all();
            }
        }
    };

    const size_t helpers = std::min(workers_.size(), count - 1);
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < helpers; ++i) jobs_.push_back(drain);
    }
    for (size_t i = 0; i < helpers; ++i) wake_.notify_one();

    drain();
    std::unique_lock lock(state->mutex);
    state->finished.wait(lock, [&] { return state->done.load() == state->count; });
    if (state->error) std::rethrow_exception(state->error);
}

}  // namespace jitnodes
