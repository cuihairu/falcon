/**
 * @file aria2_style_download.cpp
 * @brief aria2 风格下载示例
 * @author Falcon Team
 * @date 2025-12-25
 *
 * 展示如何使用 DownloadEngineV2 进行 aria2 风格的下载
 */

#include <falcon/download_engine_v2.hpp>
#include <falcon/file_hash.hpp>
#include <falcon/logger.hpp>

#include <iostream>
#include <chrono>
#include <thread>

using namespace falcon;

/**
 * @brief 简单的进度监听器
 */
class SimpleProgressListener {
public:
    void on_update(const RequestGroup::Progress& progress) {
        // 每 5% 更新一次显示
        if (progress.progress - last_progress_ >= 0.05 || progress.progress >= 1.0) {
            last_progress_ = progress.progress;

            std::cout << "\r[";
            int bar_width = 30;
            int filled = static_cast<int>(progress.progress * bar_width);
            for (int i = 0; i < bar_width; ++i) {
                std::cout << (i < filled ? "=" : " ");
            }
            std::cout << "] ";

            std::cout << static_cast<int>(progress.progress * 100) << "% ";
            std::cout << "(" << progress.downloaded / 1024 / 1024 << "MB";

            if (progress.total > 0) {
                std::cout << " / " << progress.total / 1024 / 1024 << "MB";
            }

            if (progress.speed > 0) {
                std::cout << " @ " << progress.speed / 1024 / 1024 << "MB/s";
            }

            std::cout << std::flush;
        }
    }

    void on_complete() {
        std::cout << "\n✓ 下载完成\n";
    }

private:
    double last_progress_ = 0.0;
};

/**
 * @brief 主函数
 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "用法: " << argv[0] << " <URL> [输出路径]\n";
        std::cout << "\n示例:\n";
        std::cout << "  " << argv[0] << " https://example.com/file.zip\n";
        std::cout << "  " << argv[0] << " https://example.com/file.zip /tmp/file.zip\n";
        return 1;
    }

    std::string url = argv[1];
    std::string output_path = (argc >= 3) ? argv[2] : "";

    // 配置下载引擎
    EngineConfigV2 config;
    config.max_concurrent_tasks = 5;     // 最多同时下载 5 个任务
    config.poll_timeout_ms = 100;        // 事件轮询超时
    config.global_speed_limit = 0;       // 不限速

    // 创建下载引擎
    DownloadEngineV2 engine(config);

    // 配置下载选项
    DownloadOptions options;
    options.max_connections = 8;         // 8 个并发连接
    options.timeout_seconds = 30;
    options.max_retries = 3;

    if (!output_path.empty()) {
        options.output_filename = output_path;
    }

    std::cout << "Falcon aria2 风格下载器\n";
    std::cout << "URL: " << url << "\n";
    std::cout << "并发连接: " << options.max_connections << "\n\n";

    // 添加下载任务
    TaskId task_id = engine.add_download(url, options);
    std::cout << "任务 ID: " << task_id << "\n";

    // 创建进度监听器
    SimpleProgressListener listener;

    // 在另一个线程中运行下载引擎
    std::thread engine_thread([&engine, &listener, task_id]() {
        engine.run();

        // 下载完成后
        auto* group_man = engine.request_group_man();
        auto* group = group_man->find_group(task_id);
        if (group && group->is_completed()) {
            listener.on_complete();
        }
    });

    // 监控进度
    auto* group_man = engine.request_group_man();

    while (engine_thread.joinable()) {
        auto* group = group_man->find_group(task_id);
        if (group) {
            auto progress = group->get_progress();
            listener.on_update(progress);

            if (group->is_completed() || group->status() == RequestGroupStatus::ERROR) {
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 等待引擎完成
    if (engine_thread.joinable()) {
        engine_thread.join();
    }

    // 获取统计信息
    auto stats = engine.get_statistics();
    std::cout << "\n统计信息:\n";
    std::cout << "  活动任务: " << stats.active_tasks << "\n";
    std::cout << "  等待任务: " << stats.waiting_tasks << "\n";
    std::cout << "  完成任务: " << stats.completed_tasks << "\n";
    std::cout << "  总下载: " << stats.total_downloaded / 1024 / 1024 << "MB\n";

    // 文件校验（如果指定了哈希值）
    if (argc >= 5) {
        std::string expected_hash = argv[3];
        std::string hash_type = (argc >= 5) ? argv[4] : "sha256";

        HashAlgorithm algo = HashAlgorithm::SHA256;
        if (hash_type == "md5") {
            algo = HashAlgorithm::MD5;
        } else if (hash_type == "sha1") {
            algo = HashAlgorithm::SHA1;
        }

        std::cout << "\n校验文件完整性...\n";
        HashVerifyCommand verifier(output_path, expected_hash, algo);
        if (verifier.execute()) {
            std::cout << "✓ 文件校验通过\n";
        } else {
            std::cout << "✗ 文件校验失败\n";
            return 1;
        }
    }

    return 0;
}
