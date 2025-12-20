#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace falcon {
namespace internal {

/// Simple thread pool for parallel task execution
class ThreadPool {
public:
    /// Create a thread pool with specified number of workers
    /// @param num_threads Number of worker threads (0 = hardware concurrency)
    explicit ThreadPool(std::size_t num_threads = 0);

    /// Destructor - waits for all tasks to complete
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Submit a task to the pool
    /// @param task The task function to execute
    /// @return Future for the task result
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>> {
        using return_type = typename std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                throw std::runtime_error("Submit on stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }

        cv_.notify_one();
        return result;
    }

    /// Get number of worker threads
    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

    /// Get number of pending tasks
    [[nodiscard]] std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    /// Wait for all pending tasks to complete
    void wait();

private:
    void worker_thread();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;

    std::atomic<bool> stopped_{false};
    std::atomic<std::size_t> active_tasks_{0};
};

}  // namespace internal
}  // namespace falcon
