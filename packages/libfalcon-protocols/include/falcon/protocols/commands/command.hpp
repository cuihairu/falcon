/**
 * @file command.hpp
 * @brief 命令基类 - aria2 风格的事件驱动命令系统
 * @author Falcon Team
 * @date 2025-12-24
 *
 * 设计参考: aria2/src/Command.h, aria2/src/AbstractCommand.h
 * @see https://github.com/aria2/aria2/blob/master/src/Command.h
 */

#pragma once

#include <falcon/types.hpp>
#include <falcon/download_options.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace falcon {

// 前向声明
class DownloadEngineV2;

#ifdef FAILED
#undef FAILED
#endif

#ifdef COMPLETED
#undef COMPLETED
#endif

/**
 * @brief 命令状态 - 对应 aria2 的命令生命周期
 */
enum class CommandStatus {
    READY,       // 命令已创建，等待执行
    ACTIVE,      // 命令正在执行中
    COMPLETED,   // 命令执行完成
    FAILED       // 命令执行出错（避免 Windows 宏冲突）
};

/**
 * @brief 组纪元计数器（进程内单调递增）
 *
 * RequestGroup 构造时取号，Command 构造时快照当前值。同 id 组重建
 * （cancel→重注入 / 终态回收→重注入）后新组纪元严格大于旧组，执行
 * 处按「组纪元 > 命令出生纪元」识别旧代命令并静默收口——防止僵尸
 * 命令污染重建的新组。方向比较（而非等值）使无关任务换代推进的
 * 计数不影响其他任务的命令判定
 */
inline std::uint64_t next_group_epoch() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief 命令基类接口
 *
 * 采用 Command Pattern 实现事件驱动的下载流程。
 * 每个命令代表下载过程中的一个操作单元，如：
 * - 初始化连接
 * - 发送 HTTP 请求
 * - 接收响应数据
 * - 写入文件
 *
 * @note 对应 aria2 的 Command 类
 * @see https://github.com/aria2/aria2/blob/master/src/Command.h
 */
class Command {
public:
    virtual ~Command() = default;

    /**
     * @brief 执行命令
     *
     * 命令执行的核心方法。返回值决定命令是否需要重新入队：
     * - 返回 true: 命令已完成，从队列移除
     * - 返回 false: 命令需要等待（如 I/O 未完成），稍后重试
     *
     * @param engine 下载引擎实例
     * @return true 如果命令已完成
     * @return false 如果命令需要再次执行
     */
    virtual bool execute(DownloadEngineV2* engine) = 0;

    /**
     * @brief 获取命令状态
     */
    CommandStatus status() const noexcept { return status_; }

    /**
     * @brief 获取命令名称（用于调试和日志）
     */
    virtual const char* name() const = 0;

    /**
     * @brief 获取关联的任务 ID
     */
    TaskId task_id() const noexcept { return task_id_; }

    /**
     * @brief 获取唯一命令 ID
     */
    CommandId id() const noexcept { return command_id_; }

    /**
     * @brief 获取任务 ID（供测试访问）
     */
    TaskId get_task_id() const noexcept { return task_id_; }

    /**
     * @brief 命令出生时的组纪元快照
     *
     * 构造时快照全局纪元当前值；所属 RequestGroup 构造（更早）取号
     * 不大于该值。同 id 组重建后新组纪元严格更大，执行处按
     * 「组纪元 > 出生纪元」识别旧代命令
     */
    std::uint64_t born_epoch() const noexcept { return born_epoch_; }

    /**
     * @brief 是否豁免组纪元守卫
     *
     * 引擎内部管理命令（暂停/取消清扫命令等）不属于任何一代数据面，
     * 组重建后仍必须执行——默认不豁免，清扫命令 override 返回 true
     */
    virtual bool exempt_from_epoch_guard() const noexcept { return false; }

    /**
     * @brief 命令当前持有的 socket 文件描述符（未持有返回 -1）
     *
     * fd 生命周期由引擎统一管理（超时清理与停机排水路径 close），
     * 命令自身析构不关闭；引擎据此在 run() 退出时排水所有排队/挂起
     * 命令持有的 fd
     */
    virtual int socket_fd() const noexcept { return -1; }

