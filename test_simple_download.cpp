#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <iostream>
#include <chrono>
#include <thread>

using namespace falcon;

int main() {
    try {
        // 创建下载引擎
        EngineConfig config;
        config.max_concurrent_tasks = 1;
        config.log_level = 4; // debug

        std::cout << "创建下载引擎..." << std::endl;
        DownloadEngine engine(config);

        // 配置下载选项
        DownloadOptions options;
        options.max_connections = 1;
        options.timeout_seconds = 30;
        options.resume_enabled = false;
        options.output_filename = "test_download.json";

        std::cout << "添加下载任务..." << std::endl;
        // 添加下载任务
        auto task = engine.add_task("https://httpbin.org/json", options);
        if (!task) {
            std::cerr << "错误：无法添加下载任务" << std::endl;
            return 1;
        }

        std::cout << "任务ID: " << task->id() << std::endl;
        std::cout << "任务状态: " << static_cast<int>(task->status()) << std::endl;

        // 手动启动任务
        std::cout << "启动任务..." << std::endl;
        if (!engine.start_task(task->id())) {
            std::cerr << "错误：无法启动下载任务" << std::endl;
            return 1;
        }

        // 等待下载完成
        std::cout << "等待下载完成..." << std::endl;
        int seconds = 0;
        while (!task->is_finished() && seconds < 60) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            seconds++;

            std::cout << "状态: " << static_cast<int>(task->status())
                      << ", 进度: " << task->progress() * 100 << "%"
                      << ", 下载: " << task->downloaded_bytes() << " bytes"
                      << std::endl;
        }

        // 检查最终状态
        if (task->status() == TaskStatus::Completed) {
            std::cout << "\n下载成功完成！" << std::endl;
            std::cout << "文件大小: " << task->downloaded_bytes() << " bytes" << std::endl;
        } else {
            std::cerr << "\n下载失败！状态: " << static_cast<int>(task->status()) << std::endl;
            if (!task->error_message().empty()) {
                std::cerr << "错误信息: " << task->error_message() << std::endl;
            }
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}