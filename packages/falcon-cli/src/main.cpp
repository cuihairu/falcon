/**
 * @file main.cpp
 * @brief falcon-cli 主程序入口
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <falcon/event_listener.hpp>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <sstream>

// 全局变量用于信号处理
std::atomic<bool> g_interrupted{false};
falcon::DownloadEngine* g_engine = nullptr;

// 信号处理函数
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_interrupted = true;
        if (g_engine) {
            g_engine->cancel_all();
        }
        std::cout << "\n\n中断信号接收，正在取消下载...\n";
    }
}

/**
 * @brief CLI 事件监听器
 */
class CliEventListener : public falcon::IEventListener {
public:
    explicit CliEventListener(bool verbose = false) : verbose_(verbose), last_progress_(0.0) {}

    std::string format_size(falcon::Bytes bytes) const {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int unit = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024 && unit < 4) {
            size /= 1024;
            unit++;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
        return oss.str();
    }

    void on_status_changed(falcon::TaskId task_id,
                           falcon::TaskStatus old_status,
                           falcon::TaskStatus new_status) override {
        if (!verbose_) return;

        std::cout << "\n[任务 " << task_id << "] "
                  << falcon::to_string(old_status)
                  << " -> " << falcon::to_string(new_status) << "\n";
    }

    void on_progress(const falcon::ProgressInfo& info) override {
        // 避免过于频繁的更新
        if (info.progress - last_progress_ < 0.01 && info.progress < 1.0) return;
        last_progress_ = info.progress;

        // 显示进度条
        display_progress(info);
    }

    void on_error(falcon::TaskId task_id, const std::string& error_message) override {
        std::cout << "\n错误 [任务 " << task_id << "]: " << error_message << "\n";
    }

    void on_completed(falcon::TaskId task_id, const std::string& output_path) override {
        std::cout << "\n✓ 下载完成 [任务 " << task_id << "]: " << output_path << "\n";
    }

    void on_file_info(falcon::TaskId task_id, const falcon::FileInfo& info) override {
        if (!verbose_) return;

        std::cout << "\n文件信息 [任务 " << task_id << "]:\n"
                  << "  大小: " << format_size(info.total_size) << "\n"
                  << "  类型: " << info.content_type << "\n";
    }

private:
    void display_progress(const falcon::ProgressInfo& info) {
        // 清除当前行
        std::cout << "\r";

        // 进度条（30个字符宽度）
        const int bar_width = 30;
        int filled = static_cast<int>(info.progress * bar_width);

        std::cout << "[";
        for (int i = 0; i < bar_width; ++i) {
            if (i < filled) {
                std::cout << "=";
            } else if (i == filled) {
                std::cout << ">";
            } else {
                std::cout << " ";
            }
        }
        std::cout << "] ";

        // 百分比
        std::cout << std::fixed << std::setprecision(1) << (info.progress * 100) << "% ";

        // 大小信息
        std::cout << "(" << format_size(info.downloaded_bytes);
        if (info.total_bytes > 0) {
            std::cout << " / " << format_size(info.total_bytes);
        }
        std::cout << ") ";

        // 速度
        if (info.speed > 0) {
            std::cout << " @ " << format_size(info.speed) << "/s";
        }

        std::cout.flush();
    }

    bool verbose_;
    float last_progress_;
};

/**
 * @brief 简单命令行参数解析器
 */
struct CliArgs {
    std::string url;
    std::string output_file;
    std::string output_dir;
    int connections = 4;
    falcon::Bytes speed_limit = 0;
    bool continue_download = true;
    int timeout = 30;
    bool verbose = false;
    bool quiet = false;
    bool show_help = false;
    bool show_version = false;
};

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            args.show_help = true;
        } else if (arg == "-V" || arg == "--version") {
            args.show_version = true;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                args.output_file = argv[++i];
            }
        } else if (arg == "-d" || arg == "--directory") {
            if (i + 1 < argc) {
                args.output_dir = argv[++i];
            }
        } else if (arg == "-c" || arg == "--connections") {
            if (i + 1 < argc) {
                args.connections = std::max(1, std::min(16, std::stoi(argv[++i])));
            }
        } else if (arg == "--no-continue") {
            args.continue_download = false;
        } else if (arg == "-t" || arg == "--timeout") {
            if (i + 1 < argc) {
                args.timeout = std::stoi(argv[++i]);
            }
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            args.quiet = true;
        } else if (!arg.empty() && arg[0] != '-') {
            if (args.url.empty()) {
                args.url = arg;
            }
        }
    }

    return args;
}

