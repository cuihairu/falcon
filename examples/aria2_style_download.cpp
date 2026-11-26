/**
 * @file aria2_style_download.cpp
 * @brief aria2 风格下载示例（单一 V1 引擎）
 * @author Falcon Team
 * @date 2025-12-25
 *
 * 展示如何用 falcon::DownloadEngine 配置多连接、限速与多个任务。
 */

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/logger.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>

using namespace falcon;

namespace {

class ProgressListener : public IEventListener {
public:
    void on_status_changed(TaskId task_id, TaskStatus old_status,
                           TaskStatus new_status) override {
        std::cout << "\n[Task " << task_id << "] " << to_string(old_status)
                  << " -> " << to_string(new_status) << "\n";
    }

    void on_progress(const ProgressInfo& info) override {
        std::cout << "\r[Task " << info.task_id << "] " << std::fixed
                  << std::setprecision(1) << (info.progress * 100) << "% ("
                  << info.downloaded_bytes << " / " << info.total_bytes
                  << " bytes) Speed: " << info.speed << " B/s"
                  << std::flush;
    }

    void on_completed(TaskId task_id, const std::string& output_path) override {
        std::cout << "\n[Task " << task_id << "] Completed: " << output_path
                  << "\n";
    }

    void on_error(TaskId task_id, const std::string& error_message) override {
        std::cout << "\n[Task " << task_id << "] Error: " << error_message
                  << "\n";
    }
};

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "用法: " << argv[0] << " <URL> [输出目录]\n";
        return 1;
    }

    EngineConfig config;
    config.max_concurrent_tasks = 5;
    DownloadEngine engine(config);

    ProgressListener listener;
    engine.add_listener(&listener);

    DownloadOptions options;
    options.max_connections = 8;
    options.timeout_seconds = 30;
    options.max_retries = 3;
    options.resume_enabled = true;
    if (argc >= 3) options.output_directory = argv[2];

    std::cout << "Falcon aria2 风格下载器（V1 引擎）\n";
    std::cout << "URL: " << argv[1] << "\n";
    std::cout << "并发连接: " << options.max_connections << "\n\n";

    auto task = engine.add_task(argv[1], options);
    if (!task) {
        std::cerr << "URL is not supported\n";
        return 1;
    }
    if (!engine.start_task(task->id())) {
        std::cerr << "Failed to start download task\n";
        return 1;
    }

    task->wait();
    const auto progress = task->get_progress_info();
    std::cout << "\n任务 " << task->id() << " 最终状态: "
              << to_string(task->status()) << "\n";
    std::cout << "已下载: " << progress.downloaded_bytes << " bytes"
              << "，耗时: "
              << std::chrono::duration_cast<std::chrono::seconds>(progress.elapsed).count()
              << " 秒\n";

    return task->status() == TaskStatus::Completed ? 0 : 1;
}
