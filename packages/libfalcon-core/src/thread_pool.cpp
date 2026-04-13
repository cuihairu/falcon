// Thread Pool Implementation
// Copyright (c) 2025 Falcon Project

#include "internal/thread_pool.hpp"

namespace falcon {
namespace internal {

ThreadPool::ThreadPool(std::size_t num_threads) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) {
            num_threads = 4;  // Fallback
        }
    }

    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_thread, this);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }

    cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock,
                  [this] { return tasks_.empty() && active_tasks_.load() == 0; });
}

void ThreadPool::worker_thread() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });

            if (stopped_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
            ++active_tasks_;
        }

        task();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_tasks_;
        }
        done_cv_.notify_all();
    }
}

}  // namespace internal
}  // namespace falcon
