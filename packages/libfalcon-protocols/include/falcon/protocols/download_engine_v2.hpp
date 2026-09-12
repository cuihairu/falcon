/**
 * @file download_engine_v2.hpp
 * @brief 事件驱动的下载引擎 V2 - aria2 风格
 * @author Falcon Team
 * @date 2025-12-24
 *
 * 设计参考: aria2/src/DownloadEngine.h
 * @see https://github.com/aria2/aria2/blob/master/src/DownloadEngine.h
 */

#pragma once

#include <falcon/types.hpp>
#include <falcon/download_options.hpp>
#include <falcon/protocols/request_group.hpp>
#include <falcon/protocols/commands/command.hpp>
#include <falcon/protocols/net/event_poll.hpp>
#include <falcon/protocols/net/socket_pool.hpp>

#include <memory>
#include <string>
#include <deque>
#include <utility>
#include <vector>
#include <map>
#include <atomic>
#include <mutex>
#include <chrono>
#include <unordered_map>

// 前向声明（全局命名空间）：测试夹具需访问 private 成员以驱动命令执行/验证超时清理
class DownloadEngineV2Test;
class HttpCommandsCoverageTest;

namespace falcon {

/**
 * @brief 下载引擎配置 V2
 */
struct EngineConfigV2 {
    std::size_t max_concurrent_tasks = 5;    // 最大并发任务数
    std::size_t global_speed_limit = 0;     // 全局速度限制
    int poll_timeout_ms = 100;              // 事件轮询超时
    bool enable_disk_cache = true;          // 启用磁盘缓存
    std::size_t disk_cache_size = 4 * 1024 * 1024;  // 磁盘缓存大小
    /// 命令进入 waiting_commands_ 后允许的最长等待时长（秒）
    /// 超时后由 cleanup_completed_commands 移除，防止对端异常导致资源泄漏
    int command_wait_timeout_seconds = 120;
    /// 下载临时文件扩展名（temp_extension 消费点）：非空时数据先写
    /// <最终名><扩展名>，任务组完成时原子改名为最终名——半成品不再
    /// 顶着最终名出现；置空则直接写最终名。失败/中断的临时文件保留
    /// 在磁盘（未来断点续传的挂点），最终名文件不受影响
    std::string temp_extension = ".falcon.tmp";
};

/**
 * @brief 增强版下载引擎 - aria2 风格
 *
 * 核心改进：
 * 1. 事件驱动的命令执行循环
 * 2. Socket 连接池管理
 * 3. I/O 多路复用支持
 * 4. 例程命令（后台任务）
 *
 * 对应 aria2 的 DownloadEngine 类
 */
class DownloadEngineV2 {
public:
    /**
     * @brief 构造函数
     *
     * @param config 引擎配置
     */
    explicit DownloadEngineV2(const EngineConfigV2& config = {});

    ~DownloadEngineV2();

    // 允许单元测试夹具直接驱动 private cleanup_completed_commands
    friend class ::DownloadEngineV2Test;
    // 允许 HTTP 命令覆盖测试夹具直接驱动 execute_commands 验证调度结果
    friend class ::HttpCommandsCoverageTest;

    // 禁止拷贝和移动
    DownloadEngineV2(const DownloadEngineV2&) = delete;
    DownloadEngineV2& operator=(const DownloadEngineV2&) = delete;

    /**
     * @brief 启动事件循环（阻塞直到所有任务完成）
     */
    void run();

    /**
     * @brief 添加下载任务
     *
     * @param url URL 或 URL 列表
     * @param options 下载选项
     * @return 任务 ID
     */
    TaskId add_download(const std::string& url, const DownloadOptions& options = {});
    TaskId add_download(const std::vector<std::string>& urls, const DownloadOptions& options = {});

    /**
     * @brief 暂停任务
     */
    bool pause_task(TaskId id);

    /**
     * @brief 恢复任务
     */
    bool resume_task(TaskId id);

    /**
     * @brief 取消任务
     */
    bool cancel_task(TaskId id);

    /**
     * @brief 暂停所有任务
     */
    void pause_all();

    /**
     * @brief 恢复所有任务
     */
    void resume_all();

    /**
     * @brief 取消所有任务
     */
    void cancel_all();

