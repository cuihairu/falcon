// Falcon Download Engine - Core Implementation
// Copyright (c) 2025 Falcon Project

#include <falcon/download_engine.hpp>
#include <falcon/exceptions.hpp>

#include "internal/event_dispatcher.hpp"
#include "internal/plugin_manager.hpp"
#include "internal/task_manager.hpp"
#include "internal/thread_pool.hpp"

#include <filesystem>

namespace falcon {

/// Implementation details for DownloadEngine
class DownloadEngine::Impl {
public:
    explicit Impl(EngineConfig config)
        : config_(std::move(config)),
          task_manager_(config_.max_concurrent_tasks),
          thread_pool_(config_.max_concurrent_tasks) {}

    ~Impl() {
        // Cancel all active tasks
        auto tasks = task_manager_.get_active_tasks();
        for (auto& task : tasks) {
            task->cancel();
        }
        thread_pool_.wait();
    }

    DownloadTask::Ptr add_task(const std::string& url,
                                const DownloadOptions& options) {
        // Validate URL
        if (!internal::UrlUtils::is_valid_url(url)) {
            throw InvalidURLException(url);
        }

        // Find handler
        auto* handler = plugin_manager_.find_handler(url);
        if (!handler) {
            throw UnsupportedProtocolException(
                internal::UrlUtils::extract_scheme(url));
        }

        // Create task
        TaskId id = task_manager_.next_id();
        auto task = std::make_shared<DownloadTask>(id, url, options);

        // Set up output path
        std::string output_dir = options.output_directory;
        if (output_dir.empty()) {
            output_dir = ".";
        }

        std::string filename = options.output_filename;
        if (filename.empty()) {
            filename = internal::UrlUtils::extract_filename(url);
        }

        std::filesystem::path output_path =
            std::filesystem::path(output_dir) / filename;

        // Create directory if needed
        if (options.create_directory) {
            std::filesystem::create_directories(output_path.parent_path());
        }

        task->set_output_path(output_path.string());
        task->set_listener(&event_dispatcher_);

        // Add to manager
        task_manager_.add_task(task);

        // Auto-start if configured
        if (config_.auto_start && task_manager_.can_start_more()) {
            start_task_internal(task, handler);
        }

        return task;
    }

    std::vector<DownloadTask::Ptr> add_tasks(
        const std::vector<std::string>& urls, const DownloadOptions& options) {
        std::vector<DownloadTask::Ptr> tasks;
        tasks.reserve(urls.size());
        for (const auto& url : urls) {
            try {
                tasks.push_back(add_task(url, options));
            } catch (const FalconException&) {
                // Skip invalid URLs
            }
        }
        return tasks;
    }

    DownloadTask::Ptr get_task(TaskId id) const {
        return task_manager_.get_task(id);
    }

    std::vector<DownloadTask::Ptr> get_all_tasks() const {
        return task_manager_.get_all_tasks();
    }

    std::vector<DownloadTask::Ptr> get_tasks_by_status(TaskStatus status) const {
        return task_manager_.get_tasks_by_status(status);
    }

    std::vector<DownloadTask::Ptr> get_active_tasks() const {
        return task_manager_.get_active_tasks();
    }

    bool remove_task(TaskId id) { return task_manager_.remove_task(id); }

    std::size_t remove_finished_tasks() { return task_manager_.remove_finished(); }

    bool start_task(TaskId id) {
        auto task = task_manager_.get_task(id);
        if (!task || task->status() != TaskStatus::Pending) {
            return false;
        }

        auto* handler = plugin_manager_.find_handler(task->url());
        if (!handler) {
            return false;
        }

        start_task_internal(task, handler);
        return true;
    }

    bool pause_task(TaskId id) {
        auto task = task_manager_.get_task(id);
        if (!task) {
            return false;
        }
        return task->pause();
    }

    bool resume_task(TaskId id) {
        auto task = task_manager_.get_task(id);
        if (!task) {
            return false;
        }
        return task->resume();
    }

    bool cancel_task(TaskId id) {
        auto task = task_manager_.get_task(id);
        if (!task) {
            return false;
        }
        return task->cancel();
    }

    void pause_all() {
        auto tasks = task_manager_.get_active_tasks();
        for (auto& task : tasks) {
            task->pause();
        }
    }

    void resume_all() {
        auto tasks = task_manager_.get_tasks_by_status(TaskStatus::Paused);
        for (auto& task : tasks) {
            task->resume();
        }
    }

    void cancel_all() {
        auto tasks = task_manager_.get_all_tasks();
        for (auto& task : tasks) {
            if (!task->is_finished()) {
                task->cancel();
            }
        }
    }

    void wait_all() { task_manager_.wait_all(); }

    void register_handler(std::unique_ptr<IProtocolHandler> handler) {
        plugin_manager_.register_handler(std::move(handler));
    }

    std::vector<std::string> get_supported_protocols() const {
        return plugin_manager_.get_protocols();
    }

    bool is_url_supported(const std::string& url) const {
        return plugin_manager_.is_supported(url);
    }

    void add_listener(IEventListener* listener) {
        event_dispatcher_.add_listener(listener);
    }

    void remove_listener(IEventListener* listener) {
        event_dispatcher_.remove_listener(listener);
    }

    const EngineConfig& config() const noexcept { return config_; }

    void set_global_speed_limit(BytesPerSecond bytes_per_second) {
        config_.global_speed_limit = bytes_per_second;
    }

    void set_max_concurrent_tasks(std::size_t max_tasks) {
        config_.max_concurrent_tasks = max_tasks;
        task_manager_.set_max_concurrent(max_tasks);
    }