void show_help() {
    std::cout << "Falcon 命令行下载工具 v0.1.0\n\n";
    std::cout << "用法:\n";
    std::cout << "  falcon-cli [选项] <URL>\n\n";
    std::cout << "选项:\n";
    std::cout << "  -h, --help                 显示帮助信息\n";
    std::cout << "  -V, --version              显示版本信息\n";
    std::cout << "  -o, --output <文件>        指定输出文件名\n";
    std::cout << "  -d, --directory <目录>     指定输出目录\n";
    std::cout << "  -c, --connections <数量>   并发连接数 (1-16) [默认: 4]\n";
    std::cout << "      --no-continue          禁用断点续传\n";
    std::cout << "  -t, --timeout <秒>        超时时间 [默认: 30]\n";
    std::cout << "  -v, --verbose             详细输出\n";
    std::cout << "  -q, --quiet                静默模式\n\n";
    std::cout << "示例:\n";
    std::cout << "  falcon-cli https://example.com/file.zip\n";
    std::cout << "  falcon-cli https://example.com/file.zip -o /tmp/file.zip\n";
    std::cout << "  falcon-cli https://example.com/file.zip -c 8\n";
}

int main(int argc, char* argv[]) {
    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 解析命令行参数
    auto args = parse_args(argc, argv);

    // 显示帮助或版本信息
    if (args.show_help) {
        show_help();
        return 0;
    }

    if (args.show_version) {
        std::cout << "Falcon CLI v0.1.0\n";
        return 0;
    }

    // 检查必需参数
    if (args.url.empty()) {
        std::cerr << "错误：缺少 URL 参数\n";
        std::cerr << "使用 --help 查看帮助信息\n";
        return 1;
    }

    try {
        // 创建下载引擎
        falcon::EngineConfig engine_config;
        engine_config.max_concurrent_tasks = 1;  // CLI 默认单任务
        falcon::DownloadEngine engine(engine_config);
        g_engine = &engine;

        // 配置下载选项
        falcon::DownloadOptions options;
        options.max_connections = static_cast<size_t>(args.connections);
        options.timeout_seconds = static_cast<size_t>(args.timeout);
        options.resume_enabled = args.continue_download;
        options.speed_limit = args.speed_limit;
        options.user_agent = "Falcon/0.1";
        options.verify_ssl = true;

        // 设置输出路径
        if (!args.output_dir.empty()) {
            options.output_directory = args.output_dir;
        }
        if (!args.output_file.empty()) {
            options.output_filename = args.output_file;
        }

        // 创建事件监听器
        CliEventListener listener(args.verbose);
        if (!args.quiet) {
            engine.add_listener(&listener);
        }

        // 添加下载任务
        if (!args.quiet) {
            std::cout << "开始下载: " << args.url << "\n";
        }

        auto task = engine.add_task(args.url, options);
        if (!task) {
            std::cerr << "错误：无法添加下载任务\n";
            return 1;
        }

        // 启动任务
        if (!engine.start_task(task->id())) {
            std::cerr << "错误：无法启动下载任务\n";
            return 1;
        }

        // 等待下载完成
        while (!g_interrupted && !task->is_finished()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // 如果被中断，取消任务
        if (g_interrupted && !task->is_finished()) {
            task->cancel();
            std::cout << "\n下载已取消\n";
            return 1;
        }

        // 检查最终状态
        if (task->status() == falcon::TaskStatus::Completed) {
            if (!args.quiet) {
                std::cout << "\n下载成功完成！\n";
                auto stats = task->get_progress_info();
                std::cout << "总大小: " << listener.format_size(stats.total_bytes) << "\n";
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(stats.elapsed);
                std::cout << "用时: " << elapsed.count() << " 秒\n";
            }
            return 0;
        } else if (task->status() == falcon::TaskStatus::Failed) {
            std::cerr << "\n下载失败：" << task->error_message() << "\n";
            return 1;
        } else if (task->status() == falcon::TaskStatus::Cancelled) {
            std::cerr << "\n下载已取消\n";
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "错误：" << e.what() << "\n";
        return 1;
    }

    return 0;
}