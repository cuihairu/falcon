#pragma once

#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/protocol_handler.hpp>
#include <falcon/types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace falcon {

// Forward declarations
class TaskManager;
class PluginManager;
class EventDispatcher;

/// Main download engine class
class DownloadEngine {
public:
    /// Create a download engine with default configuration
    DownloadEngine();

    /// Create a download engine with custom configuration
    explicit DownloadEngine(const EngineConfig& config);

    /// Destructor
    ~DownloadEngine();

    // Non-copyable, non-movable
    DownloadEngine(const DownloadEngine&) = delete;
    DownloadEngine& operator=(const DownloadEngine&) = delete;
    DownloadEngine(DownloadEngine&&) = delete;
    DownloadEngine& operator=(DownloadEngine&&) = delete;

    // === Task Management ===

    /// Add a new download task
    /// @param url Download URL
    /// @param options Download options (optional)
    /// @return Task pointer, or nullptr if URL is not supported
    [[nodiscard]] DownloadTask::Ptr add_task(const std::string& url,
                                              const DownloadOptions& options = {});

    /// Add multiple download tasks
    /// @param urls List of URLs
    /// @param options Common download options
    /// @return List of task pointers
    [[nodiscard]] std::vector<DownloadTask::Ptr> add_tasks(
        const std::vector<std::string>& urls,
        const DownloadOptions& options = {});

    /// Get task by ID
    /// @param id Task ID
    /// @return Task pointer, or nullptr if not found
    [[nodiscard]] DownloadTask::Ptr get_task(TaskId id) const;

    /// Get all tasks
    [[nodiscard]] std::vector<DownloadTask::Ptr> get_all_tasks() const;

    /// Get tasks by status
    [[nodiscard]] std::vector<DownloadTask::Ptr> get_tasks_by_status(
        TaskStatus status) const;

    /// Get active tasks (downloading or preparing)
    [[nodiscard]] std::vector<DownloadTask::Ptr> get_active_tasks() const;

    /// Remove a completed/failed/cancelled task
    /// @param id Task ID
    /// @return true if removed successfully
    bool remove_task(TaskId id);

    /// Remove all finished tasks
    /// @return Number of tasks removed
    std::size_t remove_finished_tasks();

    // === Task Control ===

    /// Start a pending task
    /// @param id Task ID
    /// @return true if started successfully
    bool start_task(TaskId id);

    /// Pause a downloading task
    /// @param id Task ID
    /// @return true if paused successfully
    bool pause_task(TaskId id);

    /// Resume a paused task
    /// @param id Task ID
    /// @return true if resumed successfully
    bool resume_task(TaskId id);

    /// Cancel a task
    /// @param id Task ID
    /// @return true if cancelled successfully
    bool cancel_task(TaskId id);

    /// Pause all active tasks
    void pause_all();

    /// Resume all paused tasks
    void resume_all();

    /// Cancel all tasks
    void cancel_all();

    /// Wait for all tasks to complete
    void wait_all();

    // === Plugin Management ===

    /// Register a protocol handler
    /// @param handler Protocol handler instance
    void register_handler(std::unique_ptr<IProtocolHandler> handler);

    /// Register a protocol handler factory
    /// @param factory Factory function
    void register_handler_factory(ProtocolHandlerFactory factory);

    /// Get list of supported protocols
    [[nodiscard]] std::vector<std::string> get_supported_protocols() const;

    /// Check if a URL is supported
    [[nodiscard]] bool is_url_supported(const std::string& url) const;

    // === Event Handling ===

    /// Add an event listener
    /// @param listener Listener pointer (engine does not take ownership)
    void add_listener(IEventListener* listener);

    /// Remove an event listener
    /// @param listener Listener pointer
    void remove_listener(IEventListener* listener);

    // === Configuration ===

    /// Get current configuration
    [[nodiscard]] const EngineConfig& config() const noexcept;

    /// Set global speed limit
    /// @param bytes_per_second Speed limit (0 = unlimited)
    void set_global_speed_limit(BytesPerSecond bytes_per_second);

    /// Set maximum concurrent tasks
    /// @param max_tasks Maximum number
    void set_max_concurrent_tasks(std::size_t max_tasks);

    // === Statistics ===

    /// Get total download speed
    [[nodiscard]] BytesPerSecond get_total_speed() const;

    /// Get number of active tasks
    [[nodiscard]] std::size_t get_active_task_count() const;

    /// Get total number of tasks
    [[nodiscard]] std::size_t get_total_task_count() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace falcon
