// Falcon CLI - Main Entry Point
// Copyright (c) 2025 Falcon Project

#include <falcon/falcon.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

// Global engine pointer for signal handling
falcon::DownloadEngine* g_engine = nullptr;
std::atomic<bool> g_interrupted{false};

void signal_handler(int /*signal*/) {
    g_interrupted = true;
    if (g_engine) {
        g_engine->cancel_all();
    }
}

// Format bytes to human-readable string
std::string format_bytes(falcon::Bytes bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        ++unit_index;
    }

    std::ostringstream ss;
    if (unit_index == 0) {
        ss << bytes << " " << units[unit_index];
    } else {
        ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    }
    return ss.str();
}

// Format duration to human-readable string
std::string format_duration(std::chrono::seconds seconds) {
    auto secs = seconds.count();
    if (secs < 0) {
        return "--:--:--";
    }

    int hours = static_cast<int>(secs / 3600);
    int mins = static_cast<int>((secs % 3600) / 60);
    int s = static_cast<int>(secs % 60);

    std::ostringstream ss;
    if (hours > 0) {
        ss << std::setfill('0') << std::setw(2) << hours << ":";
    }
    ss << std::setfill('0') << std::setw(2) << mins << ":"
       << std::setfill('0') << std::setw(2) << s;
    return ss.str();
}

// Get terminal width
int get_terminal_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return 80;
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        return w.ws_col;
    }
    return 80;
#endif
}

// Progress bar event listener
class ProgressListener : public falcon::IEventListener {
public:
    void on_status_changed(falcon::TaskId task_id, falcon::TaskStatus old_status,
                           falcon::TaskStatus new_status) override {
        (void)old_status;
        if (new_status == falcon::TaskStatus::Downloading) {
            std::cout << "\r\033[K";  // Clear line
            std::cout << "[开始下载] 任务 " << task_id << std::endl;
        } else if (new_status == falcon::TaskStatus::Completed) {
            std::cout << "\r\033[K";
            std::cout << "[完成] 任务 " << task_id << " 下载完成!" << std::endl;
        } else if (new_status == falcon::TaskStatus::Failed) {
            std::cout << "\r\033[K";
            std::cout << "[失败] 任务 " << task_id << std::endl;
        } else if (new_status == falcon::TaskStatus::Cancelled) {
            std::cout << "\r\033[K";
            std::cout << "[取消] 任务 " << task_id << std::endl;
        }
    }

    void on_progress(const falcon::ProgressInfo& info) override {
        // Calculate progress bar
        int width = get_terminal_width();
        int bar_width = std::max(20, width - 60);

        int filled = 0;
        if (info.total_bytes > 0) {
            filled = static_cast<int>(
                (static_cast<double>(info.downloaded_bytes) /
                 static_cast<double>(info.total_bytes)) *
                bar_width);
        }

        // Build progress bar
        std::ostringstream bar;
        bar << "[";
        for (int i = 0; i < bar_width; ++i) {
            if (i < filled) {
                bar << "=";
            } else if (i == filled) {
                bar << ">";
            } else {
                bar << " ";
            }
        }
        bar << "]";

        // Format output
        std::string downloaded = format_bytes(info.downloaded_bytes);
        std::string total =
            info.total_bytes > 0 ? format_bytes(info.total_bytes) : "???";
        std::string speed = format_bytes(info.speed) + "/s";

        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            info.estimated_remaining);
        std::string eta = format_duration(remaining);

        int percent = static_cast<int>(info.progress * 100);

        // Print progress
        std::cout << "\r\033[K";  // Clear line
        std::cout << bar.str() << " " << std::setw(3) << percent << "% "
                  << downloaded << "/" << total << " | " << speed
                  << " | ETA: " << eta;
        std::cout.flush();
    }

    void on_error(falcon::TaskId task_id,
                  const std::string& error_message) override {
        std::cout << "\r\033[K";
        std::cerr << "[错误] 任务 " << task_id << ": " << error_message
                  << std::endl;
    }

    void on_completed(falcon::TaskId task_id,
                      const std::string& output_path) override {
        std::cout << "\r\033[K";
        std::cout << "[完成] 已保存到: " << output_path << std::endl;
    }
};

void print_version() {
    std::cout << "Falcon Downloader v" << falcon::VERSION_STRING << std::endl;
    std::cout << "A modern multi-protocol download library and tool"
              << std::endl;
}

void print_help(const char* program_name) {
    std::cout << "使用方法: " << program_name << " [选项] <URL>" << std::endl;
    std::cout << std::endl;
    std::cout << "选项:" << std::endl;
    std::cout << "  -h, --help            显示帮助信息" << std::endl;
    std::cout << "  -V, --version         显示版本信息" << std::endl;
    std::cout << "  -o, --output <FILE>   输出文件名" << std::endl;
    std::cout << "  -d, --dir <DIR>       输出目录 (默认: 当前目录)" << std::endl;
    std::cout << "  -c, --connections <N> 并发连接数 (默认: 4)" << std::endl;
    std::cout << "  -l, --limit <SPEED>   限速 (例如: 1M, 512K)" << std::endl;
    std::cout << "  -t, --timeout <SEC>   超时时间 (默认: 30秒)" << std::endl;
    std::cout << "  --retry <N>           重试次数 (默认: 3)" << std::endl;
    std::cout << "  --proxy <URL>         代理服务器" << std::endl;
    std::cout << "  --no-resume           禁用断点续传" << std::endl;
    std::cout << "  --no-verify-ssl       跳过 SSL 验证" << std::endl;
    std::cout << "  -q, --quiet           静默模式" << std::endl;
    std::cout << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  " << program_name << " https://example.com/file.zip"
              << std::endl;
    std::cout << "  " << program_name
              << " -o myfile.zip https://example.com/file.zip" << std::endl;
    std::cout << "  " << program_name
              << " -d ~/Downloads -c 8 https://example.com/large.iso"
              << std::endl;
}

