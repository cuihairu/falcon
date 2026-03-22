/**
 * @file daemon.hpp
 * @brief 守护进程管理 - 跨平台实现
 * @author Falcon Team
 * @date 2025-12-28
 *
 * 提供跨平台的守护进程功能：
 * - Unix: fork() + setsid() 守护化
 * - Windows: Service API 支持
 * - PID 文件管理
 * - 信号处理
 */

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace falcon::daemon {

/**
 * @brief 守护进程配置
 */
struct DaemonConfig {
    std::string pid_file;               ///< PID 文件路径
    std::string working_dir;            ///< 工作目录（默认当前目录）
    std::string log_file;               ///< 日志文件路径（可选）
    bool redirect_stdio = true;         ///< 重定向标准输入/输出到 /dev/null 或日志文件
    bool create_pid_file = true;        ///< 是否创建 PID 文件
    bool single_instance = true;        ///< 是否确保单实例运行
};

/**
 * @brief 守护进程状态
 */
enum class DaemonState {
    NotStarted,     ///< 未启动
    Starting,       ///< 启动中
    Running,        ///< 运行中
    Paused,         ///< 暂停（Windows Service）
    Stopping,       ///< 停止中
    Stopped         ///< 已停止
};

/**
 * @brief 服务控制回调类型
 */
using ServiceControlCallback = std::function<void()>;

/**
 * @brief 守护进程管理器
 *
 * 提供守护进程的创建、管理和停止功能
 */
class DaemonManager {
public:
    /**
     * @brief 构造函数
     * @param config 守护进程配置
     */
    explicit DaemonManager(const DaemonConfig& config = {});

    /**
     * @brief 析构函数
     */
    ~DaemonManager();

    // Non-copyable
    DaemonManager(const DaemonManager&) = delete;
    DaemonManager& operator=(const DaemonManager&) = delete;

    /**
     * @brief 守护化进程（Unix: fork + setsid, Windows: 无操作或服务模式）
     * @return true 成功守护化
     * @return false 失败
     */
    bool daemonize();

    /**
     * @brief 运行主循环，直到收到停止请求
     */
    void run(ServiceControlCallback stop_callback = nullptr,
             ServiceControlCallback reload_callback = nullptr);

    /**
     * @brief 检查是否以守护进程模式运行
     * @return true 如果是守护进程
     */
    bool is_daemon() const;

    /**
     * @brief 获取当前进程 PID
     * @return PID
     */
    int get_pid() const;

    /**
     * @brief 读取 PID 文件中的 PID
     * @return PID，如果文件不存在或无效返回 -1
     */
    int read_pid_file() const;

    /**
     * @brief 检查是否有其他实例在运行
     * @return true 如果有其他实例
     */
    bool is_another_instance_running() const;

    /**
     * @brief 停止另一个实例（通过 PID 文件）
     * @return true 成功发送停止信号
     */
    bool stop_another_instance();

    /**
     * @brief 获取当前状态
     * @return 状态
     */
    DaemonState get_state() const;

    /**
     * @brief 请求停止守护进程
     */
    void request_stop();

    /**
     * @brief 检查是否收到停止请求
     * @return true 如果应该停止
     */
    bool should_stop() const;

    /**
     * @brief 设置停止回调（收到停止信号时调用）
     * @param callback 回调函数
     */
    void set_stop_callback(ServiceControlCallback callback);

    /**
     * @brief 设置重载回调（收到 SIGHUP 时调用）
     * @param callback 回调函数
     */
    void set_reload_callback(ServiceControlCallback callback);

    /**
     * @brief 触发重载回调
     */
    void reload();

    /**
     * @brief 兼容别名：请求停止
     */
    void stop();

    /**
     * @brief 是否仍在运行
     */
    bool is_running() const;

    /**
     * @brief 获取最后的错误信息
     * @return 错误信息
     */
    std::string get_last_error() const;

    /**
     * @brief 获取全局实例（供信号处理器使用）
     */
    static DaemonManager* get_instance();

#ifdef _WIN32
    bool run_as_service();
    bool install_service(const std::string& binary_path,
                         const std::string& display_name = "",
                         const std::string& description = "");
    bool uninstall_service();

    /**
     * @brief Windows 服务入口点（内部使用）
     */
    static void WINAPI service_main(DWORD argc, LPSTR* argv);

    /**
     * @brief Windows 服务控制处理器（内部使用）
     */
    static void WINAPI service_control_handler(DWORD ctrl_type);
#endif

private:
    /**
     * @brief 创建 PID 文件
     * @return true 成功
     */
    bool create_pid_file();

    /**
     * @brief 删除 PID 文件
     */
    void remove_pid_file();

    /**
     * @brief 设置信号处理器
     */
    void setup_signal_handlers();

    /**
     * @brief 重定向标准 I/O
     */
    void redirect_stdio();

    /**
     * @brief 切换工作目录
     */
    void change_working_directory();

    DaemonConfig config_;
    std::atomic<DaemonState> state_{DaemonState::NotStarted};
    std::atomic<bool> stop_requested_{false};
    ServiceControlCallback stop_callback_;
    ServiceControlCallback reload_callback_;
    mutable std::string last_error_;
    bool is_daemon_{false};

#ifdef _WIN32
    SERVICE_STATUS_HANDLE status_handle_{nullptr};
    SERVICE_STATUS service_status_{};

    void report_service_status(DWORD current_state,
                               DWORD win32_exit_code,
                               DWORD wait_hint);
#endif

    static DaemonManager* instance_;
};

/**
 * @brief 辅助函数：获取默认 PID 文件路径
 * @return PID 文件路径
 */
std::string get_default_pid_file();

/**
 * @brief 辅助函数：获取默认配置目录
 * @return 配置目录路径
 */
std::string get_default_config_dir();

/**
 * @brief 辅助函数：创建目录（包括父目录）
 * @param path 目录路径
 * @return true 成功
 */
bool create_directories(const std::string& path);

} // namespace falcon::daemon