    /**
     * @brief 引擎清扫（暂停回收挂起连接）前的检查点
     *
     * 有落盘状态的命令 override：冲刷写缓冲并把最终落盘进度上报
     * 任务组（默认无操作）。析构路径只冲刷不上报（悬垂指针风险），
     * 清扫路径在引擎线程内执行、可以安全触达任务组，必须补上报
     * 才能固化断点
     */
    virtual void prepare_sweep(DownloadEngineV2* /*engine*/) {}

    /**
     * @brief 超时清理路径：命令正承载某个段时尝试段级换源重试
     *
     * 有段上下文的命令 override（按命令各自携带的段号/范围/来源 URL
     * 调度段级重试命令），调度成功返回 true——引擎跳过该组的失败
     * 收口，段由重试链接管；默认无段上下文，返回 false 走既有整组
     * 失败。仅在 prepare_sweep 冲刷并上报断点之后调用（重试 Range
     * 依赖最新落盘进度）
     */
    virtual bool retry_expired_segment(DownloadEngineV2* /*engine*/) {
        return false;
    }

protected:
    explicit Command(TaskId task_id)
        : task_id_(task_id), command_id_(generate_command_id()),
          born_epoch_(next_group_epoch()) {}

    /**
     * @brief 转换命令状态
     */
    void transition(CommandStatus new_status) {
        status_ = new_status;
    }

    /**
     * @brief 设置命令状态为活动
     */
    void mark_active() { status_ = CommandStatus::ACTIVE; }

    /**
     * @brief 设置命令状态为完成
     */
    void mark_completed() { status_ = CommandStatus::COMPLETED; }

    /**
     * @brief 设置命令状态为错误
     */
    void mark_error() { status_ = CommandStatus::FAILED; }

private:
    static CommandId generate_command_id() {
        static std::atomic<CommandId> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    TaskId task_id_;           // 关联的下载任务 ID
    CommandId command_id_;     // 唯一命令 ID
    std::uint64_t born_epoch_; // 出生时组纪元快照（僵尸命令识别）
    CommandStatus status_ = CommandStatus::READY;
};

/**
 * @brief 抽象命令基类 - 提供通用功能
 *
 * 对应 aria2 的 AbstractCommand 类，提供大多数命令共享的基础功能
 * @see https://github.com/aria2/aria2/blob/master/src/AbstractCommand.h
 */
class AbstractCommand : public Command {
public:
    /**
     * @brief 命令执行结果
     */
    enum class ExecutionResult {
        OK,                // 命令执行完成
        WAIT_FOR_SOCKET,   // 等待 Socket 事件
        ERROR_OCCURRED,    // 发生错误
        NEED_RETRY         // 需要重试
    };

protected:
    explicit AbstractCommand(TaskId task_id)
        : Command(task_id) {}

    /**
     * @brief 处理执行结果
     *
     * 根据执行结果更新命令状态并返回适当的布尔值
     */
    bool handle_result(ExecutionResult result) {
        switch (result) {
            case ExecutionResult::OK:
                mark_completed();
                return true;  // 从队列移除

            case ExecutionResult::WAIT_FOR_SOCKET:
                mark_active();
                return false;  // 保持活跃，等待事件

            case ExecutionResult::ERROR_OCCURRED:
                mark_error();
                return true;  // 从队列移除（错误处理）

            case ExecutionResult::NEED_RETRY:
                mark_active();
                return false;  // 稍后重试
        }
        return false;
    }

    /**
     * @brief 获取下载引擎的命令队列
     *
     * 供子类添加后续命令使用
     */
    static void schedule_next(DownloadEngineV2* engine,
                              std::unique_ptr<Command> next_cmd);
};

/**
 * @brief 命令工厂 - 创建特定类型的命令
 *
 * 用于根据协议和当前状态创建合适的命令
 */
class CommandFactory {
public:
    /**
     * @brief 创建 HTTP 连接初始化命令
     */
    static std::unique_ptr<Command> create_http_init_command(
        TaskId task_id,
        const std::string& url,
        const DownloadOptions& options);

    /**
     * @brief 创建 HTTP 响应处理命令
     */
    static std::unique_ptr<Command> create_http_response_command(
        TaskId task_id,
        int socket_fd);

    /**
     * @brief 创建 HTTP 下载命令
     */
    static std::unique_ptr<Command> create_http_download_command(
        TaskId task_id,
        int socket_fd,
        SegmentId segment_id);

    /**
     * @brief 创建 FTP 连接命令
     */
    static std::unique_ptr<Command> create_ftp_init_command(
        TaskId task_id,
        const std::string& url,
        const DownloadOptions& options);
};

} // namespace falcon