falcon::Bytes parse_speed_limit(const std::string& limit) {
    if (limit.empty()) {
        return 0;
    }

    falcon::Bytes value = 0;
    char suffix = 0;
    std::istringstream ss(limit);
    ss >> value >> suffix;

    switch (std::toupper(suffix)) {
        case 'K':
            return value * 1024;
        case 'M':
            return value * 1024 * 1024;
        case 'G':
            return value * 1024 * 1024 * 1024;
        default:
            return value;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    // Default options
    falcon::DownloadOptions options;
    std::string url;
    bool quiet = false;

    // Parse command line arguments (simple parser)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "-V" || arg == "--version") {
            print_version();
            return 0;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            options.output_filename = argv[++i];
        } else if ((arg == "-d" || arg == "--dir") && i + 1 < argc) {
            options.output_directory = argv[++i];
        } else if ((arg == "-c" || arg == "--connections") && i + 1 < argc) {
            options.max_connections = std::stoul(argv[++i]);
        } else if ((arg == "-l" || arg == "--limit") && i + 1 < argc) {
            options.speed_limit = parse_speed_limit(argv[++i]);
        } else if ((arg == "-t" || arg == "--timeout") && i + 1 < argc) {
            options.timeout_seconds = std::stoul(argv[++i]);
        } else if (arg == "--retry" && i + 1 < argc) {
            options.max_retries = std::stoul(argv[++i]);
        } else if (arg == "--proxy" && i + 1 < argc) {
            options.proxy = argv[++i];
        } else if (arg == "--no-resume") {
            options.resume_enabled = false;
        } else if (arg == "--no-verify-ssl") {
            options.verify_ssl = false;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg[0] != '-') {
            url = arg;
        } else {
            std::cerr << "未知选项: " << arg << std::endl;
            std::cerr << "使用 --help 查看帮助信息" << std::endl;
            return 1;
        }
    }

    // Validate URL
    if (url.empty()) {
        std::cerr << "错误: 未指定下载 URL" << std::endl;
        std::cerr << "使用 --help 查看帮助信息" << std::endl;
        return 1;
    }

    // Setup signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        // Create engine
        falcon::EngineConfig config;
        config.auto_start = true;

        falcon::DownloadEngine engine(config);
        g_engine = &engine;

        // Add event listener
        ProgressListener listener;
        if (!quiet) {
            engine.add_listener(&listener);
        }

        // Note: In a real implementation, we would register the HTTP handler here
        // For now, we'll show a message about missing handlers
        auto protocols = engine.get_supported_protocols();
        if (protocols.empty()) {
            std::cerr << "警告: 未注册任何协议处理器" << std::endl;
            std::cerr << "请确保编译时启用了 FALCON_ENABLE_HTTP 选项" << std::endl;

            // For demonstration, show what would happen
            std::cout << "\n准备下载: " << url << std::endl;
            std::cout << "输出目录: "
                      << (options.output_directory.empty()
                              ? "."
                              : options.output_directory)
                      << std::endl;
            std::cout << "并发连接: " << options.max_connections << std::endl;
            if (options.speed_limit > 0) {
                std::cout << "限速: " << format_bytes(options.speed_limit) << "/s"
                          << std::endl;
            }
            std::cout << "\n(实际下载功能需要编译时启用 libcurl)" << std::endl;
            return 0;
        }

        // Add download task
        auto task = engine.add_task(url, options);
        if (!task) {
            std::cerr << "错误: 无法创建下载任务" << std::endl;
            return 1;
        }

        if (!quiet) {
            std::cout << "开始下载: " << url << std::endl;
            std::cout << "保存到: " << task->output_path() << std::endl;
            std::cout << std::endl;
        }

        // Wait for completion
        while (!task->is_finished() && !g_interrupted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << std::endl;

        if (task->status() == falcon::TaskStatus::Completed) {
            if (!quiet) {
                std::cout << "下载完成!" << std::endl;
            }
            return 0;
        } else if (task->status() == falcon::TaskStatus::Cancelled) {
            std::cerr << "下载已取消" << std::endl;
            return 130;  // Standard exit code for SIGINT
        } else {
            std::cerr << "下载失败: " << task->error_message() << std::endl;
            return 1;
        }

    } catch (const falcon::InvalidURLException& e) {
        std::cerr << "错误: 无效的 URL - " << e.what() << std::endl;
        return 1;
    } catch (const falcon::UnsupportedProtocolException& e) {
        std::cerr << "错误: 不支持的协议 - " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
