/**
 * @file download_engine.cpp
 * @brief 下载引擎实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/download_engine.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>
#include <falcon/task_manager.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/plugin_manager.hpp>
#include <algorithm>
#include <filesystem>

// 临时日志宏
#define LOG_ERROR(msg) falcon::log_error(msg)

namespace falcon {

/**
 * @brief 下载引擎实现类
 */
class DownloadEngine::Impl {
public:
    explicit Impl(const EngineConfig& config)
        : config_(config)
        , task_manager_([&] {
              TaskManagerConfig tm;
              tm.max_concurrent_tasks = std::max<std::size_t>(1, config_.max_concurrent_tasks);
              return tm;
          }(),
          &event_dispatcher_)
        , global_speed_limiter_(0)
        , next_task_id_(1) {

        // 启动事件分发器
        event_dispatcher_.start();

        // 启动任务管理器
        task_manager_.start();
    }

    ~Impl() {
        // 停止所有任务
        task_manager_.cancel_all();
        task_manager_.stop();

        // 停止事件分发器
        event_dispatcher_.stop();
    }

    DownloadTask::Ptr add_task(const std::string& url,
                               const DownloadOptions& options) {
        if (url.empty()) {
            throw InvalidURLException("Empty URL");
        }

        // 查找协议处理器
        auto* handler = plugin_manager_.getPluginByUrl(url);
        if (!handler) {
            throw UnsupportedProtocolException("No handler for URL: " + url);
        }

        // 创建任务
        TaskId id = next_task_id_++;
        auto task = std::make_shared<DownloadTask>(id, url, options);

        // 设置协议处理器
        task->set_handler(std::shared_ptr<IProtocolHandler>(handler, [](IProtocolHandler*) {}));

        // 生成输出路径
        const bool has_custom_dir =
            !options.output_directory.empty() && options.output_directory != ".";
        std::filesystem::path out_dir =
            has_custom_dir ? std::filesystem::path(options.output_directory) : std::filesystem::path();

        std::filesystem::path out_path;
        if (!options.output_filename.empty()) {
            std::filesystem::path filename(options.output_filename);
            if (has_custom_dir && filename.is_relative()) {
                out_path = out_dir / filename;
            } else {
                out_path = filename;
            }
        } else {
            // No filename specified: derive from URL.
            std::string filename = "download";
            auto pos = url.rfind('/');
            if (pos != std::string::npos && pos + 1 < url.size()) {
                filename = url.substr(pos + 1);
                // Remove query string if present
                auto query_pos = filename.find('?');
                if (query_pos != std::string::npos) {
                    filename = filename.substr(0, query_pos);
                }
                if (filename.empty()) {
                    filename = "download";
                }
            }
            out_path = has_custom_dir ? (out_dir / filename) : std::filesystem::path(filename);
        }

        if (options.create_directory) {
            std::error_code ec;
            auto parent = out_path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    throw FileIOException("Failed to create output directory: " + ec.message());
                }
            }
        }

        task->set_output_path(out_path.string());

        // 添加到任务管理器
        task_manager_.add_task(task, TaskPriority::Normal);

        return task;
    }

    std::vector<DownloadTask::Ptr> add_tasks(
        const std::vector<std::string>& urls,
        const DownloadOptions& options) {

        std::vector<DownloadTask::Ptr> tasks;
        tasks.reserve(urls.size());

        for (const auto& url : urls) {
            try {
                auto task = add_task(url, options);
                if (task) {
                    tasks.push_back(task);
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to add task for URL: " + url + " Error: " + e.what());
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

    bool remove_task(TaskId id) {
        return task_manager_.remove_task(id);
    }

    size_t remove_finished_tasks() {
        return task_manager_.cleanup_finished_tasks();
    }

    bool start_task(TaskId id) {
        return task_manager_.start_task(id);
    }

    bool pause_task(TaskId id) {
        return task_manager_.pause_task(id);
    }

    bool resume_task(TaskId id) {
        return task_manager_.resume_task(id);
    }

    bool cancel_task(TaskId id) {
        return task_manager_.cancel_task(id);
    }

    void pause_all() {
        task_manager_.pause_all();
    }

    void resume_all() {
        task_manager_.resume_all();
    }

    void cancel_all() {
        task_manager_.cancel_all();
    }

    void wait_all() {
        task_manager_.wait_all();
    }

    void register_handler(std::unique_ptr<IProtocolHandler> handler) {
        if (handler) {
            plugin_manager_.registerPlugin(std::move(handler));
        }
    }

    void register_handler_factory(ProtocolHandlerFactory factory) {
        handler_factories_.push_back(factory);
    }

    std::vector<std::string> get_supported_protocols() const {
        return plugin_manager_.getSupportedProtocols();
    }

    bool is_url_supported(const std::string& url) const {
        return plugin_manager_.supportsUrl(url);
    }

    void add_listener(IEventListener* listener) {
        event_dispatcher_.add_listener(listener);
    }

    void remove_listener(IEventListener* listener) {
        event_dispatcher_.remove_listener(listener);
    }

    const EngineConfig& config() const noexcept {
        return config_;
    }

    void set_global_speed_limit(BytesPerSecond bytes_per_second) {
        global_speed_limiter_ = bytes_per_second;
        // TODO: 通知所有任务调整速度
    }

    void set_max_concurrent_tasks(std::size_t max_tasks) {
        task_manager_.set_max_concurrent_tasks(max_tasks);
    }

    BytesPerSecond get_total_speed() const {
        auto stats = task_manager_.get_statistics();
        return stats.total_speed;
    }

    std::size_t get_active_task_count() const {
        return task_manager_.get_active_task_count();
    }

    std::size_t get_total_task_count() const {
        auto stats = task_manager_.get_statistics();
        return stats.total_tasks;
    }

    void load_all_plugins() {
        plugin_manager_.loadAllPlugins();

        // 加载工厂函数创建的插件
        for (auto& factory : handler_factories_) {
            if (factory) {
                auto handler = factory();
                if (handler) {
                    plugin_manager_.registerPlugin(std::move(handler));
                }
            }
        }
    }

private:
    EngineConfig config_;
    PluginManager plugin_manager_;
    EventDispatcher event_dispatcher_;
    TaskManager task_manager_;

    std::vector<ProtocolHandlerFactory> handler_factories_;
    std::atomic<BytesPerSecond> global_speed_limiter_;
    std::atomic<TaskId> next_task_id_;
};

// DownloadEngine 实现
DownloadEngine::DownloadEngine()
    : impl_(std::make_unique<Impl>(EngineConfig{})) {
    impl_->load_all_plugins();
}

DownloadEngine::DownloadEngine(const EngineConfig& config)
    : impl_(std::make_unique<Impl>(config)) {
    impl_->load_all_plugins();
}

DownloadEngine::~DownloadEngine() = default;

DownloadTask::Ptr DownloadEngine::add_task(const std::string& url,
                                            const DownloadOptions& options) {
    return impl_->add_task(url, options);
}

std::vector<DownloadTask::Ptr> DownloadEngine::add_tasks(
    const std::vector<std::string>& urls,
    const DownloadOptions& options) {
    return impl_->add_tasks(urls, options);
}

DownloadTask::Ptr DownloadEngine::get_task(TaskId id) const {
    return impl_->get_task(id);
}

std::vector<DownloadTask::Ptr> DownloadEngine::get_all_tasks() const {
    return impl_->get_all_tasks();
}

std::vector<DownloadTask::Ptr> DownloadEngine::get_tasks_by_status(TaskStatus status) const {
    return impl_->get_tasks_by_status(status);
}

std::vector<DownloadTask::Ptr> DownloadEngine::get_active_tasks() const {
    return impl_->get_active_tasks();
}

bool DownloadEngine::remove_task(TaskId id) {
    return impl_->remove_task(id);
}

size_t DownloadEngine::remove_finished_tasks() {
    return impl_->remove_finished_tasks();
}

bool DownloadEngine::start_task(TaskId id) {
    return impl_->start_task(id);
}

bool DownloadEngine::pause_task(TaskId id) {
    return impl_->pause_task(id);
}

bool DownloadEngine::resume_task(TaskId id) {
    return impl_->resume_task(id);
}

bool DownloadEngine::cancel_task(TaskId id) {
    return impl_->cancel_task(id);
}

void DownloadEngine::pause_all() {
    impl_->pause_all();
}

void DownloadEngine::resume_all() {
    impl_->resume_all();
}

void DownloadEngine::cancel_all() {
    impl_->cancel_all();
}

void DownloadEngine::wait_all() {
    impl_->wait_all();
}

void DownloadEngine::register_handler(std::unique_ptr<IProtocolHandler> handler) {
    impl_->register_handler(std::move(handler));
}

void DownloadEngine::register_handler_factory(ProtocolHandlerFactory factory) {
    impl_->register_handler_factory(factory);
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

} // namespace falcon
