/**
 * @file download_engine_v2.cpp
 * @brief 事件驱动的下载引擎实现
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/detail/injection.hpp>
#include <falcon/logger.hpp>
#include <falcon/protocols/commands/command.hpp>
#include <falcon/protocols/commands/http_commands.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#ifndef _WIN32
#include <unistd.h>  // close
#else
#include <winsock2.h>
#include <windows.h>
#endif

namespace falcon {

namespace {

/// 关闭 socket fd（命令对象析构不关 fd，fd 所有权随命令流转，
/// 超时清理等引擎侧销毁路径必须显式关闭）
void close_socket_fd(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

} // namespace

namespace {
// 自动分配与显式注入（add_download_as）共用一个 ID 计数器：注入侧
// 把计数器推到注入 ID 之上，自动分配不再撞上外部（V1 契约）占用的 ID
std::atomic<TaskId> g_task_id_counter{1};

/// 暂停清扫命令：任务组暂停后由 pause_task 投递，引擎线程执行时
/// 收走该任务挂起中的连接（挂起命令不经 execute，execute 入口的
/// PAUSED 守卫覆盖不到它们），关闭 fd 并销毁
class HttpPauseSweepCommand : public AbstractCommand {
public:
    explicit HttpPauseSweepCommand(TaskId task_id)
        : AbstractCommand(task_id) {}

    bool execute(DownloadEngineV2* engine) override {
        if (engine) {
            engine->sweep_task_connections(get_task_id());
        }
        return handle_result(ExecutionResult::OK);
    }

    const char* name() const override { return "HttpPauseSweepCommand"; }
};
} // namespace

#ifdef _WIN32
namespace {
// 引擎数据面直接使用 Winsock,不依赖调用方先经其他组件初始化——
// daemon 碰巧由 RPC 层构造时的 WSAStartup 覆盖,CLI/桌面进程内引擎
// 没有该路径,缺初始化时 socket() 全部报 WSA 10093。构造时幂等初始
// 化,进程生命周期内不清理(进程退出自动回收,与 daemon RPC 层做法
// 一致)
void ensure_winsock_initialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
} // namespace
#endif

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
#ifdef _WIN32
    // 先于任何 socket 操作(成员初始化不涉 winsock,下载路径均在
    // run()/add_download 之后)
    ensure_winsock_initialized();
#endif
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
    TaskId id = g_task_id_counter.fetch_add(1, std::memory_order_relaxed);

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

TaskId DownloadEngineV2::add_download_as(TaskId id,
                                          const std::vector<std::string>& urls,
                                          const DownloadOptions& options,
                                          const std::string& output_path_override) {
    if (urls.empty()) {
        FALCON_LOG_WARN_STREAM("尝试注入空下载 URL 列表: id=" << id);
        return INVALID_TASK_ID;
    }

    // 把共享 ID 计数器推到 bound 之上：该 ID 已被外部占用（即将归属
    // 本组），自动分配必须跳过它，不再撞上外部注入的 ID
    auto push_counter_above = [](TaskId bound) {
        TaskId current = g_task_id_counter.load(std::memory_order_relaxed);
        while (current <= bound &&
               !g_task_id_counter.compare_exchange_weak(current, bound + 1,
                                                        std::memory_order_relaxed)) {
        }
    };

    // ID 冲突：不创建组
    if (request_group_man_->find_group(id) != nullptr) {
        FALCON_LOG_WARN_STREAM("注入下载任务 ID 冲突: id=" << id);
        push_counter_above(id);
        return INVALID_TASK_ID;
    }
    push_counter_above(id);

    FALCON_LOG_INFO_STREAM("注入下载任务: id=" << id << ", url=" << urls[0]);

    auto group = std::make_unique<RequestGroup>(id, urls, options, output_path_override);
    request_group_man_->add_request_group(std::move(group));

    if (running_) {
        request_group_man_->fill_request_group_from_reserver(this);
    }

    return id;
}

bool DownloadEngineV2::pause_task(TaskId id) {
    FALCON_LOG_INFO_STREAM("暂停任务: id=" << id);
    if (!request_group_man_->pause_group(id)) {
        return false;
    }
    // 投递清扫：收走该任务挂起中的连接。execute 入口的 PAUSED 守卫
    // 只覆盖还在执行队列里的命令，挂起等事件的命令由清扫收口
    add_command(std::make_unique<HttpPauseSweepCommand>(id));
    return true;
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

std::uint64_t DownloadEngineV2::recv_budget(TaskId task_id,
                                            std::uint64_t task_limit) noexcept {
    constexpr auto kUnlimited = std::numeric_limits<std::uint64_t>::max();

    const auto limit = global_speed_limit_.load(std::memory_order_relaxed);
    std::uint64_t budget = kUnlimited;

    // 全局预算
    if (limit > 0) {
        budget = speed_window_bytes_ >= limit ? 0 : limit - speed_window_bytes_;
    }

    // 任务预算（取严）
    if (task_limit > 0) {
        std::uint64_t task_budget = kUnlimited;
        // 查询时同步淘汰过期样本：预算耗尽挂起期间没有新 report，
        // 任务窗口只能靠查询方的淘汰恢复，否则预算永不恢复（死锁）
        const auto now = std::chrono::steady_clock::now();
        prune_task_window(task_id, now);
        auto it = task_windows_.find(task_id);
        if (it != task_windows_.end() && it->second.total >= task_limit) {
            task_budget = 0;
            // 记录本轮 poll 最长等待点：睡到预算恢复，抑制空轮询。
            // 直接赋值（不合并）：过期恢复点判定 now < until 不成立，
            // 自然退化为常规 poll 节奏，无正确性影响
            const auto rp = window_recovery_point(
                it->second.samples, it->second.total, task_limit);
            if (rp != std::chrono::steady_clock::time_point{}) {
                task_throttle_until_ = rp;
            }
        } else if (it != task_windows_.end()) {
            task_budget = task_limit - it->second.total;
        }
        budget = std::min(budget, task_budget);
    }

    return budget;
}

void DownloadEngineV2::report_downloaded_bytes(TaskId task_id, Bytes n) {
    if (n == 0) return;
    const auto now = std::chrono::steady_clock::now();

    // 全局窗口
    speed_samples_.emplace_back(now, n);
    speed_window_bytes_ += n;
    prune_speed_window(now);

    // 任务窗口（多连接分段共享同一任务窗口，先到先得自然分摊）
    auto& win = task_windows_[task_id];
    win.samples.emplace_back(now, n);
    win.total += n;
    prune_task_window(task_id, now);
}

void DownloadEngineV2::prune_speed_window(std::chrono::steady_clock::time_point now) {
    const auto cutoff = now - std::chrono::seconds(1);
    while (!speed_samples_.empty() && speed_samples_.front().first < cutoff) {
        speed_window_bytes_ -= speed_samples_.front().second;
        speed_samples_.pop_front();
    }
}

bool DownloadEngineV2::prune_task_window(TaskId task_id,
                                         std::chrono::steady_clock::time_point now) {
    auto it = task_windows_.find(task_id);
    if (it == task_windows_.end()) {
        return true;
    }
    const auto cutoff = now - std::chrono::seconds(1);
    auto& win = it->second;
    while (!win.samples.empty() && win.samples.front().first < cutoff) {
        win.total -= win.samples.front().second;
        win.samples.pop_front();
    }
    if (win.samples.empty()) {
        task_windows_.erase(it);
        return true;
    }
    return false;
}

void DownloadEngineV2::prune_finished_task_windows() {
    for (auto it = task_windows_.begin(); it != task_windows_.end();) {
        const auto* group = request_group_man_ ? request_group_man_->find_group(it->first)
                                               : nullptr;
        const bool finished = !group ||
                              group->status() == RequestGroupStatus::COMPLETED ||
                              group->status() == RequestGroupStatus::FAILED ||
                              group->status() == RequestGroupStatus::REMOVED;
        it = finished ? task_windows_.erase(it) : std::next(it);
    }
}

std::chrono::steady_clock::time_point DownloadEngineV2::window_recovery_point(
    const std::deque<std::pair<std::chrono::steady_clock::time_point, Bytes>>& samples,
    Bytes total, std::uint64_t limit) {
    if (limit == 0 || total < limit || samples.empty()) {
        return {};  // 未达限：无节流语义
    }
    // 找最早的 k 使去掉前 k 个样本后剩余和 < limit——第 k 个样本满龄
    // （时间戳 + 1s）时窗口恰好降到 limit 以下、预算恢复
    Bytes suffix = total;
    std::size_t k = 0;
    while (k < samples.size() && suffix >= limit) {
        suffix -= samples[k].second;
        ++k;
    }
    // k == size() 时全部样本满龄后窗口归零，取末样本锚点
    const auto anchor = k < samples.size() ? samples[k].first : samples.back().first;
    // +1ms：prune 用严格小于判定，样本恰满 1s 时还未淘汰
    return anchor + std::chrono::seconds(1) + std::chrono::milliseconds(1);
}

void DownloadEngineV2::evaluate_throttle(std::chrono::steady_clock::time_point now) {
    prune_speed_window(now);
    const auto limit = global_speed_limit_.load(std::memory_order_relaxed);
    if (limit == 0 || speed_window_bytes_ < limit) {
        return;  // 窗口未达限：不节流
    }
    const auto until = window_recovery_point(speed_samples_, speed_window_bytes_, limit);
    if (until != std::chrono::steady_clock::time_point{} && until > throttle_until_) {
        throttle_until_ = until;
    }
}

void DownloadEngineV2::run() {
    FALCON_LOG_INFO_STREAM("启动下载引擎事件循环");

    running_ = true;
    halt_requested_ = 0;
    // 周期回收从本轮 run() 起算（成员默认值为时钟纪元，直接用会让
    // 首轮循环立即触发 purge）
    last_group_purge_ = std::chrono::steady_clock::now();

    // 初始化：从等待队列激活初始任务
    request_group_man_->fill_request_group_from_reserver(this);

    // 主事件循环
    while (!is_shutdown_requested()) {
        try {
            // 异常注入闸门（仅测试构建生效）：覆盖循环体顶层兜底
            // catch——单命令与例程命令异常已有内层收口，这两处
            // 兜底只在异常逃出内层边界时可达
            if (::falcon::detail::inject_failure(
                    ::falcon::detail::InjectPoint::EngineLoopThrowStd)) {
                throw std::runtime_error("注入: 事件循环异常");
            }
            if (::falcon::detail::inject_failure(
                    ::falcon::detail::InjectPoint::EngineLoopThrowNonStd)) {
                throw 42;  // 非 std 异常形态
            }

            // 检查是否所有任务完成（wait_when_idle 时引擎常驻，
            // 只随显式 shutdown 退出）
            if (!config_.wait_when_idle && request_group_man_->all_completed()) {
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

            // 任务级限速节流：只拉长本轮 poll 等待（睡到最早的任务预算
            // 恢复点，抑制预算耗尽的空轮询），不跳过 execute_commands
            // ——单任务限速不能拖累其他任务的命令执行
            if (!throttled) {
                const auto now = std::chrono::steady_clock::now();
                if (now < task_throttle_until_) {
                    const auto remain_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            task_throttle_until_ - now)
                            .count();
                    poll_timeout_ms = std::min(
                        poll_timeout_ms,
                        static_cast<int>(std::clamp<long long>(remain_ms, 1, 1000)));
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

            // 清理终态任务的限速窗口条目
            prune_finished_task_windows();

            // 周期回收终态组：引擎常驻（wait_when_idle）后终态组
            // 不再随 run() 退出销毁，不回收则组表无界增长
            const auto now_for_purge = std::chrono::steady_clock::now();
            if (now_for_purge - last_group_purge_ >= std::chrono::seconds(10)) {
                last_group_purge_ = now_for_purge;
                request_group_man_->purge_finished_groups();
            }

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

    // 停机排水：run() 退出（shutdown/force/全任务终态/异常 break）时
    // 统一关闭命令队列与挂起命令持有的 fd。运行期挂起 fd 由超时清理
    // 收口；此处的排水覆盖"引擎停了但命令还攥着 fd"的剩余窗口
    //（对端黑洞任务在默认 120s 兜底超时到达前停机即属此列）
    drain_command_fds();

    FALCON_LOG_INFO_STREAM("下载引擎事件循环结束");
}

void DownloadEngineV2::drain_command_fds() {
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(socket_map_mutex_);
        for (auto& entry : waiting_commands_) {
            const int fd = entry.second ? entry.second->socket_fd() : -1;
            if (fd >= 0) {
                fds.push_back(fd);
            }
        }
        waiting_commands_.clear();
        waiting_command_times_.clear();
        socket_wait_map_.clear();
        socket_command_map_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(command_queue_mutex_);
        for (auto& command : command_queue_) {
            const int fd = command ? command->socket_fd() : -1;
            if (fd >= 0) {
                fds.push_back(fd);
            }
        }
        command_queue_.clear();
    }

    for (const int fd : fds) {
        // EventPoll::poll 在 run() 线程内同步派发回调，此处循环已退出，
        // 无并发回调；remove_event 防止已关闭 fd 残留在监听集
        if (event_poll_) {
            event_poll_->remove_event(fd);
        }
        close_socket_fd(fd);
    }
    if (!fds.empty()) {
        FALCON_LOG_INFO_STREAM("停机排水: 关闭 " << fds.size()
                              << " 个命令持有的 socket fd");
    }
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
    // 清理三类资源：
    // 1) waiting_commands_ 中超时未恢复的命令（对端异常断开、EventPoll
    //    one-shot 丢失、对端黑洞不响应等）
    // 2) 关联的 socket 事件注册与 fd 本身（命令对象析构不关 fd）
    // 3) 所属任务组的终态：超时前组停在 ACTIVE，不标 FAILED 会让任务
    //    悬空在 Downloading、all_completed 永不成立、run() 无法退出
    if (config_.command_wait_timeout_seconds <= 0) {
        return;  // 超时清理已禁用
    }

    const auto now = std::chrono::steady_clock::now();

    // 第一遍扫描：在锁内收集全部等待中命令的 {command_id, fd, task_id, 挂起时间}，
    // 避免在锁内执行回调或 IO
    struct WaitEntry {
        CommandId cmd_id;
        int fd;
        TaskId task_id;
        std::chrono::steady_clock::time_point parked_at;
    };
    std::vector<WaitEntry> waiting;
    std::vector<WaitEntry> expired;

    {
        std::lock_guard<std::mutex> lock(socket_map_mutex_);
        if (waiting_command_times_.empty()) {
            return;
        }

        waiting.reserve(waiting_command_times_.size());
        for (const auto& [cmd_id, ts] : waiting_command_times_) {
            int fd = -1;
            TaskId task_id = 0;
            auto w_it = socket_wait_map_.find(cmd_id);
            if (w_it != socket_wait_map_.end()) {
                fd = w_it->second.fd;
            }
            auto c_it = waiting_commands_.find(cmd_id);
            if (c_it != waiting_commands_.end() && c_it->second) {
                task_id = c_it->second->get_task_id();
            }
            waiting.push_back(WaitEntry{cmd_id, fd, task_id, ts});
        }
    }

    // 锁外：按任务超时阈值筛选超时者——任务的 options.timeout_seconds
    // 显式设置（>0）时优先生效，未设置回落引擎全局兜底值
    for (const auto& entry : waiting) {
        std::size_t threshold = static_cast<std::size_t>(config_.command_wait_timeout_seconds);
        if (entry.task_id != 0) {
            if (auto* group = request_group_man_->find_group(entry.task_id)) {
                if (group->options().timeout_seconds > 0) {
                    threshold = group->options().timeout_seconds;
                }
            }
        }
        if (now - entry.parked_at >= std::chrono::seconds(threshold)) {
            expired.push_back(entry);
        }
    }

    if (expired.empty()) {
        return;
    }

    // 第二遍：锁内从所有映射中移除，并取出命令所有权以便锁外处理
    struct Dropped {
        WaitEntry entry;
        std::unique_ptr<Command> command;
    };
    std::vector<Dropped> dropped;
    {
        std::lock_guard<std::mutex> lock(socket_map_mutex_);
        for (const auto& entry : expired) {
            Dropped d;
            d.entry = entry;
            auto w_it = waiting_commands_.find(entry.cmd_id);
            if (w_it != waiting_commands_.end()) {
                d.command = std::move(w_it->second);
                waiting_commands_.erase(w_it);
            }
            waiting_command_times_.erase(entry.cmd_id);
            socket_wait_map_.erase(entry.cmd_id);
            if (entry.fd >= 0) {
                socket_command_map_.erase(entry.fd);
            }
            dropped.push_back(std::move(d));
        }
    }

    // 锁外：摘除 EventPoll 监听、关闭 fd（平台 IO 调用不应持锁），
    // 然后处理落盘状态与组终态
    for (auto& d : dropped) {
        if (d.entry.fd >= 0) {
            event_poll_->remove_event(d.entry.fd);
            close_socket_fd(d.entry.fd);
        }
        if (d.command) {
            // 先冲刷写缓冲并上报落盘进度（此前只在暂停清扫路径执行，
            // 析构冲刷发生在 fail 记账之后会造成断点滞后），随后段
            // 命令先试段级换源重试——成功则该段由重试链接管，不因
            // 单段超时连坐整组；预算耗尽/无段上下文才标组 FAILED
            d.command->prepare_sweep(this);
            if (d.command->retry_expired_segment(this)) {
                FALCON_LOG_INFO_STREAM("超时命令所在段已调度换源重试: cmd="
                                      << d.entry.cmd_id
                                      << ", task=" << d.entry.task_id);
                continue;
            }
        }
        fail_group_of_command(d.entry.task_id, "download wait timeout");
        FALCON_LOG_WARN_STREAM("等待中的命令超时被清理: cmd=" << d.entry.cmd_id
                              << ", task=" << d.entry.task_id);
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
    // 状态守卫：暂停/移除/已完成组不被超时清理等路径误杀——暂停组
    // 的挂起连接正被清扫收走，竞态窗口内到达的失败路径不得把非失败
    // 语义改写成 FAILED
    const auto st = group->status();
    if (st == RequestGroupStatus::PAUSED || st == RequestGroupStatus::REMOVED ||
        st == RequestGroupStatus::COMPLETED) {
        return;
    }
    if (group->is_multi_segment()) {
        group->finish_segment(false);
    }
    group->save_resume_now();  // 续传追踪中则固化断点（超时清理等收口路径）
    group->set_error_message(reason);
    group->set_status(RequestGroupStatus::FAILED);
}

void DownloadEngineV2::sweep_task_connections(TaskId task_id) {
    struct SweptEntry {
        CommandId cmd_id;
        int fd;
        std::unique_ptr<Command> command;
    };
    std::vector<SweptEntry> swept;
    {
        std::lock_guard<std::mutex> lock(socket_map_mutex_);
        for (auto it = waiting_commands_.begin();
             it != waiting_commands_.end();) {
            if (!it->second || it->second->get_task_id() != task_id) {
                ++it;
                continue;
            }
            SweptEntry entry;
            entry.cmd_id = it->first;
            entry.fd = it->second->socket_fd();
            entry.command = std::move(it->second);
            swept.push_back(std::move(entry));
            it = waiting_commands_.erase(it);
        }
        for (const auto& entry : swept) {
            waiting_command_times_.erase(entry.cmd_id);
            socket_wait_map_.erase(entry.cmd_id);
            if (entry.fd >= 0) {
                auto scm = socket_command_map_.find(entry.fd);
                if (scm != socket_command_map_.end() &&
                    scm->second == entry.cmd_id) {
                    socket_command_map_.erase(scm);
                }
            }
        }
    }

    // 锁外：摘除事件监听并关闭 fd（平台 IO 调用不应持锁），随后给
    // 命令检查点机会（下载命令冲刷残留缓冲并上报断点），最后销毁
    for (auto& entry : swept) {
        if (entry.fd >= 0) {
            event_poll_->remove_event(entry.fd);
            close_socket_fd(entry.fd);
        }
        if (entry.command) {
            entry.command->prepare_sweep(this);
        }
    }
    // swept 在作用域结束时销毁命令对象

    if (!swept.empty()) {
        FALCON_LOG_INFO_STREAM("暂停清扫: task=" << task_id
                              << ", 收走挂起连接 " << swept.size() << " 条");
    }
}

void DownloadEngineV2::handle_socket_ready(int socket_fd, int ready_events) {
    // 异常注入闸门（仅测试构建生效）：socket 事件就绪是回环测试必经
    // 路径，注入在事件到达瞬间命中，覆盖回调 lambda 顶层兜底 catch
    if (::falcon::detail::inject_failure(
            ::falcon::detail::InjectPoint::SocketReadyThrowStd)) {
        throw std::runtime_error("注入: socket 回调异常");
    }
    if (::falcon::detail::inject_failure(
            ::falcon::detail::InjectPoint::SocketReadyThrowNonStd)) {
        throw 42;  // 非 std 异常形态
    }
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
        if (::falcon::detail::inject_failure(
                ::falcon::detail::InjectPoint::EventPollAddFail) ||
            !event_poll_->add_event(fd, events, callback)) {
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
