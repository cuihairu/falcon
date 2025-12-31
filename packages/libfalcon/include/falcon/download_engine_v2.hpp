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
#include <falcon/request_group.hpp>
#include <falcon/commands/command.hpp>
#include <falcon/net/event_poll.hpp>
#include <falcon/net/socket_pool.hpp>

#include <memory>
#include <deque>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <unordered_map>

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

    // 成员变量
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
    std::mutex socket_map_mutex_;

    // 状态
    std::atomic<int> halt_requested_{0};
    bool running_ = false;

    // 配置
    EngineConfigV2 config_;
};

} // namespace falcon
