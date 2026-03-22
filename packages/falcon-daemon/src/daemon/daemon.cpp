/**
 * @file daemon.cpp
 * @brief 守护进程管理实现
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "daemon.hpp"
#include <falcon/logger.hpp>

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <chrono>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <syslog.h>
#else
#include <psapi.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace falcon::daemon {

// ============================================================================
// 辅助函数实现
// ============================================================================

std::string get_default_pid_file() {
#ifdef _WIN32
    // Windows: %PROGRAMDATA%\falcon\falcon-daemon.pid
    char path[MAX_PATH];
    if (GetEnvironmentVariableA("PROGRAMDATA", path, MAX_PATH) > 0) {
        return std::string(path) + "\\falcon\\falcon-daemon.pid";
    }
    return "C:\\ProgramData\\falcon\\falcon-daemon.pid";
#else
    // Unix: /var/run/falcon-daemon.pid 或 /tmp/falcon-daemon.pid
    if (geteuid() == 0) {
        return "/var/run/falcon-daemon.pid";
    }
    return "/tmp/falcon-daemon.pid";
#endif
}

std::string get_default_config_dir() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", path, MAX_PATH) > 0) {
        return std::string(path) + "\\falcon";
    }
    return "C:\\ProgramData\\falcon";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/falcon";
    }
    return "/etc/falcon";
#endif
}

bool create_directories(const std::string& path) {
    try {
        std::filesystem::create_directories(path);
        return true;
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to create directories: " << path << " - " << e.what());
        return false;
    }
}

// ============================================================================
// DaemonManager 实现
// ============================================================================

DaemonManager* DaemonManager::instance_ = nullptr;

DaemonManager::DaemonManager(const DaemonConfig& config)
    : config_(config) {
    instance_ = this;
#ifdef _WIN32
    std::memset(&service_status_, 0, sizeof(service_status_));
    service_status_.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status_.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
#endif
}

DaemonManager::~DaemonManager() {
    if (is_daemon_) {
        remove_pid_file();
    }
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

bool DaemonManager::daemonize() {
#ifdef _WIN32
    // Windows: 不支持传统 fork() 守护化
    // 如需 Windows Service，使用 run_as_service()
    FALCON_LOG_WARN("Windows does not support fork-based daemonization. Use run_as_service() for Windows Service.");
    is_daemon_ = true;
    state_ = DaemonState::Running;

    if (config_.create_pid_file && !config_.pid_file.empty()) {
        return create_pid_file();
    }
    return true;
#else
    // Unix: 使用 fork() + setsid() 守护化

    // 1. 第一次 fork
    pid_t pid = fork();
    if (pid < 0) {
        last_error_ = "First fork failed";
        FALCON_LOG_ERROR(last_error_);
        return false;
    }
    if (pid > 0) {
        // 父进程退出
        _exit(0);
    }

    // 2. 创建新会话
    if (setsid() < 0) {
        last_error_ = "setsid() failed";
        FALCON_LOG_ERROR(last_error_);
        return false;
    }

    // 3. 第二次 fork（确保永远不会获取终端）
    pid = fork();
    if (pid < 0) {
        last_error_ = "Second fork failed";
        FALCON_LOG_ERROR(last_error_);
        return false;
    }
    if (pid > 0) {
        _exit(0);
    }

    // 4. 设置文件权限掩码
    umask(0);

    // 5. 切换工作目录
    change_working_directory();

    // 6. 关闭并重定向标准 I/O
    if (config_.redirect_stdio) {
        redirect_stdio();
    }

    is_daemon_ = true;
    state_ = DaemonState::Running;

    // 7. 设置信号处理器
    setup_signal_handlers();

    // 8. 创建 PID 文件
    if (config_.create_pid_file) {
        if (!create_pid_file()) {
            return false;
        }
    }

    FALCON_LOG_INFO("Daemon started successfully (PID: " << getpid() << ")");
    return true;
#endif
}

void DaemonManager::run(ServiceControlCallback stop_callback,
                        ServiceControlCallback reload_callback) {
    stop_callback_ = std::move(stop_callback);
    reload_callback_ = std::move(reload_callback);

    state_ = DaemonState::Running;

    // 主循环
    while (!stop_requested_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 调用停止回调
    if (stop_callback_) {
        FALCON_LOG_INFO("Executing stop callback...");
        stop_callback_();
    }

    state_ = DaemonState::Stopped;
    FALCON_LOG_INFO("Daemon stopped");
}

bool DaemonManager::is_daemon() const {
    return is_daemon_;
}

int DaemonManager::get_pid() const {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

int DaemonManager::read_pid_file() const {
    if (config_.pid_file.empty()) {
        return -1;
    }
    std::ifstream pid_file(config_.pid_file);
    if (!pid_file.is_open()) {
        return -1;
    }
    int pid = -1;
    pid_file >> pid;
    return pid > 0 ? pid : -1;
}

bool DaemonManager::is_another_instance_running() const {
    int pid = read_pid_file();
    if (pid <= 0 || pid == get_pid()) {
        return false;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return false;
    }
    CloseHandle(process);
    return true;
#else
    return kill(pid, 0) == 0;
#endif
}

bool DaemonManager::stop_another_instance() {
    int pid = read_pid_file();
    if (pid <= 0 || pid == get_pid()) {
        return false;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return false;
    }
    BOOL ok = TerminateProcess(process, 0);
    CloseHandle(process);
    return ok == TRUE;
#else
    return kill(pid, SIGTERM) == 0;
#endif
}

void DaemonManager::request_stop() {
    stop();
}

bool DaemonManager::should_stop() const {
    return stop_requested_.load();
}

void DaemonManager::set_stop_callback(ServiceControlCallback callback) {
    stop_callback_ = std::move(callback);
}

void DaemonManager::set_reload_callback(ServiceControlCallback callback) {
    reload_callback_ = std::move(callback);
}

#ifdef _WIN32
// Windows Service 实现

void WINAPI DaemonManager::service_main(DWORD argc, LPSTR* argv) {
    if (!instance_) return;

    instance_->status_handle_ = RegisterServiceCtrlHandlerA(
        "falcon-daemon", service_control_handler);

    if (!instance_->status_handle_) {
        FALCON_LOG_ERROR("RegisterServiceCtrlHandler failed");
        return;
    }

    instance_->report_service_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    // 初始化
    instance_->is_daemon_ = true;
    instance_->state_ = DaemonState::Running;

    if (instance_->config_.create_pid_file && !instance_->config_.pid_file.empty()) {
        instance_->create_pid_file();
    }

    instance_->report_service_status(SERVICE_RUNNING, NO_ERROR, 0);

    // 运行主循环
    while (!instance_->stop_requested_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (instance_->stop_callback_) {
        instance_->stop_callback_();
    }

    instance_->report_service_status(SERVICE_STOPPED, NO_ERROR, 0);
}

void WINAPI DaemonManager::service_control_handler(DWORD ctrl_code) {
    if (!instance_) return;

    switch (ctrl_code) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            instance_->report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 0);
            instance_->stop_requested_ = true;
            break;

        case SERVICE_CONTROL_PAUSE:
            instance_->report_service_status(SERVICE_PAUSE_PENDING, NO_ERROR, 0);
            instance_->state_ = DaemonState::Paused;
            instance_->report_service_status(SERVICE_PAUSED, NO_ERROR, 0);
            break;

        case SERVICE_CONTROL_CONTINUE:
            instance_->report_service_status(SERVICE_CONTINUE_PENDING, NO_ERROR, 0);
            instance_->state_ = DaemonState::Running;
            instance_->report_service_status(SERVICE_RUNNING, NO_ERROR, 0);
            break;

        case SERVICE_CONTROL_INTERROGATE:
            break;

        default:
            break;
    }
}

void DaemonManager::report_service_status(DWORD current_state,
                                           DWORD win32_exit_code,
                                           DWORD wait_hint) {
    service_status_.dwCurrentState = current_state;
    service_status_.dwWin32ExitCode = win32_exit_code;
    service_status_.dwWaitHint = wait_hint;

    if (current_state == SERVICE_START_PENDING ||
        current_state == SERVICE_STOP_PENDING ||
        current_state == SERVICE_PAUSE_PENDING ||
        current_state == SERVICE_CONTINUE_PENDING) {
        service_status_.dwControlsAccepted = 0;
    } else {
        service_status_.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if (current_state == SERVICE_RUNNING || current_state == SERVICE_STOPPED) {
        service_status_.dwCheckPoint = 0;
    } else {
        service_status_.dwCheckPoint++;
    }

    SetServiceStatus(status_handle_, &service_status_);
}

bool DaemonManager::run_as_service() {
    SERVICE_TABLE_ENTRYA service_table[] = {
        {const_cast<LPSTR>("falcon-daemon"), service_main},
        {nullptr, nullptr}
    };

    if (!StartServiceCtrlDispatcherA(service_table)) {
        last_error_ = "StartServiceCtrlDispatcher failed: " + std::to_string(GetLastError());
        FALCON_LOG_ERROR(last_error_);
        return false;
    }

    return true;
}

bool DaemonManager::install_service(const std::string& binary_path,
                                    const std::string& display_name,
                                    const std::string& description) {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        last_error_ = "Failed to open SCM: " + std::to_string(GetLastError());
        FALCON_LOG_ERROR(last_error_);
        return false;
    }

    SC_HANDLE service = CreateServiceA(
        scm,
        "falcon-daemon",
        display_name.empty() ? "Falcon Download Daemon" : display_name.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        binary_path.c_str(),
        nullptr,  // No load ordering group
        nullptr,  // No tag identifier
        nullptr,  // No dependencies
        nullptr,  // LocalSystem account
        nullptr   // No password
    );

    if (!service) {
        last_error_ = "Failed to create service: " + std::to_string(GetLastError());
        FALCON_LOG_ERROR(last_error_);
        CloseServiceHandle(scm);
        return false;
    }

    // 设置服务描述
    if (!description.empty()) {
        SERVICE_DESCRIPTIONA desc;
        desc.lpDescription = const_cast<LPSTR>(description.c_str());
        ChangeServiceConfig2A(service, SERVICE_CONFIG_DESCRIPTION, &desc);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    FALCON_LOG_INFO("Service installed successfully");
    return true;
}

bool DaemonManager::uninstall_service() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        last_error_ = "Failed to open SCM: " + std::to_string(GetLastError());
        FALCON_LOG_ERROR(last_error_);
        return false;
    }

    SC_HANDLE service = OpenServiceA(scm, "falcon-daemon", SERVICE_STOP | DELETE);
    if (!service) {
        last_error_ = "Failed to open service: " + std::to_string(GetLastError());
        FALCON_LOG_ERROR(last_error_);
        CloseServiceHandle(scm);
        return false;
    }

    // 先停止服务
    SERVICE_STATUS status;
    ControlService(service, SERVICE_CONTROL_STOP, &status);

    // 删除服务
    BOOL result = DeleteService(service);

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (result) {
        FALCON_LOG_INFO("Service uninstalled successfully");
        return true;
    } else {
        last_error_ = "Failed to delete service: " + std::to_string(GetLastError());
        FALCON_LOG_ERROR(last_error_);
        return false;
    }
}
#endif

void DaemonManager::stop() {
    stop_requested_ = true;
    state_ = DaemonState::Stopping;
}

void DaemonManager::reload() {
    if (reload_callback_) {
        FALCON_LOG_INFO("Executing reload callback...");
        reload_callback_();
    }
}

bool DaemonManager::is_running() const {
    return state_ == DaemonState::Running && !stop_requested_.load();
}

DaemonState DaemonManager::get_state() const {
    return state_.load();
}

std::string DaemonManager::get_last_error() const {
    return last_error_;
}

DaemonManager* DaemonManager::get_instance() {
    return instance_;
}

// ============================================================================
// 私有方法实现
// ============================================================================

bool DaemonManager::create_pid_file() {
    // 确保目录存在
    std::filesystem::path pid_path(config_.pid_file);
    std::filesystem::path pid_dir = pid_path.parent_path();

    if (!pid_dir.empty() && !std::filesystem::exists(pid_dir)) {
        if (!create_directories(pid_dir.string())) {
            last_error_ = "Failed to create PID file directory";
            return false;
        }
    }

#ifdef _WIN32
    // Windows: 使用文件锁确保只有一个实例
    HANDLE file = CreateFileA(
        config_.pid_file.c_str(),
        GENERIC_WRITE,
        0,  // No sharing - 锁定文件
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) {
            last_error_ = "Another instance is already running";
        } else {
            last_error_ = "Failed to create PID file: " + std::to_string(GetLastError());
        }
        FALCON_LOG_ERROR(last_error_);
        return false;
    }

    // 写入 PID
    DWORD pid = GetCurrentProcessId();
    std::string pid_str = std::to_string(pid);
    DWORD written;
    WriteFile(file, pid_str.c_str(), static_cast<DWORD>(pid_str.length()), &written, nullptr);
    CloseHandle(file);

#else
    // Unix: 检查是否已有实例运行
    std::ifstream existing_pid(config_.pid_file);
    if (existing_pid.is_open()) {
        pid_t existing;
        existing_pid >> existing;
        existing_pid.close();

        // 检查进程是否存在
        if (existing > 0 && kill(existing, 0) == 0) {
            last_error_ = "Another instance is already running (PID: " + std::to_string(existing) + ")";
            FALCON_LOG_ERROR(last_error_);
            return false;
        }
    }

    // 写入 PID
    std::ofstream pid_file(config_.pid_file);
    if (!pid_file.is_open()) {
        last_error_ = "Failed to create PID file";
        FALCON_LOG_ERROR(last_error_);
        return false;
    }

    pid_file << getpid() << std::endl;
    pid_file.close();

    // 设置权限
    chmod(config_.pid_file.c_str(), 0644);
#endif

    FALCON_LOG_INFO("PID file created: " << config_.pid_file);
    return true;
}

void DaemonManager::remove_pid_file() {
    if (!config_.pid_file.empty() && std::filesystem::exists(config_.pid_file)) {
        std::filesystem::remove(config_.pid_file);
        FALCON_LOG_INFO("PID file removed: " << config_.pid_file);
    }
}

#ifndef _WIN32
// Unix 信号处理器
static void unix_signal_handler(int signum) {
    if (auto* instance = DaemonManager::get_instance()) {
        switch (signum) {
            case SIGTERM:
            case SIGINT:
                instance->stop();
                break;
            case SIGHUP:
                instance->reload();
                break;
        }
    }
}
#endif

void DaemonManager::setup_signal_handlers() {
#ifndef _WIN32
    // Unix 信号处理
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));

    // SIGTERM - 正常终止
    sa.sa_handler = unix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);

    // SIGINT - Ctrl+C
    sigaction(SIGINT, &sa, nullptr);

    // SIGHUP - 重新加载配置
    sa.sa_handler = unix_signal_handler;
    sigaction(SIGHUP, &sa, nullptr);

    // SIGCHLD - 忽略子进程退出
    sa.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &sa, nullptr);

    // SIGPIPE - 忽略管道断开
    sigaction(SIGPIPE, &sa, nullptr);

    FALCON_LOG_DEBUG("Signal handlers configured");
#else
    // Windows 使用 SetConsoleCtrlHandler
    SetConsoleCtrlHandler([](DWORD ctrl_type) -> BOOL {
        if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
            if (auto* instance = DaemonManager::get_instance()) {
                instance->stop();
            }
            return TRUE;
        }
        return FALSE;
    }, TRUE);
#endif
}

void DaemonManager::redirect_stdio() {
#ifndef _WIN32
    // 关闭标准文件描述符
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // 重定向到 /dev/null 或日志文件
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        if (!config_.log_file.empty()) {
            // 重定向到日志文件
            int log_fd = open(config_.log_file.c_str(),
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            } else {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
            }
        } else {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        if (fd > STDERR_FILENO) {
            close(fd);
        }
    }
#endif
}

void DaemonManager::change_working_directory() {
    if (!config_.working_dir.empty()) {
        std::filesystem::current_path(config_.working_dir);
        FALCON_LOG_DEBUG("Changed working directory to: " << config_.working_dir);
    } else {
#ifndef _WIN32
        // 默认切换到根目录
        chdir("/");
#endif
    }
}

} // namespace falcon::daemon
