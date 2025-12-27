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
    return request_group_man_->remove_group(id);
}

void DownloadEngineV2::pause_all() {
    FALCON_LOG_INFO("暂停所有任务");
    // TODO: 遍历所有活动任务并暂停
}

void DownloadEngineV2::resume_all() {
    FALCON_LOG_INFO("恢复所有任务");
    // TODO: 遍历所有暂停任务并恢复
}

void DownloadEngineV2::cancel_all() {
    FALCON_LOG_INFO("取消所有任务");
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
    stats.completed_tasks = completed_count_;
    stats.stopped_tasks = stopped_count_;
    stats.global_download_speed = 0;  // TODO: 计算实际速度
    stats.total_downloaded = total_downloaded_.load();
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
    std::lock_guard<std::mutex> lock(command_queue_mutex_);

    while (!command_queue_.empty()) {
        auto command = std::move(command_queue_.front());
        command_queue_.pop_front();

        // 执行命令
        bool completed = command->execute(this);

        if (!completed) {
            // 命令未完成，放回队列末尾
            command_queue_.push_back(std::move(command));
        }
        // else: 命令已完成，自动销毁
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
    // TODO: 检查任务状态并更新
    // 例如：检查下载完成、失败、超时等情况
}

bool DownloadEngineV2::register_socket_event(int fd, int events, CommandId command_id) {
    std::lock_guard<std::mutex> lock(socket_map_mutex_);

    // 注册到 EventPoll
    auto callback = [this](int socket_fd, int ready_events, void* /*user_data*/) {
        // 查找对应的命令并重新加入队列
        std::lock_guard<std::mutex> lock(command_queue_mutex_);

        // TODO: 根据 command_id 找到对应的命令并重新激活
        // 这里简化处理：假设所有等待事件的命令都会被重新加入队列
        FALCON_LOG_DEBUG("Socket 事件就绪: fd=" << socket_fd << ", events=" << ready_events);
    };

    if (!event_poll_->add_event(fd, events, callback)) {
        FALCON_LOG_ERROR("注册 Socket 事件失败: fd=" << fd);
        return false;
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
