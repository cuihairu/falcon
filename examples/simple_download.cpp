/**
 * @file simple_download.cpp
 * @brief 简单下载示例
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/download_engine.hpp>
#include <falcon/event_listener.hpp>
#include <iostream>
#include <iomanip>
#include <thread>

class SimpleListener : public falcon::IEventListener {
public:
    void on_status_changed(falcon::TaskId task_id,
                           falcon::TaskStatus old_status,
                           falcon::TaskStatus new_status) override {
        std::cout << "\n[Task " << task_id << "] "
                  << falcon::to_string(old_status)
                  << " -> " << falcon::to_string(new_status) << "\n";
    }

    void on_progress(const falcon::ProgressInfo& info) override {
        std::cout << "\r[Task " << info.task_id << "] "
                  << std::fixed << std::setprecision(1)
                  << (info.progress * 100) << "% "
                  << "(" << info.downloaded_bytes << " / " << info.total_bytes << " bytes)"
                  << " Speed: " << info.speed << " B/s";
        std::cout.flush();
    }

    void on_error(falcon::TaskId task_id, const std::string& error_message) override {
        std::cout << "\n[Task " << task_id << "] Error: " << error_message << "\n";
    }

    void on_completed(falcon::TaskId task_id, const std::string& output_path) override {
        std::cout << "\n[Task " << task_id << "] Completed: " << output_path << "\n";
    }

    void on_file_info(falcon::TaskId task_id, const falcon::FileInfo& info) override {
        std::cout << "\n[Task " << task_id << "] File Info:\n"
                  << "  URL: " << info.url << "\n"
                  << "  Filename: " << info.filename << "\n"
                  << "  Size: " << info.total_size << " bytes\n"
                  << "  Content-Type: " << info.content_type << "\n"
                  << "  Supports Resume: " << (info.supports_resume ? "Yes" : "No") << "\n";
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <URL>\n";
        return 1;
    }

    try {
        // 创建下载引擎
        falcon::DownloadEngine engine;

        // 设置事件监听器
        SimpleListener listener;
        engine.add_listener(&listener);

        // 创建下载选项
        falcon::DownloadOptions options;
        options.output_directory = "./downloads";
        options.max_connections = 4;
        options.resume_enabled = true;
        options.user_agent = "Falcon/0.1.0";

        // 添加下载任务
        auto task = engine.add_task(argv[1], options);
        if (!task) {
            std::cerr << "Failed to add download task. URL not supported.\n";
            return 1;
        }

        std::cout << "Download started. Task ID: " << task->id() << "\n";
        std::cout << "URL: " << task->url() << "\n";

        // 等待下载完成
        task->wait();

        // 输出最终统计
        auto stats = task->get_progress_info();
        std::cout << "\n\nDownload finished!\n";
        std::cout << "Total downloaded: " << stats.downloaded_bytes << " bytes\n";
        std::cout << "Time elapsed: "
                  << std::chrono::duration_cast<std::chrono::seconds>(stats.elapsed).count()
                  << " seconds\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}