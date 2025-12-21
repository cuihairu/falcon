/**
 * @file task_manager.hpp
 * @brief 任务管理器接口
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/types.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>

#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <queue>

namespace falcon {

// Forward declarations
class EventDispatcher;

/**
 * @brief 任务优先级
 */
enum class TaskPriority : int {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3
};

/**
 * @brief 任务队列项
 */
struct TaskQueueItem {
    TaskId task_id;
    TaskPriority priority;
    std::chrono::steady_clock::time_point schedule_time;

    // 优先级队列比较函数
    bool operator<(const TaskQueueItem& other) const {
        // 优先级高的先执行
        if (priority != other.priority) {
            return priority > other.priority;
        }
        // 相同优先级按时间排序
        return schedule_time < other.schedule_time;
    }
};

/**
 * @brief 任务管理器配置
 */
struct TaskManagerConfig {
    size_t max_concurrent_tasks = 5;        ///< 最大并发任务数
    size_t max_queue_size = 1000;           ///< 最大队列大小
    bool enable_task_priority = true;       ///< 启用任务优先级
    std::chrono::seconds task_timeout{3600}; ///< 任务超时时间（1小时）
    std::chrono::seconds cleanup_interval{60}; ///< 清理间隔
    bool auto_save_state = true;            ///< 自动保存任务状态
    std::string state_file;                 ///< 状态文件路径
};

/**
 * @brief 任务管理器
 *
 * 负责管理所有下载任务的生命周期，包括：
 * - 任务队列管理
 * - 并发控制
 * - 任务调度
 * - 状态持久化
 * - 任务恢复
 */
class TaskManager {
public:
    /**
     * @brief 构造函数
     * @param config 配置选项
     * @param event_dispatcher 事件分发器
     */
    explicit TaskManager(const TaskManagerConfig& config = {},
                         EventDispatcher* event_dispatcher = nullptr);

    /**
     * @brief 析构函数
     */
    ~TaskManager();

    // Non-copyable, non-movable
    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;
    TaskManager(TaskManager&&) = delete;
    TaskManager& operator=(TaskManager&&) = delete;

    // === 任务管理 ===

    /**
     * @brief 添加任务
     * @param task 任务指针
     * @param priority 任务优先级
     * @return 任务ID
     */
    TaskId add_task(DownloadTask::Ptr task,
                    TaskPriority priority = TaskPriority::Normal);

    /**
     * @brief 启动任务
     * @param id 任务ID
     * @return 是否成功启动
     */
    bool start_task(TaskId id);

    /**
     * @brief 启动任务（带优先级）
     * @param id 任务ID
     * @param priority 任务优先级
     * @return 是否成功启动
     */
    bool start_task(TaskId id, TaskPriority priority);

    /**
     * @brief 获取任务
     * @param id 任务ID
     * @return 任务指针，如果不存在返回nullptr
     */
    DownloadTask::Ptr get_task(TaskId id) const;

    /**
     * @brief 获取所有任务
     * @return 任务列表
     */
    std::vector<DownloadTask::Ptr> get_all_tasks() const;

    /**
     * @brief 根据状态获取任务
     * @param status 任务状态
     * @return 任务列表
     */
    std::vector<DownloadTask::Ptr> get_tasks_by_status(TaskStatus status) const;

    /**
     * @brief 获取活跃任务
     * @return 活跃任务列表
     */
    std::vector<DownloadTask::Ptr> get_active_tasks() const;

    /**
     * @brief 移除任务
     * @param id 任务ID
     * @return 是否成功移除
     */
    bool remove_task(TaskId id);

    /**
     * @brief 清理已完成的任务
     * @return 清理的任务数量
     */
    size_t cleanup_finished_tasks();

    // === 任务控制 ===

    /**
     * @brief 启动任务
     * @param id 任务ID
     * @return 是否成功启动
     */
    bool start_task(TaskId id);

    /**
     * @brief 暂停任务
     * @param id 任务ID
     * @return 是否成功暂停
     */
    bool pause_task(TaskId id);

    /**
     * @brief 恢复任务
     * @param id 任务ID
     * @return 是否成功恢复
     */
    bool resume_task(TaskId id);

    /**
     * @brief 取消任务
     * @param id 任务ID
     * @return 是否成功取消
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
     * @brief 等待所有任务完成
     * @param timeout 超时时间
     * @return true if all tasks completed, false if timeout
     */
    bool wait_all(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

    // === 队列管理 ===

    /**
     * @brief 获取队列大小
     * @return 队列中的任务数量
     */
    size_t get_queue_size() const;

    /**
     * @brief 获取活跃任务数量
     * @return 正在下载的任务数量
     */
    size_t get_active_task_count() const;

    /**
     * @brief 获取最大并发数
     * @return 最大并发任务数
     */
    size_t get_max_concurrent_tasks() const;

    /**
     * @brief 设置最大并发数
     * @param max_tasks 最大任务数
     */
    void set_max_concurrent_tasks(size_t max_tasks);

    /**
     * @brief 调整任务优先级
     * @param id 任务ID
     * @param priority 新优先级
     * @return 是否成功调整
     */
    bool adjust_task_priority(TaskId id, TaskPriority priority);

    // === 统计信息 ===

    /**
     * @brief 获取总体统计
     * @return 统计信息
     */
    struct Statistics {
        size_t total_tasks = 0;          ///< 总任务数
        size_t pending_tasks = 0;        ///< 等待中的任务
        size_t downloading_tasks = 0;    ///< 下载中的任务
        size_t paused_tasks = 0;         ///< 已暂停的任务
        size_t completed_tasks = 0;      ///< 已完成的任务
        size_t failed_tasks = 0;         ///< 失败的任务
        size_t cancelled_tasks = 0;      ///< 已取消的任务
        BytesPerSecond total_speed = 0;  ///< 总下载速度
        Bytes total_downloaded = 0;      ///< 总下载量
    };

    Statistics get_statistics() const;

    // === 状态持久化 ===

    /**
     * @brief 保存任务状态
     * @param file_path 状态文件路径
     * @return 是否成功保存
     */
    bool save_state(const std::string& file_path);

    /**
     * @brief 加载任务状态
     * @param file_path 状态文件路径
     * @return 是否成功加载
     */
    bool load_state(const std::string& file_path);

    /**
     * @brief 设置状态文件
     * @param file_path 状态文件路径
     */
    void set_state_file(const std::string& file_path);

    // === 内部事件 ===

    /**
     * @brief 任务状态变化回调
     * @param task_id 任务ID
     * @param old_status 旧状态
     * @param new_status 新状态
     */
    void on_task_status_changed(TaskId task_id,
                                TaskStatus old_status,
                                TaskStatus new_status);

    /**
     * @brief 任务进度更新回调
     * @param task_id 任务ID
     * @param progress 进度信息
     */
    void on_task_progress(TaskId task_id, const ProgressInfo& progress);

    /**
     * @brief 启动管理器
     */
    void start();

    /**
     * @brief 停止管理器
     */
    void stop();

    /**
     * @brief 检查是否正在运行
     * @return true if running
     */
    bool is_running() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace falcon