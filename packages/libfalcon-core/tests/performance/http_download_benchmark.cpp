/**
 * @file http_download_benchmark.cpp
 * @brief HTTP 下载性能基准测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/falcon.hpp>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>

class HTTPDownloadBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_unique<DownloadEngine>();
        engine->loadAllPlugins();

        // 创建输出目录
        std::filesystem::create_directories("benchmark_downloads");
    }

    void TearDown() override {
        std::filesystem::remove_all("benchmark_downloads");
        engine.reset();
    }

    std::unique_ptr<DownloadEngine> engine;
};

TEST_F(HTTPDownloadBenchmark, SingleDownloadSpeed) {
    const std::string testUrl = "https://httpbin.org/bytes/1048576";  // 1MB
    const int iterations = 10;
    std::vector<double> durations;

    for (int i = 0; i < iterations; ++i) {
        DownloadOptions options;
        options.output_path = "benchmark_downloads/single_" + std::to_string(i) + ".bin";

        auto task = engine->startDownload(testUrl, options);
        ASSERT_NE(task, nullptr);

        auto start = std::chrono::high_resolution_clock::now();

        task->start();

        // 等待完成或超时（最多30秒）
        for (int j = 0; j < 300; ++j) {
            if (task->getStatus() == TaskStatus::Completed ||
                task->getStatus() == TaskStatus::Failed) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        durations.push_back(duration.count() / 1000.0);

        // 验证下载是否成功
        EXPECT_EQ(task->getStatus(), TaskStatus::Completed)
            << "Download failed on iteration " << i;

        if (task->getStatus() == TaskStatus::Completed) {
            EXPECT_EQ(task->getTotalBytes(), 1048576);
            EXPECT_EQ(task->getDownloadedBytes(), 1048576);
        }

        task->cancel();
    }

    // 计算统计信息
    double totalDuration = 0;
    for (double d : durations) {
        totalDuration += d;
    }
    double avgDuration = totalDuration / iterations;
    double avgSpeed = 1048576.0 / avgDuration;  // MB/s

    // 打印性能结果
    std::cout << "\n=== 单文件下载性能测试 ===" << std::endl;
    std::cout << "文件大小: 1 MB" << std::endl;
    std::cout << "迭代次数: " << iterations << std::endl;
    std::cout << "平均下载时间: " << avgDuration << " 秒" << std::endl;
    std::cout << "平均下载速度: " << std::fixed << std::setprecision(2) << avgSpeed << " MB/s" << std::endl;
    std::cout << "最快下载: " << *std::min_element(durations.begin(), durations.end()) << " 秒" << std::endl;
    std::cout << "最慢下载: " << *std::max_element(durations.begin(), durations.end()) << " 秒" << std::endl;

    // 性能基准：平均速度应该至少 1 MB/s（在测试环境）
    EXPECT_GT(avgSpeed, 0.5) << "Average download speed too low";
}

TEST_F(HTTPDownloadBenchmark, ConcurrentDownloads) {
    const int numConcurrent = 10;
    const std::string testUrl = "https://httpbin.org/bytes/102400";  // 100KB
    std::vector<std::shared_ptr<DownloadTask>> tasks;
    std::atomic<int> completedCount{0};
    std::atomic<int> failedCount{0};

    // 添加事件监听器
    auto listener = std::make_shared<EventListener>();
    listener->onCompleted = [&](TaskId id) {
        completedCount++;
    };
    listener->onFailed = [&](TaskId id, const std::string& error) {
        failedCount++;
        std::cout << "Task " << id << " failed: " << error << std::endl;
    };

    auto start = std::chrono::high_resolution_clock::now();

    // 启动并发下载
    for (int i = 0; i < numConcurrent; ++i) {
        DownloadOptions options;
        options.output_path = "benchmark_downloads/concurrent_" + std::to_string(i) + ".bin";
        options.max_connections = 1;  // 每个任务单连接

        auto task = engine->startDownload(testUrl, options);
        if (task) {
            task->addEventListener(listener);
            tasks.push_back(task);
            task->start();
        }
    }

    // 等待所有任务完成（最多60秒）
    for (int i = 0; i < 600; ++i) {
        if (completedCount + failedCount >= numConcurrent) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto totalTime = std::chrono::duration_cast<std::chrono::seconds>(end - start);

    // 清理未完成的任务
    for (auto& task : tasks) {
        if (task->getStatus() == TaskStatus::Downloading) {
            task->cancel();
        }
    }

    // 计算总数据量
    size_t totalBytes = completedCount * 102400;  // 100KB per completed task
    double throughput = totalBytes / static_cast<double>(totalTime.count());

    std::cout << "\n=== 并发下载性能测试 ===" << std::endl;
    std::cout << "并发数: " << numConcurrent << std::endl;
    std::cout << "完成数: " << completedCount << std::endl;
    std::cout << "失败数: " << failedCount << std::endl;
    std::cout << "总耗时: " << totalTime.count() << " 秒" << std::endl;
    std::cout << "总数据量: " << totalBytes / 1024 << " KB" << std::endl;
    std::cout << "总吞吐量: " << std::fixed << std::setprecision(2) << throughput / 1024 << " KB/s" << std::endl;

    EXPECT_GT(completedCount, numConcurrent * 0.9);  // 至少90%成功率
    EXPECT_GT(throughput, 500);  // 至少 500 KB/s
}

TEST_F(HTTPDownloadBenchmark, MultiConnectionDownload) {
    const std::string testUrl = "https://httpbin.org/bytes/10485760";  // 10MB
    const std::vector<int> connectionCounts = {1, 2, 4, 8};

    for (int connections : connectionCounts) {
        DownloadOptions options;
        options.output_path = "benchmark_downloads/multi_" + std::to_string(connections) + ".bin";
        options.max_connections = connections;

        auto task = engine->startDownload(testUrl, options);
        ASSERT_NE(task, nullptr);

        auto start = std::chrono::high_resolution_clock::now();
        task->start();

        // 等待完成或超时（最多60秒）
        for (int i = 0; i < 600; ++i) {
            if (task->getStatus() == TaskStatus::Completed ||
                task->getStatus() == TaskStatus::Failed) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);

        task->cancel();

        double speed = 0;
        if (task->getStatus() == TaskStatus::Completed) {
            speed = task->getTotalBytes() / static_cast<double>(duration.count());
        }

        std::cout << "连接数: " << connections
                  << ", 耗时: " << duration.count() << "s"
                  << ", 速度: " << std::fixed << std::setprecision(1) << speed / 1024 << " KB/s" << std::endl;
    }
}

TEST_F(HTTPDownloadBenchmark, MemoryUsage) {
    // 内存使用测试 - 大量小文件
    const int numFiles = 100;
    const std::string testUrl = "https://httpbin.org/bytes/10240";  // 10KB

    std::vector<std::shared_ptr<DownloadTask>> tasks;

    auto start = std::chrono::high_resolution_clock::now();

    // 创建但不启动所有任务，测试内存占用
    for (int i = 0; i < numFiles; ++i) {
        DownloadOptions options;
        options.output_path = "benchmark_downloads/memory_" + std::to_string(i) + ".bin";

        auto task = engine->startDownload(testUrl, options);
        if (task) {
            tasks.push_back(task);
        }
    }

    auto creationEnd = std::chrono::high_resolution_clock::now();
    auto creationTime = std::chrono::duration_cast<std::chrono::milliseconds>(creationEnd - start);

    // 启动所有任务
    for (auto& task : tasks) {
        task->start();
    }

    // 等待所有任务完成或超时
    for (int i = 0; i < 600; ++i) {
        int completed = 0;
        for (auto& task : tasks) {
            if (task->getStatus() == TaskStatus::Completed ||
                task->getStatus() == TaskStatus::Failed) {
                completed++;
            }
        }
        if (completed >= numFiles) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto totalTime = std::chrono::duration_cast<std::chrono::seconds>(end - start);

    // 清理
    for (auto& task : tasks) {
        task->cancel();
    }

    std::cout << "\n=== 内存使用测试 ===" << std::endl;
    std::cout << "文件数量: " << numFiles << std::endl;
    std::cout << "创建时间: " << creationTime.count() << " ms" << std::endl;
    std::cout << "总耗时: " << totalTime.count() << " 秒" << std::endl;
    std::cout << "平均每文件创建时间: " << creationTime.count() / static_cast<double>(numFiles) << " ms" << std::endl;

    // 验证性能
    EXPECT_LT(creationTime.count() / numFiles, 10.0);  // 每个任务创建时间应小于10ms
}

// 性能基准测试宏
#define BENCHMARK_TEST(test_name, threshold_ms) \
    TEST_F(HTTPDownloadBenchmark, test_name) { \
        const std::string testUrl = "https://httpbin.org/json"; \
        const int iterations = 100; \
        \
        std::vector<double> times; \
        for (int i = 0; i < iterations; ++i) { \
            DownloadOptions options; \
            auto task = engine->startDownload(testUrl, options); \
            auto start = std::chrono::high_resolution_clock::now(); \
            task->start(); \
            \
            for (int j = 0; j < 50; ++j) { \
                if (task->getStatus() == TaskStatus::Completed) break; \
                std::this_thread::sleep_for(std::chrono::milliseconds(10)); \
            } \
            \
            auto end = std::chrono::high_resolution_clock::now(); \
            auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(); \
            times.push_back(time_ms); \
            task->cancel(); \
        } \
        \
        double avg_time = std::accumulate(times.begin(), times.end(), 0.0) / times.size(); \
        \
        EXPECT_LT(avg_time, threshold_ms) << "Average time " << avg_time << "ms exceeds threshold " << threshold_ms << "ms"; \
        std::cout << #test_name << ": " << avg_time << "ms" << std::endl; \
    }

// 定义性能基准
BENCHMARK_TEST(TaskCreationPerformance, 10)
BENCHMARK_TEST(TaskCancellationPerformance, 5)
BENCHMARK_TEST(StatusCheckPerformance, 1)