    BytesPerSecond get_total_speed() const {
        BytesPerSecond total = 0;
        auto tasks = task_manager_.get_active_tasks();
        for (const auto& task : tasks) {
            total += task->speed();
        }
        return total;
    }

    std::size_t get_active_task_count() const {
        return task_manager_.active_count();
    }

    std::size_t get_total_task_count() const {
        return task_manager_.total_count();
    }

private:
    void start_task_internal(DownloadTask::Ptr task, IProtocolHandler* handler) {
        task->set_status(TaskStatus::Preparing);
        task->mark_started();

        // Submit to thread pool
        thread_pool_.submit([this, task, handler]() {
            try {
                // Get file info first
                auto info =
                    handler->get_file_info(task->url(), task->options());
                task->set_file_info(info);

                // Check if file exists and resume
                auto output_path = task->output_path();
                std::filesystem::path temp_path =
                    output_path + config_.temp_extension;

                if (task->options().resume_enabled &&
                    std::filesystem::exists(temp_path)) {
                    auto existing_size = std::filesystem::file_size(temp_path);
                    task->update_progress(existing_size, info.total_size, 0);
                }

                // Start download
                task->set_status(TaskStatus::Downloading);
                handler->download(task, &event_dispatcher_);

                // Check completion
                if (task->status() == TaskStatus::Downloading) {
                    // Rename temp file to final
                    if (std::filesystem::exists(temp_path)) {
                        std::filesystem::rename(temp_path, output_path);
                    }

                    task->set_status(TaskStatus::Completed);
                    event_dispatcher_.on_completed(task->id(), output_path);
                }
            } catch (const std::exception& e) {
                task->set_error(e.what());
                task->set_status(TaskStatus::Failed);
            }

            // Try to start next pending task
            try_start_next();
        });
    }

    void try_start_next() {
        if (!config_.auto_start || !task_manager_.can_start_more()) {
            return;
        }

        auto next = task_manager_.get_next_pending();
        if (next) {
            auto* handler = plugin_manager_.find_handler(next->url());
            if (handler) {
                start_task_internal(next, handler);
            }
        }
    }

    EngineConfig config_;
    internal::TaskManager task_manager_;
    internal::PluginManager plugin_manager_;
    internal::EventDispatcher event_dispatcher_;
    internal::ThreadPool thread_pool_;
};

// DownloadEngine public API implementation

DownloadEngine::DownloadEngine() : impl_(std::make_unique<Impl>(EngineConfig{})) {}

DownloadEngine::DownloadEngine(const EngineConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

DownloadEngine::~DownloadEngine() = default;

DownloadTask::Ptr DownloadEngine::add_task(const std::string& url,
                                            const DownloadOptions& options) {
    return impl_->add_task(url, options);
}

std::vector<DownloadTask::Ptr> DownloadEngine::add_tasks(
    const std::vector<std::string>& urls, const DownloadOptions& options) {
    return impl_->add_tasks(urls, options);
}

DownloadTask::Ptr DownloadEngine::get_task(TaskId id) const {
    return impl_->get_task(id);
}

std::vector<DownloadTask::Ptr> DownloadEngine::get_all_tasks() const {
    return impl_->get_all_tasks();
}

std::vector<DownloadTask::Ptr> DownloadEngine::get_tasks_by_status(
    TaskStatus status) const {
    return impl_->get_tasks_by_status(status);
}

std::vector<DownloadTask::Ptr> DownloadEngine::get_active_tasks() const {
    return impl_->get_active_tasks();
}

bool DownloadEngine::remove_task(TaskId id) { return impl_->remove_task(id); }

std::size_t DownloadEngine::remove_finished_tasks() {
    return impl_->remove_finished_tasks();
}

bool DownloadEngine::start_task(TaskId id) { return impl_->start_task(id); }

bool DownloadEngine::pause_task(TaskId id) { return impl_->pause_task(id); }

bool DownloadEngine::resume_task(TaskId id) { return impl_->resume_task(id); }

bool DownloadEngine::cancel_task(TaskId id) { return impl_->cancel_task(id); }

void DownloadEngine::pause_all() { impl_->pause_all(); }

void DownloadEngine::resume_all() { impl_->resume_all(); }

void DownloadEngine::cancel_all() { impl_->cancel_all(); }

void DownloadEngine::wait_all() { impl_->wait_all(); }

void DownloadEngine::register_handler(std::unique_ptr<IProtocolHandler> handler) {
    impl_->register_handler(std::move(handler));
}

void DownloadEngine::register_handler_factory(ProtocolHandlerFactory factory) {
    if (factory) {
        impl_->register_handler(factory());
    }
}

std::vector<std::string> DownloadEngine::get_supported_protocols() const {
    return impl_->get_supported_protocols();
}

bool DownloadEngine::is_url_supported(const std::string& url) const {
    return impl_->is_url_supported(url);
}

void DownloadEngine::add_listener(IEventListener* listener) {
    impl_->add_listener(listener);
}

void DownloadEngine::remove_listener(IEventListener* listener) {
    impl_->remove_listener(listener);
}

const EngineConfig& DownloadEngine::config() const noexcept {
    return impl_->config();
}

void DownloadEngine::set_global_speed_limit(BytesPerSecond bytes_per_second) {
    impl_->set_global_speed_limit(bytes_per_second);
}

void DownloadEngine::set_max_concurrent_tasks(std::size_t max_tasks) {
    impl_->set_max_concurrent_tasks(max_tasks);
}

BytesPerSecond DownloadEngine::get_total_speed() const {
    return impl_->get_total_speed();
}

std::size_t DownloadEngine::get_active_task_count() const {
    return impl_->get_active_task_count();
}

std::size_t DownloadEngine::get_total_task_count() const {
    return impl_->get_total_task_count();
}

}  // namespace falcon