    /**
     * @brief 添加命令到执行队列
     */
    void add_command(std::unique_ptr<Command> command);

    /**
     * @brief 添加例程命令（周期性执行的后台任务）
     */
    void add_routine_command(std::unique_ptr<Command> command);

    /**
     * @brief 获取 EventPoll 实例
     */
    net::EventPoll* event_poll() noexcept { return event_poll_.get(); }

    /**
     * @brief 获取请求组管理器
     */
    RequestGroupMan* request_group_man() noexcept { return request_group_man_.get(); }

    /**
     * @brief 获取引擎配置（只读）
     *
     * 命令据此读取引擎级配置（如磁盘写缓冲 enable_disk_cache/
     * disk_cache_size），任务选项无法承载引擎级参数
     */
    const EngineConfigV2& config() const noexcept { return config_; }

    /**
     * @brief 获取 Socket 连接池
     */
    net::SocketPool* socket_pool() noexcept { return socket_pool_.get(); }

    /**
     * @brief 注册 Socket 事件
     *
     * @param fd Socket 文件描述符
     * @param events 事件类型 (READ/WRITE/ERROR)
     * @param command_id 关联的命令 ID
     */
    bool register_socket_event(int fd, int events, CommandId command_id);

    /**
     * @brief 取消注册 Socket 事件
     */
    bool unregister_socket_event(int fd);

    /**
     * @brief 请求关闭（优雅关闭）
     */
    void shutdown() { halt_requested_ = 1; }

    /**
     * @brief 强制关闭
     */
    void force_shutdown() { halt_requested_ = 2; }

    /**
     * @brief 检查是否关闭请求
     */
    bool is_shutdown_requested() const {
        return halt_requested_.load() > 0;
    }

    /**
     * @brief 检查是否强制关闭
     */
    bool is_force_shutdown_requested() const {
        return halt_requested_.load() >= 2;
    }

    /**
     * @brief 获取全局统计信息
     */
    struct Statistics {
        std::size_t active_tasks = 0;
        std::size_t waiting_tasks = 0;
        std::size_t completed_tasks = 0;
        std::size_t stopped_tasks = 0;
        Speed global_download_speed = 0;
        Bytes total_downloaded = 0;
    };
    Statistics get_statistics() const;

    /**
     * @brief 设置全局下载限速（bytes/s，0 = 不限速），运行时可调
     *
     * 引擎按滑动窗口统计全局接收速率，超限时拉长事件轮询等待节流
     * （与 aria2 的 SpeedCalc/整体节流思路一致）
     */
    void set_global_speed_limit(std::uint64_t bytes_per_second);

    /// 当前生效的全局限速（bytes/s，0 = 不限速）
    std::uint64_t get_global_speed_limit() const noexcept;

    /**
     * @brief 命令报告本轮接收字节数（限速统计入口）
     *
     * 下载命令在每次成功接收后调用；引擎同时记入全局窗口（总体
     * 限速）与该任务窗口（单任务限速），达限后在事件循环中节流
     */
    void report_downloaded_bytes(TaskId task_id, Bytes n);

    /**
     * @brief 当前接收预算（字节）：min(全局限速, 任务限速) − 窗口占用
     *
     * 下载命令在每次 recv 前查询并截断单次读取量；预算为 0 表示
     * 本秒配额已用完，应挂起等待（引擎节流期不再读数据）。
     * task_limit 为该任务的 options.speed_limit（0 = 任务不限速，
     * 仅受全局限速约束）。不限速时返回 UINT64_MAX。
     * 仅引擎线程调用（与窗口统计同线程）
     */
    std::uint64_t recv_budget(TaskId task_id, std::uint64_t task_limit) noexcept;

private:
    /**
     * @brief 执行命令队列
     */
    void execute_commands();

    /**
     * @brief 执行例程命令
     */
    void execute_routine_commands();

    /**
     * @brief 处理就绪事件
     */
    void process_ready_events();

    /**
     * @brief 清理已完成的命令
     */
    void cleanup_completed_commands();

    /**
     * @brief 更新任务状态
     */
    void update_task_status();

