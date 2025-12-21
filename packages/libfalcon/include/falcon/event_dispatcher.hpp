/**
 * @file event_dispatcher.hpp
 * @brief 事件分发器
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/types.hpp>
#include <falcon/event_listener.hpp>

#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <functional>

namespace falcon {

/**
 * @brief 事件类型枚举
 */
enum class EventType : int {
    StatusChanged,     ///< 状态变化
    Progress,          ///< 进度更新
    Error,            ///< 错误发生
    Completed,        ///< 任务完成
    FileInfo,         ///< 文件信息获取
    SpeedLimit,       ///< 速度限制
    Statistics,       ///< 统计信息
    Custom            ///< 自定义事件
};

/**
 * @brief 事件数据基类
 */
struct EventData {
    EventType type;
    TaskId task_id;

    explicit EventData(EventType t, TaskId id = INVALID_TASK_ID)
        : type(t), task_id(id) {}

    virtual ~EventData() = default;
};

/**
 * @brief 状态变化事件
 */
struct StatusChangedEvent : public EventData {
    TaskStatus old_status;
    TaskStatus new_status;

    StatusChangedEvent(TaskId id, TaskStatus old, TaskStatus new_)
        : EventData(EventType::StatusChanged, id)
        , old_status(old)
        , new_status(new_) {}
};

/**
 * @brief 进度更新事件
 */
struct ProgressEvent : public EventData {
    ProgressInfo progress;

    ProgressEvent(TaskId id, const ProgressInfo& info)
        : EventData(EventType::Progress, id)
        , progress(info) {}
};

/**
 * @brief 错误事件
 */
struct ErrorEvent : public EventData {
    std::string error_message;

    ErrorEvent(TaskId id, const std::string& message)
        : EventData(EventType::Error, id)
        , error_message(message) {}
};

/**
 * @brief 任务完成事件
 */
struct CompletedEvent : public EventData {
    std::string output_path;
    Bytes total_size;
    Duration duration;

    CompletedEvent(TaskId id, const std::string& path, Bytes size, Duration dur)
        : EventData(EventType::Completed, id)
        , output_path(path)
        , total_size(size)
        , duration(dur) {}
};

/**
 * @brief 文件信息事件
 */
struct FileInfoEvent : public EventData {
    FileInfo file_info;

    FileInfoEvent(TaskId id, const FileInfo& info)
        : EventData(EventType::FileInfo, id)
        , file_info(info) {}
};

/**
 * @brief 速度限制事件
 */
struct SpeedLimitEvent : public EventData {
    BytesPerSecond current_speed;
    BytesPerSecond limit;

    SpeedLimitEvent(TaskId id, BytesPerSecond speed, BytesPerSecond limit)
        : EventData(EventType::SpeedLimit, id)
        , current_speed(speed)
        , limit(limit) {}
};

/**
 * @brief 自定义事件
 */
struct CustomEvent : public EventData {
    std::string event_name;
    std::string data;

    CustomEvent(const std::string& name, const std::string& d, TaskId id = INVALID_TASK_ID)
        : EventData(EventType::Custom, id)
        , event_name(name)
        , data(d) {}
};

/**
 * @brief 事件处理器函数类型
 */
using EventHandler = std::function<void(std::shared_ptr<EventData>)>;

/**
 * @brief 事件分发器配置
 */
struct EventDispatcherConfig {
    size_t max_queue_size = 10000;        ///< 最大事件队列大小
    size_t thread_pool_size = 2;          ///< 事件处理线程池大小
    bool enable_async_dispatch = true;    ///< 启用异步分发
    std::chrono::milliseconds dispatch_interval{10}; ///< 分发间隔
};

/**
 * @brief 事件分发器
 *
 * 负责事件的分发和处理，支持：
 * - 同步/异步事件分发
 * - 多线程事件处理
 * - 事件过滤和优先级
 * - 性能监控
 */
class EventDispatcher {
public:
    /**
     * @brief 构造函数
     * @param config 配置选项
     */
    explicit EventDispatcher(const EventDispatcherConfig& config = {});

    /**
     * @brief 析构函数
     */
    ~EventDispatcher();

    // Non-copyable, non-movable
    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;
    EventDispatcher(EventDispatcher&&) = delete;
    EventDispatcher& operator=(EventDispatcher&&) = delete;

    // === 监听器管理 ===

    /**
     * @brief 添加事件监听器
     * @param listener 监听器指针（不获取所有权）
     */
    void add_listener(IEventListener* listener);

    /**
     * @brief 移除事件监听器
     * @param listener 监听器指针
     */
    void remove_listener(IEventListener* listener);

    /**
     * @brief 清空所有监听器
     */
    void clear_listeners();

    /**
     * @brief 获取监听器数量
     * @return 监听器数量
     */
    size_t get_listener_count() const;

    // === 事件分发 ===

    /**
     * @brief 分发事件（异步）
     * @param event 事件数据
     * @return 是否成功加入队列
     */
    bool dispatch(std::shared_ptr<EventData> event);

    /**
     * @brief 分发事件（同步）
     * @param event 事件数据
     */
    void dispatch_sync(std::shared_ptr<EventData> event);

    // === 便捷方法 ===

    /**
     * @brief 分发状态变化事件
     */
    void dispatch_status_changed(TaskId task_id, TaskStatus old_status, TaskStatus new_status);

    /**
     * @brief 分发进度事件
     */
    void dispatch_progress(TaskId task_id, const ProgressInfo& progress);

    /**
     * @brief 分发错误事件
     */
    void dispatch_error(TaskId task_id, const std::string& error_message);

    /**
     * @brief 分发完成事件
     */
    void dispatch_completed(TaskId task_id, const std::string& output_path,
                           Bytes total_size, Duration duration);

    /**
     * @brief 分发文件信息事件
     */
    void dispatch_file_info(TaskId task_id, const FileInfo& info);

    /**
     * @brief 分发自定义事件
     */
    void dispatch_custom(const std::string& event_name, const std::string& data,
                         TaskId task_id = INVALID_TASK_ID);

    // === 控制 ===

    /**
     * @brief 启动事件分发器
     */
    void start();

    /**
     * @brief 停止事件分发器
     * @param wait_for_completion 是否等待所有事件处理完成
     */
    void stop(bool wait_for_completion = true);

    /**
     * @brief 检查是否正在运行
     * @return true if running
     */
    bool is_running() const;

    /**
     * @brief 获取队列大小
     * @return 当前队列中的事件数量
     */
    size_t get_queue_size() const;

    /**
     * @brief 获取已处理的事件数量
     * @return 已处理事件数
     */
    uint64_t get_processed_count() const;

    /**
     * @brief 获取丢弃的事件数量
     * @return 丢弃事件数
     */
    uint64_t get_dropped_count() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace falcon