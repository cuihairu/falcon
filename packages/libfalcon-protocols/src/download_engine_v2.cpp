/**
 * @file download_engine_v2.cpp
 * @brief 事件驱动的下载引擎实现
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/logger.hpp>
#include <falcon/protocols/commands/command.hpp>
#include <falcon/protocols/commands/http_commands.hpp>

#include <algorithm>
#include <chrono>
#include <limits>

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
    , global_speed_limit_(config.global_speed_limit)
    , config_(config)
{
    FALCON_LOG_INFO_STREAM("创建 DownloadEngineV2");
    FALCON_LOG_INFO_STREAM("  最大并发任务: " << config.max_concurrent_tasks);
    FALCON_LOG_INFO_STREAM("  全局限速: " << config.global_speed_limit);
    FALCON_LOG_INFO_STREAM("  Socket 池超时: 30s, 最大空闲: 16");
}

DownloadEngineV2::~DownloadEngineV2() {
    FALCON_LOG_INFO_STREAM("销毁 DownloadEngineV2");
}

TaskId DownloadEngineV2::add_download(const std::string& url,
                                        const DownloadOptions& options) {
    std::vector<std::string> urls = {url};
    return add_download(urls, options);
}

TaskId DownloadEngineV2::add_download(const std::vector<std::string>& urls,
                                        const DownloadOptions& options) {
    if (urls.empty()) {
        FALCON_LOG_WARN_STREAM("尝试添加空下载 URL 列表");
        return INVALID_TASK_ID;
    }

    // 生成新的任务 ID
    static std::atomic<TaskId> id_counter{1};
    TaskId id = id_counter.fetch_add(1, std::memory_order_relaxed);

    FALCON_LOG_INFO_STREAM("添加下载任务: id=" << id << ", url=" << urls[0]);

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
    FALCON_LOG_INFO_STREAM("暂停任务: id=" << id);
    return request_group_man_->pause_group(id);
}

bool DownloadEngineV2::resume_task(TaskId id) {
    FALCON_LOG_INFO_STREAM("恢复任务: id=" << id);
    return request_group_man_->resume_group(id);
}

bool DownloadEngineV2::cancel_task(TaskId id) {
    FALCON_LOG_INFO_STREAM("取消任务: id=" << id);
    auto* group = request_group_man_->find_group(id);
    if (group) {
        if (auto task = group->download_task()) {
            task->cancel();
        }
    }
    return request_group_man_->remove_group(id);
}

void DownloadEngineV2::pause_all() {
    FALCON_LOG_INFO_STREAM("暂停所有任务");
    for (const auto& group : request_group_man_->all_groups()) {
        if (group && group->status() == RequestGroupStatus::ACTIVE) {
            pause_task(group->id());
        }
    }
}

void DownloadEngineV2::resume_all() {
    FALCON_LOG_INFO_STREAM("恢复所有任务");
    for (const auto& group : request_group_man_->all_groups()) {
        if (group && group->status() == RequestGroupStatus::PAUSED) {
            resume_task(group->id());
        }
    }
}

void DownloadEngineV2::cancel_all() {
    FALCON_LOG_INFO_STREAM("取消所有任务");
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
        FALCON_LOG_WARN_STREAM("尝试添加空命令");
        return;
    }

    std::lock_guard<std::mutex> lock(command_queue_mutex_);
    command_queue_.push_back(std::move(command));
}

void DownloadEngineV2::add_routine_command(std::unique_ptr<Command> command) {
    if (!command) {
        FALCON_LOG_WARN_STREAM("尝试添加空例程命令");
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

void DownloadEngineV2::set_global_speed_limit(std::uint64_t bytes_per_second) {
    global_speed_limit_.store(bytes_per_second, std::memory_order_relaxed);
    // 变更立即生效：清空节流截止，下轮重新评估（提速能马上恢复预算）
    throttle_until_ = {};
    if (bytes_per_second > 0) {
        FALCON_LOG_INFO_STREAM("全局限速: " << bytes_per_second << " bytes/s");
    } else {
        FALCON_LOG_INFO_STREAM("全局限速: 取消");
    }
}

std::uint64_t DownloadEngineV2::get_global_speed_limit() const noexcept {
    return global_speed_limit_.load(std::memory_order_relaxed);
}

std::uint64_t DownloadEngineV2::recv_budget() const noexcept {
    const auto limit = global_speed_limit_.load(std::memory_order_relaxed);
    if (limit == 0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return speed_window_bytes_ >= limit ? 0 : limit - speed_window_bytes_;
}

void DownloadEngineV2::report_downloaded_bytes(Bytes n) {
    if (n == 0) return;
    const auto now = std::chrono::steady_clock::now();
    speed_samples_.emplace_back(now, n);
    speed_window_bytes_ += n;
    prune_speed_window(now);
}

void DownloadEngineV2::prune_speed_window(std::chrono::steady_clock::time_point now) {
    const auto cutoff = now - std::chrono::seconds(1);
    while (!speed_samples_.empty() && speed_samples_.front().first < cutoff) {
        speed_window_bytes_ -= speed_samples_.front().second;
        speed_samples_.pop_front();
    }
}

void DownloadEngineV2::evaluate_throttle(std::chrono::steady_clock::time_point now) {
    prune_speed_window(now);
    const auto limit = global_speed_limit_.load(std::memory_order_relaxed);
    if (limit == 0 || speed_window_bytes_ < limit) {
        return;  // 窗口未达限：不节流
    }
    // 窗口达限：等到足够多的旧样本满 1s 龄淘汰、接收预算恢复为止。
    // 找最早的 k 使去掉前 k 个样本后剩余和 < limit——第 k 个样本满龄
    // （时间戳 + 1s）时窗口恰好降到 limit 以下
    Bytes suffix = speed_window_bytes_;
    std::size_t k = 0;
    while (k < speed_samples_.size() && suffix >= limit) {
        suffix -= speed_samples_[k].second;
        ++k;
    }
    // k == size() 时全部样本满龄后窗口归零（suffix < limit），取末样本锚点
    const auto anchor = k < speed_samples_.size()
                            ? speed_samples_[k].first
                            : speed_samples_.back().first;
    // +1ms：prune 用严格小于判定，样本恰满 1s 时还未淘汰
    const auto until =
        anchor + std::chrono::seconds(1) + std::chrono::milliseconds(1);
    if (until > throttle_until_) {
        throttle_until_ = until;
    }
}

void DownloadEngineV2::run() {
    FALCON_LOG_INFO_STREAM("启动下载引擎事件循环");

    running_ = true;
    halt_requested_ = 0;

    // 初始化：从等待队列激活初始任务
    request_group_man_->fill_request_group_from_reserver(this);

    // 主事件循环
    while (!is_shutdown_requested()) {
        try {
            // 检查是否所有任务完成
            if (request_group_man_->all_completed()) {
                FALCON_LOG_INFO_STREAM("所有任务已完成");
                break;
            }

            // 全局限速节流：超速时拉长本轮事件等待并跳过数据面命令，
            // 让 socket 事件照常处理（回调只入队，不丢事件）
            bool throttled = false;
            int poll_timeout_ms = config_.poll_timeout_ms;
            if (global_speed_limit_.load(std::memory_order_relaxed) > 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now < throttle_until_) {
                    throttled = true;
                } else {
                    evaluate_throttle(now);
                    throttled = now < throttle_until_;
                }
                if (throttled) {
                    const auto remain_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            throttle_until_ - std::chrono::steady_clock::now())
                            .count();
                    poll_timeout_ms = static_cast<int>(
                        std::clamp<long long>(remain_ms, 1, 1000));
                }
            }

            // 执行例程命令
            execute_routine_commands();

            // 等待事件（带超时）
            int events = event_poll_->poll(poll_timeout_ms);

            // 处理就绪事件
            if (events > 0) {
                process_ready_events();
            }

            // 执行命令队列（节流轮跳过，抑制接收速率）
            if (!throttled) {
                execute_commands();
            }

            // 清理已完成的命令
            cleanup_completed_commands();

            // 更新任务状态
            update_task_status();

            // 从等待队列激活新任务
            request_group_man_->fill_request_group_from_reserver(this);
        } catch (const std::exception& e) {
            // 引擎通常运行在独立线程（测试/宿主直接以 run() 作线程函数），
            // 异常逃逸即 std::terminate——这里兜底保证线程正常收尾
            FALCON_LOG_ERROR_STREAM("引擎事件循环异常，安全停机: " << e.what());
            break;
        } catch (...) {
            FALCON_LOG_ERROR_STREAM("引擎事件循环未知异常，安全停机");
            break;
        }
    }

    running_ = false;

    FALCON_LOG_INFO_STREAM("下载引擎事件循环结束");
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

        // 执行命令（不要持有队列锁，避免阻塞 socket 回调入队）。
        // 异常边界：单个命令抛出只终结其所属任务，引擎继续运行
        bool completed = false;
        try {
            completed = command->execute(this);
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR_STREAM("命令执行异常: cmd=" << command->name()
                                  << ", task=" << command->get_task_id()
                                  << ", error=" << e.what());
            fail_group_of_command(command->get_task_id(),
                                  std::string("command exception: ") + e.what());
            continue;
        } catch (...) {
            FALCON_LOG_ERROR_STREAM("命令执行异常: cmd=" << command->name()
                                  << ", task=" << command->get_task_id()
                                  << ", error=unknown");
            fail_group_of_command(command->get_task_id(),
                                  "command exception: unknown");
            continue;
        }

        if (completed) {
            continue;
        }

        // 未完成：若该命令已注册 socket 等待事件，则将其挂起等待回调重新入队
        bool parked = false;
        {
            std::lock_guard<std::mutex> lock(socket_map_mutex_);
            const CommandId cmd_id = command->id();
            auto it = socket_wait_map_.find(cmd_id);
            if (it != socket_wait_map_.end()) {
                waiting_commands_[cmd_id] = std::move(command);
                waiting_command_times_[cmd_id] = std::chrono::steady_clock::now();
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
        // 例程命令总是放回队列（周期性执行）；异常只跳过本轮，
        // 不终止引擎（例程命令是全局性的，无对应任务可标失败）
        try {
            command->execute(this);
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR_STREAM("例程命令异常: cmd=" << command->name()
                                  << ", error=" << e.what());
        } catch (...) {
            FALCON_LOG_ERROR_STREAM("例程命令异常: cmd=" << command->name()
                                  << ", error=unknown");
        }
    }
}

void DownloadEngineV2::process_ready_events() {
    // 事件回调会自动触发对应的命令执行
    // 这里只需要等待 EventPoll 的回调被调用
}

void DownloadEngineV2::cleanup_completed_commands() {
    // 清理两类资源：
    // 1) waiting_commands_ 中超时未恢复的命令（对端异常断开、EventPoll one-shot 丢失等）
    // 2) 关联的 socket 事件注册，避免 fd 资源泄漏
    if (config_.command_wait_timeout_seconds <= 0) {
        return;  // 超时清理已禁用
    }

    const auto now = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(config_.command_wait_timeout_seconds);

    // 第一遍扫描：在锁内收集超时命令 ID 与对应 fd，避免在锁内执行回调或 IO
    std::vector<std::pair<CommandId, int>> expired;  // {command_id, fd}
    std::vector<std::unique_ptr<Command>> dropped;

    {
        std::lock_guard<std::mutex> lock(socket_map_mutex_);
        if (waiting_command_times_.empty()) {
            return;
        }

        expired.reserve(waiting_command_times_.size());
        for (const auto& [cmd_id, ts] : waiting_command_times_) {
            if (now - ts >= timeout) {
                int fd = -1;
                auto w_it = socket_wait_map_.find(cmd_id);
                if (w_it != socket_wait_map_.end()) {
                    fd = w_it->second.fd;
                }
                expired.emplace_back(cmd_id, fd);
            }
        }

        if (expired.empty()) {
            return;
        }

        // 第二遍：从所有映射中移除，并取出命令所有权以便锁外销毁
        for (const auto& [cmd_id, fd] : expired) {
            auto w_it = waiting_commands_.find(cmd_id);
            if (w_it != waiting_commands_.end()) {
                dropped.push_back(std::move(w_it->second));
                waiting_commands_.erase(w_it);
            }
            waiting_command_times_.erase(cmd_id);
            socket_wait_map_.erase(cmd_id);
            if (fd >= 0) {
                socket_command_map_.erase(fd);
            }
        }
    }

    // 锁外：移除 EventPoll 监听（平台 IO 调用不应持锁），记录日志
    for (const auto& [cmd_id, fd] : expired) {
        if (fd >= 0) {
            event_poll_->remove_event(fd);
        }
        FALCON_LOG_WARN_STREAM("等待中的命令超时被清理: cmd=" << cmd_id
                              << ", timeout=" << config_.command_wait_timeout_seconds << "s");
    }
    // dropped 在作用域结束时自动销毁命令对象
}

void DownloadEngineV2::update_task_status() {
    request_group_man_->cleanup_finished_active();
}

void DownloadEngineV2::fail_group_of_command(TaskId task_id, const std::string& reason) {
    auto* group = request_group_man_ ? request_group_man_->find_group(task_id) : nullptr;
    if (!group) {
        return;
    }
    if (group->is_multi_segment()) {
        group->finish_segment(false);
    }
    group->set_error_message(reason);
    group->set_status(RequestGroupStatus::FAILED);
}

void DownloadEngineV2::handle_socket_ready(int socket_fd, int ready_events) {
    std::unique_ptr<Command> resumed;
    CommandId cmd_id = 0;
    {
        std::lock_guard<std::mutex> lock(socket_map_mutex_);
        auto it = socket_command_map_.find(socket_fd);
        if (it == socket_command_map_.end()) {
            FALCON_LOG_DEBUG_STREAM("Socket 事件就绪但无关联命令: fd=" << socket_fd);
            return;
        }
        cmd_id = it->second;

        auto w_it = waiting_commands_.find(cmd_id);
        if (w_it != waiting_commands_.end()) {
            resumed = std::move(w_it->second);
            waiting_commands_.erase(w_it);
            waiting_command_times_.erase(cmd_id);
        }

        socket_wait_map_.erase(cmd_id);
        socket_command_map_.erase(it);
    }

    // one-shot: 事件就绪后移除监听；失败则忽略（平台可能已自动删除）
    event_poll_->remove_event(socket_fd);

    if (!resumed) {
        FALCON_LOG_DEBUG_STREAM("Socket 事件就绪但命令不存在: fd=" << socket_fd << ", cmd=" << cmd_id);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(command_queue_mutex_);
        command_queue_.push_back(std::move(resumed));
    }

    FALCON_LOG_DEBUG_STREAM("Socket 事件就绪: fd=" << socket_fd << ", events=" << ready_events
                                          << ", 恢复命令=" << cmd_id);
}

bool DownloadEngineV2::register_socket_event(int fd, int events, CommandId command_id) {
    std::lock_guard<std::mutex> lock(socket_map_mutex_);

    // 记录等待信息（供 execute_commands() 将命令移出队列）
    socket_wait_map_[command_id] = SocketWait{fd, events};

    auto callback = [this](int socket_fd, int ready_events, void* /*user_data*/) {
        // 回调在 EventPoll 线程上下文执行，异常逃逸即 std::terminate，
        // 因此整体兜底：回调失败只丢失这一次唤醒（超时清理会回收挂起命令）
        try {
            handle_socket_ready(socket_fd, ready_events);
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR_STREAM("Socket 事件回调异常: fd=" << socket_fd
                                  << ", error=" << e.what());
        } catch (...) {
            FALCON_LOG_ERROR_STREAM("Socket 事件回调异常: fd=" << socket_fd
                                  << ", error=unknown");
        }
    };

    // 已存在则修改事件，否则新增事件
    if (socket_command_map_.find(fd) != socket_command_map_.end()) {
        if (!event_poll_->modify_event(fd, events)) {
            FALCON_LOG_ERROR_STREAM("修改 Socket 事件失败: fd=" << fd);
            return false;
        }
    } else {
        if (!event_poll_->add_event(fd, events, callback)) {
            FALCON_LOG_ERROR_STREAM("注册 Socket 事件失败: fd=" << fd);
            return false;
        }
    }

    socket_command_map_[fd] = command_id;
    return true;
}

bool DownloadEngineV2::unregister_socket_event(int fd) {
    std::lock_guard<std::mutex> lock(socket_map_mutex_);

    if (!event_poll_->remove_event(fd)) {
        FALCON_LOG_WARN_STREAM("取消注册 Socket 事件失败: fd=" << fd);
        return false;
    }

    socket_command_map_.erase(fd);
    return true;
}

} // namespace falcon