    /// 命令执行异常的兜底：把命令所属任务组置为 FAILED（含多段组的
    /// finish_segment 收尾），保证引擎级异常边界有确定的任务可见结果
    void fail_group_of_command(TaskId task_id, const std::string& reason);

    /// Socket 事件就绪处理（register_socket_event 回调的实际逻辑，
    /// 提取为方法以便在回调中整体 try/catch 兜底）
    void handle_socket_ready(int socket_fd, int ready_events);

    /// 淘汰滑动窗口（最近 1s）外的速率样本
    void prune_speed_window(std::chrono::steady_clock::time_point now);

    /// 窗口占用达到限值时评估节流截止时间：等足够多的旧样本满 1s 龄
    /// 淘汰、接收预算恢复为止（与 recv_budget 的预算语义一致）
    void evaluate_throttle(std::chrono::steady_clock::time_point now);

    /// 淘汰单个任务窗口的过期样本；样本清空后返回 true（条目可删）
    bool prune_task_window(TaskId task_id,
                           std::chrono::steady_clock::time_point now);

    /// 移除终态/不存在任务的窗口条目（防 map 随历史任务无限增长）
    void prune_finished_task_windows();

    /// 窗口预算恢复时间点：最早一个使剩余样本和 < limit 的样本满龄
    /// 时刻（ts + 1s + 1ms）；未达限返回 epoch（无节流语义）
    static std::chrono::steady_clock::time_point window_recovery_point(
        const std::deque<std::pair<std::chrono::steady_clock::time_point, Bytes>>& samples,
        Bytes total, std::uint64_t limit);

    /// 停机排水：关闭命令队列与挂起命令持有的 socket fd。
    /// 命令析构 = default 不关 fd；运行期 fd 由超时清理收口，
    /// shutdown/异常停机退出 run() 时统一在此排水，防泄漏
    void drain_command_fds();

    /// 成员变量
    std::unique_ptr<net::EventPoll> event_poll_;
    std::unique_ptr<RequestGroupMan> request_group_man_;
    std::unique_ptr<net::SocketPool> socket_pool_;

    // 命令队列
    std::deque<std::unique_ptr<Command>> command_queue_;
    std::mutex command_queue_mutex_;

    // 例程命令队列
    std::vector<std::unique_ptr<Command>> routine_commands_;
    std::mutex routine_commands_mutex_;

    // Socket 事件注册
    std::map<int, CommandId> socket_command_map_;
    struct SocketWait {
        int fd = -1;
        int events = 0;
    };
    std::unordered_map<CommandId, SocketWait> socket_wait_map_;
    std::unordered_map<CommandId, std::unique_ptr<Command>> waiting_commands_;
    /// sidecar：记录每个 waiting command 进入等待状态的时间点
    /// 用于 cleanup_completed_commands 中的超时清理
    std::unordered_map<CommandId, std::chrono::steady_clock::time_point> waiting_command_times_;
    std::mutex socket_map_mutex_;

    // 状态
    std::atomic<int> halt_requested_{0};
    bool running_ = false;

    // 全局限速：limit 原子供其他线程热更；统计仅引擎线程访问（单线程事件循环）
    std::atomic<std::uint64_t> global_speed_limit_{0};
    /// 滑动窗口样本 (时间点, 字节数)，统计最近 1s 全局接收速率
    std::deque<std::pair<std::chrono::steady_clock::time_point, Bytes>> speed_samples_;
    Bytes speed_window_bytes_ = 0;
    /// 限速节流剩余等待（receive_data 让出后累计，poll 时消费）
    std::chrono::steady_clock::time_point throttle_until_{};

    /// 单任务限速：每任务一个 1s 滑动窗口（多连接分段共享本任务窗口）
    struct TaskSpeedWindow {
        std::deque<std::pair<std::chrono::steady_clock::time_point, Bytes>> samples;
        Bytes total = 0;
    };
    std::unordered_map<TaskId, TaskSpeedWindow> task_windows_;
    /// 任务级节流：本轮 poll 最长等到该时间点（最早的任务预算恢复点），
    /// 仅拉长等待、不跳过 execute_commands（其他任务照常执行）
    std::chrono::steady_clock::time_point task_throttle_until_{};

    // 配置
    EngineConfigV2 config_;
};

} // namespace falcon
