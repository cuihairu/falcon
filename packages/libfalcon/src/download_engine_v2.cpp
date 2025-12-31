/**
 * @file download_engine_v2.cpp
 * @brief 事件驱动的下载引擎实现
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <falcon/download_engine_v2.hpp>
#include <falcon/logger.hpp>
#include <falcon/commands/command.hpp>
#include <falcon/commands/http_commands.hpp>

#include <algorithm>
#include <chrono>

namespace falcon {

//==============================================================================
// DownloadEngineV2 实现
//==============================================================================

DownloadEngineV2::DownloadEngineV2(const EngineConfigV2& config)
    : event_poll_(net::EventPoll::create())
    , request_group_man_(std::make_unique<RequestGroupMan>(config.max_concurrent_tasks))
    , socket_pool_(std::make_unique<net::SocketPool>(
          std::chrono::seconds(30),  // 30秒超时
          16))                        // 最大空闲连接数
    , config_(config)
{
    FALCON_LOG_INFO("创建 DownloadEngineV2");
    FALCON_LOG_INFO("  最大并发任务: " << config.max_concurrent_tasks);
    FALCON_LOG_INFO("  全局限速: " << config.global_speed_limit);
    FALCON_LOG_INFO("  Socket 池超时: 30s, 最大空闲: 16");
}

DownloadEngineV2::~DownloadEngineV2() {
    FALCON_LOG_INFO("销毁 DownloadEngineV2");
}

TaskId DownloadEngineV2::add_download(const std::string& url,
                                        const DownloadOptions& options) {
    std::vector<std::string> urls = {url};
    return add_download(urls, options);
}

TaskId DownloadEngineV2::add_download(const std::vector<std::string>& urls,
                                        const DownloadOptions& options) {
    // 生成新的任务 ID
    static std::atomic<TaskId> id_counter{1};
    TaskId id = id_counter.fetch_add(1, std::memory_order_relaxed);

    FALCON_LOG_INFO("添加下载任务: id=" << id << ", url=" << urls[0]);

    // 创建请求组
    auto group = std::make_unique<RequestGroup>(id, urls, options);

    // 添加到管理器
    request_group_man_->add_request_group(std::move(group));

    // 如果引擎正在运行，尝试从等待队列激活任务
    if (running_) {
        request_group_man_->fill_request_group_from_reserver(this);
    }

    return id;
}

bool DownloadEngineV2::pause_task(TaskId id) {
    FALCON_LOG_INFO("暂停任务: id=" << id);
    return request_group_man_->pause_group(id);
}

bool DownloadEngineV2::resume_task(TaskId id) {
    FALCON_LOG_INFO("恢复任务: id=" << id);
    return request_group_man_->resume_group(id);
}

bool DownloadEngineV2::cancel_task(TaskId id) {
    FALCON_LOG_INFO("取消任务: id=" << id);
    auto* group = request_group_man_->find_group(id);
    if (group) {
        if (auto task = group->download_task()) {
            task->cancel();
        }
    }
    return request_group_man_->remove_group(id);
}

void DownloadEngineV2::pause_all() {
    FALCON_LOG_INFO("暂停所有任务");
    for (const auto& group : request_group_man_->all_groups()) {
        if (group && group->status() == RequestGroupStatus::ACTIVE) {
            pause_task(group->id());
        }
    }
}

void DownloadEngineV2::resume_all() {
    FALCON_LOG_INFO("恢复所有任务");
    for (const auto& group : request_group_man_->all_groups()) {
        if (group && group->status() == RequestGroupStatus::PAUSED) {
            resume_task(group->id());
        }
    }
}

void DownloadEngineV2::cancel_all() {
    FALCON_LOG_INFO("取消所有任务");
    for (const auto& group : request_group_man_->all_groups()) {
        if (group && (group->status() == RequestGroupStatus::ACTIVE ||
                      group->status() == RequestGroupStatus::WAITING ||
                      group->status() == RequestGroupStatus::PAUSED)) {
            cancel_task(group->id());
        }
    }
    shutdown();
}

void DownloadEngineV2::add_command(std::unique_ptr<Command> command) {
    if (!command) {
        FALCON_LOG_WARN("尝试添加空命令");
        return;
    }

    std::lock_guard<std::mutex> lock(command_queue_mutex_);
    command_queue_.push_back(std::move(command));
}

void DownloadEngineV2::add_routine_command(std::unique_ptr<Command> command) {
    if (!command) {
        FALCON_LOG_WARN("尝试添加空例程命令");
        return;
    }

    std::lock_guard<std::mutex> lock(routine_commands_mutex_);
    routine_commands_.push_back(std::move(command));
}

DownloadEngineV2::Statistics DownloadEngineV2::get_statistics() const {
    Statistics stats;
    stats.active_tasks = request_group_man_->active_count();
    stats.waiting_tasks = request_group_man_->waiting_count();

    Speed total_speed = 0;
    Bytes total_downloaded = 0;
    std::size_t completed = 0;
    std::size_t stopped = 0;

    for (const auto& group : request_group_man_->all_groups()) {
        if (!group) continue;
        auto p = group->get_progress();
        total_downloaded += p.downloaded;
        total_speed += p.speed;

        switch (group->status()) {
            case RequestGroupStatus::COMPLETED:
                ++completed;
                break;
            case RequestGroupStatus::FAILED:
            case RequestGroupStatus::REMOVED:
                ++stopped;
                break;
            default:
                break;
        }
    }

    stats.completed_tasks = completed;
    stats.stopped_tasks = stopped;
    stats.global_download_speed = total_speed;
    stats.total_downloaded = total_downloaded;
    return stats;
}

void DownloadEngineV2::run() {
    FALCON_LOG_INFO("启动下载引擎事件循环");

    running_ = true;
    halt_requested_ = 0;

    // 初始化：从等待队列激活初始任务
    request_group_man_->fill_request_group_from_reserver(this);

    // 主事件循环
    while (!is_shutdown_requested()) {
        // 检查是否所有任务完成
        if (request_group_man_->all_completed()) {
            FALCON_LOG_INFO("所有任务已完成");
            break;
        }

        // 执行例程命令
        execute_routine_commands();

        // 等待事件（带超时）
        int events = event_poll_->poll(config_.poll_timeout_ms);

        // 处理就绪事件
        if (events > 0) {
            process_ready_events();
        }

        // 执行命令队列
        execute_commands();

        // 清理已完成的命令
        cleanup_completed_commands();

        // 更新任务状态
        update_task_status();

        // 从等待队列激活新任务
        request_group_man_->fill_request_group_from_reserver(this);
    }

    running_ = false;

    FALCON_LOG_INFO("下载引擎事件循环结束");
}

void DownloadEngineV2::execute_commands() {
    std::size_t round = 0;
    {
        std::lock_guard<std::mutex> lock(command_queue_mutex_);
        round = command_queue_.size();
    }

    for (std::size_t i = 0; i < round; ++i) {
        std::unique_ptr<Command> command;
        {
            std::lock_guard<std::mutex> lock(command_queue_mutex_);
            if (command_queue_.empty()) {
                break;
            }
            command = std::move(command_queue_.front());
            command_queue_.pop_front();
        }

        if (!command) {
            continue;
        }

        // 执行命令（不要持有队列锁，避免阻塞 socket 回调入队）
        bool completed = command->execute(this);

        if (completed) {
            continue;
        }

        // 未完成：若该命令已注册 socket 等待事件，则将其挂起等待回调重新入队
        bool parked = false;
        {
            std::lock_guard<std::mutex> lock(socket_map_mutex_);
            auto it = socket_wait_map_.find(command->id());
            if (it != socket_wait_map_.end()) {
                waiting_commands_[command->id()] = std::move(command);
                parked = true;
            }
        }

        if (!parked) {
            std::lock_guard<std::mutex> lock(command_queue_mutex_);
            command_queue_.push_back(std::move(command));
        }
    }
}

void DownloadEngineV2::execute_routine_commands() {
    std::lock_guard<std::mutex> lock(routine_commands_mutex_);

    for (auto& command : routine_commands_) {
        // 例程命令总是放回队列（周期性执行）
        command->execute(this);
    }
}

void DownloadEngineV2::process_ready_events() {
    // 事件回调会自动触发对应的命令执行
    // 这里只需要等待 EventPoll 的回调被调用
}

void DownloadEngineV2::cleanup_completed_commands() {
    // TODO: 清理已完成的命令
    // 目前命令在完成后自动销毁，不需要额外清理
}

void DownloadEngineV2::update_task_status() {
    request_group_man_->cleanup_finished_active();
}

bool DownloadEngineV2::register_socket_event(int fd, int events, CommandId command_id) {
    std::lock_guard<std::mutex> lock(socket_map_mutex_);

    // 记录等待信息（供 execute_commands() 将命令移出队列）
    socket_wait_map_[command_id] = SocketWait{fd, events};

    auto callback = [this](int socket_fd, int ready_events, void* /*user_data*/) {
        std::unique_ptr<Command> resumed;
        CommandId cmd_id = 0;
        {
            std::lock_guard<std::mutex> lock(socket_map_mutex_);
            auto it = socket_command_map_.find(socket_fd);
            if (it == socket_command_map_.end()) {
                FALCON_LOG_DEBUG("Socket 事件就绪但无关联命令: fd=" << socket_fd);
                return;
            }
            cmd_id = it->second;

            auto w_it = waiting_commands_.find(cmd_id);
            if (w_it != waiting_commands_.end()) {
                resumed = std::move(w_it->second);
                waiting_commands_.erase(w_it);
            }

            socket_wait_map_.erase(cmd_id);
            socket_command_map_.erase(it);
        }

        // one-shot: 事件就绪后移除监听；失败则忽略（平台可能已自动删除）
        event_poll_->remove_event(socket_fd);

        if (!resumed) {
            FALCON_LOG_DEBUG("Socket 事件就绪但命令不存在: fd=" << socket_fd << ", cmd=" << cmd_id);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(command_queue_mutex_);
            command_queue_.push_back(std::move(resumed));
        }

        FALCON_LOG_DEBUG("Socket 事件就绪: fd=" << socket_fd << ", events=" << ready_events
                                              << ", 恢复命令=" << cmd_id);
    };

    // 已存在则修改事件，否则新增事件
    if (socket_command_map_.find(fd) != socket_command_map_.end()) {
        if (!event_poll_->modify_event(fd, events)) {
            FALCON_LOG_ERROR("修改 Socket 事件失败: fd=" << fd);
            return false;
        }
    } else {
        if (!event_poll_->add_event(fd, events, callback)) {
            FALCON_LOG_ERROR("注册 Socket 事件失败: fd=" << fd);
            return false;
        }
    }

    socket_command_map_[fd] = command_id;
    return true;
}

bool DownloadEngineV2::unregister_socket_event(int fd) {
    std::lock_guard<std::mutex> lock(socket_map_mutex_);

    if (!event_poll_->remove_event(fd)) {
        FALCON_LOG_WARN("取消注册 Socket 事件失败: fd=" << fd);
        return false;
    }

    socket_command_map_.erase(fd);
    return true;
}

} // namespace falcon
