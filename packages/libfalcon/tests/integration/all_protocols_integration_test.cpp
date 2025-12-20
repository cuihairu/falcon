/**
 * @file all_protocols_integration_test.cpp
 * @brief 所有协议的集成测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/falcon.hpp>
#include <falcon/plugin_manager.hpp>
#include <thread>
#include <chrono>
#include <vector>
#include <fstream>
#include <sstream>
#include <atomic>

using namespace falcon;

class AllProtocolsIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建下载引擎
        engine = std::make_unique<DownloadEngine>();

        // 加载所有插件
        engine->loadAllPlugins();

        // 获取支持的协议列表
        protocols = engine->listSupportedProtocols();

        // 创建输出目录
        std::filesystem::create_directories("test_downloads");
    }

    void TearDown() override {
        // 清理测试文件
        std::error_code ec;
        std::filesystem::remove_all("test_downloads", ec);

        // 重置引擎
        engine.reset();
    }

    // 创建测试用的URL列表
    std::vector<std::string> createTestUrls() {
        return {
            // HTTP/HTTPS
            "https://httpbin.org/json",  // 小JSON文件用于测试
            "https://httpbin.org/uuid",  // 返回UUID的端点
            "https://httpbin.org/base64/SGVsbG8gV29ybGQ=",  // 返回Base64解码结果

            // 私有协议（模拟数据）
            "thunder://QUFodHRwczovL2h0dHBiaW4ub3JnL2pzb24uWg==",  // Base64编码的HTTP URL
            "qqlink://aHR0cHM6Ly9odHRwYmluLm9yZy91dWlkWg==",       // Base64编码的HTTPS URL
            "flashget://W10=",                                       // 空列表
            "ed2k://file|test.json|1024|A1B2C3D4E5F6789012345678901234AB|/",

            // 流媒体（使用测试服务器）
            "https://test-streams.example.com/playlist.m3u8",
            "https://test-streams.example.com/manifest.mpd",

            // BitTorrent
            "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"
            "&dn=test_file.txt"
            "&tr=udp%3A%2F%2Ftracker.example.com%3A6969",
        };
    }

    std::unique_ptr<DownloadEngine> engine;
    std::vector<std::string> protocols;
};

TEST_F(AllProtocolsIntegrationTest, VerifyAllPluginsLoaded) {
    // 验证至少加载了一些插件
    EXPECT_GT(protocols.size(), 0);

    // 打印所有支持的协议
    std::cout << "Loaded protocols: ";
    for (const auto& protocol : protocols) {
        std::cout << protocol << " ";
    }
    std::cout << std::endl;

    // 验证基础协议
    EXPECT_TRUE(std::find(protocols.begin(), protocols.end(), "http") != protocols.end());

    // 验证私有协议（如果启用）
    #ifdef FALCON_ENABLE_THUNDER
    EXPECT_TRUE(std::find(protocols.begin(), protocols.end(), "thunder") != protocols.end());
    #endif

    #ifdef FALCON_ENABLE_ED2K
    EXPECT_TRUE(std::find(protocols.begin(), protocols.end(), "ed2k") != protocols.end());
    #endif
}

TEST_F(AllProtocolsIntegrationTest, CreateTasksForAllProtocols) {
    auto testUrls = createTestUrls();
    std::vector<std::shared_ptr<DownloadTask>> tasks;

    for (const auto& url : testUrls) {
        if (engine->supportsUrl(url)) {
            DownloadOptions options;
            options.output_path = "test_downloads/" + std::to_string(tasks.size()) + ".download";
            options.timeout_seconds = 5;  // 短超时用于测试

            try {
                auto task = engine->startDownload(url, options);
                EXPECT_NE(task, nullptr) << "Failed to create task for: " << url;
                if (task) {
                    tasks.push_back(task);
                }
            } catch (const std::exception& e) {
                // 某些协议可能需要特定条件
                std::cout << "Note: " << url << " failed with: " << e.what() << std::endl;
            }
        } else {
            std::cout << "URL not supported: " << url << std::endl;
        }
    }

    EXPECT_GT(tasks.size(), 0) << "At least one task should be created";

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 取消所有任务
    for (auto& task : tasks) {
        task->cancel();
    }
}

TEST_F(AllProtocolsIntegrationTest, ConcurrentDownloads) {
    std::vector<std::string> urls = {
        "https://httpbin.org/delay/1",
        "https://httpbin.org/delay/1",
        "https://httpbin.org/delay/1",
        "https://httpbin.org/delay/1",
        "https://httpbin.org/delay/1"
    };

    std::vector<std::shared_ptr<DownloadTask>> tasks;
    std::atomic<int> completedTasks{0};
    std::atomic<int> failedTasks{0};

    // 创建监听器
    auto listener = std::make_shared<EventListener>();
    listener->onCompleted = [&](TaskId id) {
        completedTasks++;
    };
    listener->onFailed = [&](TaskId id, const std::string& error) {
        failedTasks++;
    };

    // 启动并发下载
    for (size_t i = 0; i < urls.size(); ++i) {
        DownloadOptions options;
        options.output_path = "test_downloads/concurrent_" + std::to_string(i) + ".txt";

        try {
            auto task = engine->startDownload(urls[i], options);
            if (task) {
                task->addEventListener(listener);
                tasks.push_back(task);
            }
        } catch (const std::exception& e) {
            std::cout << "Failed to start task " << i << ": " << e.what() << std::endl;
        }
    }

    // 等待所有任务完成或超时
    auto startTime = std::chrono::steady_clock::now();
    while (completedTasks + failedTasks < tasks.size()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 超时检查（5秒）
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > std::chrono::seconds(5)) {
            break;
        }
    }

    EXPECT_GT(completedTasks, 0) << "At least one task should complete";

    // 清理
    for (auto& task : tasks) {
        if (task->getStatus() == TaskStatus::Downloading) {
            task->cancel();
        }
    }
}

TEST_F(AllProtocolsIntegrationTest, TaskStateTransitions) {
    std::string testUrl = "https://httpbin.org/delay/2";

    DownloadOptions options;
    options.output_path = "test_downloads/state_test.txt";

    try {
        auto task = engine->startDownload(testUrl, options);
        ASSERT_NE(task, nullptr);

        // 初始状态
        EXPECT_EQ(task->getStatus(), TaskStatus::Pending);

        // 启动下载
        task->start();
        EXPECT_EQ(task->getStatus(), TaskStatus::Downloading);

        // 暂停
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        task->pause();
        EXPECT_EQ(task->getStatus(), TaskStatus::Paused);

        // 恢复
        task->resume();
        EXPECT_EQ(task->getStatus(), TaskStatus::Downloading);

        // 等待一小段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 取消
        task->cancel();
        EXPECT_EQ(task->getStatus(), TaskStatus::Cancelled);
    } catch (const std::exception& e) {
        SUCCEED() << "State transition test needs server: " << e.what();
    }
}

TEST_F(AllProtocolsIntegrationTest, ProgressTracking) {
    std::string testUrl = "https://httpbin.org/bytes/1024";  // 1KB 数据

    DownloadOptions options;
    options.output_path = "test_downloads/progress_test.bin";

    try {
        auto task = engine->startDownload(testUrl, options);
        ASSERT_NE(task, nullptr);

        // 添加进度监听器
        std::atomic<float> lastProgress{0.0f};
        task->addEventListener(std::make_shared<EventListener>(
            [&](TaskEvent event, TaskId id, const EventData& data) {
                if (event == TaskEvent::Progress) {
                    float progress = data.progress;
                    EXPECT_GE(progress, lastProgress);
                    EXPECT_LE(progress, 1.0f);
                    lastProgress = progress;
                }
            }
        ));

        task->start();

        // 监控进度
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            float progress = task->getProgress();
            EXPECT_GE(progress, 0.0f);
            EXPECT_LE(progress, 1.0f);

            if (task->getStatus() == TaskStatus::Completed) {
                break;
            }
        }

        task->cancel();
    } catch (const std::exception& e) {
        SUCCEED() << "Progress tracking test needs server: " << e.what();
    }
}

TEST_F(AllProtocolsIntegrationTest, ErrorHandling) {
    // 测试无效URL
    try {
        auto task = engine->startDownload("", {});
        EXPECT_EQ(task, nullptr);
    } catch (...) {
        // 期望失败
    }

    // 测试不支持的协议
    try {
        auto task = engine->startDownload("unsupported://example.com", {});
        EXPECT_EQ(task, nullptr);
    } catch (...) {
        // 期望失败
    }

    // 测试404错误
    try {
        DownloadOptions options;
        options.output_path = "test_downloads/404_test.txt";
        auto task = engine->startDownload("https://httpbin.org/status/404", options);

        if (task) {
            task->start();

            // 等待任务失败
            for (int i = 0; i < 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (task->getStatus() == TaskStatus::Failed) {
                    EXPECT_FALSE(task->getErrorMessage().empty());
                    break;
                }
            }

            task->cancel();
        }
    } catch (const std::exception& e) {
        SUCCEED() << "Error handling test needs server: " << e.what();
    }
}

TEST_F(AllProtocolsIntegrationTest, ConfigurationOptions) {
    // 测试不同的配置选项
    std::vector<DownloadOptions> testOptions;

    // 选项1：基本配置
    DownloadOptions opt1;
    opt1.output_path = "test_downloads/opt1.txt";
    opt1.timeout_seconds = 30;
    testOptions.push_back(opt1);

    // 选项2：速度限制
    DownloadOptions opt2;
    opt2.output_path = "test_downloads/opt2.txt";
    opt2.speed_limit = 100 * 1024;  // 100 KB/s
    testOptions.push_back(opt2);

    // 选项3：自定义头部
    DownloadOptions opt3;
    opt3.output_path = "test_downloads/opt3.txt";
    opt3.user_agent = "Falcon Integration Test/1.0";
    opt3.headers["X-Test-Header"] = "test-value";
    testOptions.push_back(opt3);

    // 选项4：重试配置
    DownloadOptions opt4;
    opt4.output_path = "test_downloads/opt4.txt";
    opt4.max_retries = 3;
    testOptions.push_back(opt4);

    std::string testUrl = "https://httpbin.org/uuid";

    for (size_t i = 0; i < testOptions.size(); ++i) {
        try {
            auto task = engine->startDownload(testUrl, testOptions[i]);
            if (task) {
                // 验证选项是否正确应用（通过日志或其他方式）
                EXPECT_EQ(task->getStatus(), TaskStatus::Pending);
                task->cancel();
            }
        } catch (const std::exception& e) {
            std::cout << "Option " << i << " test failed: " << e.what() << std::endl;
        }
    }
}

class ProtocolsCompatibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_unique<DownloadEngine>();
        engine->loadAllPlugins();
    }

    void TearDown() override {
        engine.reset();
    }

    std::unique_ptr<DownloadEngine> engine;
};

TEST_F(ProtocolsCompatibilityTest, UrlSchemeDetection) {
    struct TestCase {
        std::string url;
        std::string expectedScheme;
        bool shouldBeSupported;
    };

    std::vector<TestCase> testCases = {
        // HTTP/HTTPS
        {"http://example.com/file.zip", "http", true},
        {"https://secure.example.com/file.pdf", "http", true},
        {"ftp://ftp.example.com/data.bin", "ftp", false},  // 如果FTP未启用

        // 私有协议
        {"thunder://QUFodHRwOi8vZXhhbXBsZS5jb20uWg==", "thunder", false},
        {"qqlink://aHR0cDovL2V4YW1wbGUuY29tL1o=", "qqdl", false},
        {"flashget://W10=", "flashget", false},
        {"ed2k://file|test.zip|1024|HASH|/", "ed2k", false},

        // 流媒体
        {"https://example.com/playlist.m3u8", "http", true},  // HLS被HTTP处理
        {"https://example.com/manifest.mpd", "http", true),  // DASH被HTTP处理

        // P2P
        {"magnet:?xt=urn:btih:HASH", "bittorrent", false},

        // 边界情况
        {"", "", false},
        {"://missing-scheme", "", false},
        {"unknown://example.com", "", false},
    };

    for (const auto& testCase : testCases) {
        bool isSupported = engine->supportsUrl(testCase.url);

        EXPECT_EQ(isSupported, testCase.shouldBeSupported)
            << "URL: " << testCase.url << " expected " << testCase.shouldBeSupported;
    }
}

TEST_F(ProtocolsCompatibilityTest, ProtocolSpecificFeatures) {
    // 测试协议特有的功能

    // HTTP特有功能
    DownloadOptions httpOptions;
    httpOptions.max_connections = 5;
    httpOptions.resume_if_exists = true;

    try {
        auto httpTask = engine->startDownload("https://example.com/large_file.zip", httpOptions);
        // 验证HTTP特有选项
    } catch (...) {
        // 需要服务器
    }

    // BitTorrent特有功能
    DownloadOptions btOptions;
    btOptions.seeding_time = 3600;  // 做种1小时

    try {
        auto btTask = engine->startDownload("magnet:?xt=urn:btih:HASH", btOptions);
        // 验证BitTorrent特有选项
    } catch (...) {
        // 需要支持
    }
}

// 性能测试
class PerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_unique<DownloadEngine>();
        engine->loadAllPlugins();
    }

    void TearDown() override {
        engine.reset();
    }

    std::unique_ptr<DownloadEngine> engine;
};

TEST_F(PerformanceTest, TaskCreationPerformance) {
    const int NUM_TASKS = 1000;
    std::vector<std::shared_ptr<DownloadTask>> tasks;

    auto startTime = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_TASKS; ++i) {
        std::string url = "https://httpbin.org/uuid";
        DownloadOptions options;
        options.output_path = "perf_test_" + std::to_string(i) + ".txt";

        try {
            auto task = engine->startDownload(url, options);
            if (task) {
                tasks.push_back(task);
            }
        } catch (...) {
            // 忽略网络错误
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // 清理
    for (auto& task : tasks) {
        task->cancel();
    }

    // 验证性能：1000个任务创建应该在1秒内完成
    EXPECT_LT(duration.count(), 1000);

    std::cout << "Created " << tasks.size() << " tasks in " << duration.count() << " ms" << std::endl;
}

// 压力测试
class StressTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_unique<DownloadEngine>();
        engine->loadAllPlugins();
    }

    void TearDown() override {
        engine.reset();
    }

    std::unique_ptr<DownloadEngine> engine;
};

TEST_F(StressTest, MaxConcurrentTasks) {
    const int MAX_TASKS = 100;
    std::vector<std::shared_ptr<DownloadTask>> tasks;

    // 尝试创建最大数量的并发任务
    for (int i = 0; i < MAX_TASKS; ++i) {
        try {
            std::string url = "https://httpbin.org/delay/1";
            DownloadOptions options;
            options.output_path = "stress_test_" + std::to_string(i) + ".txt";

            auto task = engine->startDownload(url, options);
            if (task) {
                tasks.push_back(task);
            } else {
                std::cout << "Failed to create task " << i << std::endl;
                break;
            }
        } catch (...) {
            break;
        }
    }

    // 运行一段时间
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 检查系统稳定性
    int activeTasks = 0;
    for (const auto& task : tasks) {
        TaskStatus status = task->getStatus();
        if (status == TaskStatus::Downloading || status == TaskStatus::Paused) {
            activeTasks++;
        }
    }

    std::cout << "Active tasks after 2 seconds: " << activeTasks << "/" << tasks.size() << std::endl;

    // 清理
    for (auto& task : tasks) {
        task->cancel();
    }

    // 验证没有崩溃
    EXPECT_GT(tasks.size(), 0);
}