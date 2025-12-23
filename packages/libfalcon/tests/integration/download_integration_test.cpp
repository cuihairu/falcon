// Falcon Download Integration Tests
// Copyright (c) 2025 Falcon Project

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <gtest/gtest.h>

#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace falcon;

namespace {

bool env_truthy(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    return std::string(value) == "1" || std::string(value) == "true" || std::string(value) == "TRUE";
}

std::string test_server_url() {
    const char* value = std::getenv("FALCON_TEST_SERVER_URL");
    return value ? std::string(value) : std::string{};
}

} // namespace

class DownloadIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        test_dir_ = std::filesystem::temp_directory_path() / "falcon_test";
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        // Clean up test directory
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    std::filesystem::path test_dir_;
};

TEST_F(DownloadIntegrationTest, LocalHttpDownloadFile) {
    const std::string base_url = test_server_url();
    if (base_url.empty()) {
        GTEST_SKIP() << "Set FALCON_TEST_SERVER_URL to enable local HTTP integration tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "test_download.json";
    options.max_connections = 1;
    options.timeout_seconds = 10;
    options.resume_enabled = false;

    std::string url = base_url + "/test.json";
    auto task = engine.add_task(url, options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    ASSERT_TRUE(task->wait_for(std::chrono::seconds(20)));
    EXPECT_EQ(task->status(), TaskStatus::Completed);

    std::filesystem::path output_file = test_dir_ / "test_download.json";
    ASSERT_TRUE(std::filesystem::exists(output_file));

    std::ifstream file(output_file);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\"hello\""), std::string::npos);
}

TEST_F(DownloadIntegrationTest, HttpDownloadFile) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "test_download.json";
    options.max_connections = 1;
    options.timeout_seconds = 30;
    options.resume_enabled = false;

    // Test with httpbin.org
    std::string url = "https://httpbin.org/json";
    auto task = engine.add_task(url, options);

    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->id(), 1);
    EXPECT_EQ(task->url(), url);
    EXPECT_EQ(task->status(), TaskStatus::Pending);

    ASSERT_TRUE(engine.start_task(task->id()));
    ASSERT_TRUE(task->wait_for(std::chrono::seconds(60)));

    // Check result
    EXPECT_EQ(task->status(), TaskStatus::Completed);

    // Verify file exists
    std::filesystem::path output_file = test_dir_ / "test_download.json";
    EXPECT_TRUE(std::filesystem::exists(output_file));

    // Check file size (httpbin.org/json returns known content)
    auto file_size = std::filesystem::file_size(output_file);
    EXPECT_GT(file_size, 0);

    // Verify content
    std::ifstream file(output_file);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    EXPECT_FALSE(content.empty());
    EXPECT_TRUE(content.find("\"slideshow\"") != std::string::npos);
}

TEST_F(DownloadIntegrationTest, MultipleDownloads) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.max_connections = 2;
    options.timeout_seconds = 30;
    options.resume_enabled = false;

    std::vector<std::string> urls = {
        "https://httpbin.org/uuid",
        "https://httpbin.org/ip",
        "https://httpbin.org/user-agent"
    };

    std::vector<std::string> filenames = {
        "uuid.json",
        "ip.json",
        "user-agent.json"
    };

    std::vector<DownloadTask::Ptr> tasks;

    // Add multiple tasks
    for (size_t i = 0; i < urls.size(); ++i) {
        options.output_filename = filenames[i];
        auto task = engine.add_task(urls[i], options);
        ASSERT_NE(task, nullptr);
        ASSERT_TRUE(engine.start_task(task->id()));
        tasks.push_back(task);
    }

    // Wait for all to complete
    for (auto& task : tasks) {
        ASSERT_TRUE(task->wait_for(std::chrono::seconds(60)));
        EXPECT_EQ(task->status(), TaskStatus::Completed);

        // Verify file exists
        std::filesystem::path output_file = test_dir_ / filenames[task->id() - 1];
        EXPECT_TRUE(std::filesystem::exists(output_file));
        EXPECT_GT(std::filesystem::file_size(output_file), 0);
    }
}

TEST_F(DownloadIntegrationTest, PauseAndResume) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "pause_test.bin";
    options.max_connections = 1;
    options.timeout_seconds = 30;
    options.resume_enabled = true;

    // Use a larger file for pause/resume test
    std::string url = "https://httpbin.org/bytes/1024"; // 1KB file
    auto task = engine.add_task(url, options);

    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    // Wait a bit then pause
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(engine.pause_task(task->id()));
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // Verify partial file exists
    std::filesystem::path output_file = test_dir_ / "pause_test.bin";
    if (std::filesystem::exists(output_file)) {
        auto partial_size = std::filesystem::file_size(output_file);
        EXPECT_GT(partial_size, 0);
        EXPECT_LT(partial_size, 1024);
    }

    // Resume
    EXPECT_TRUE(engine.resume_task(task->id()));

    // Wait for completion
    ASSERT_TRUE(task->wait_for(std::chrono::seconds(60)));

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_TRUE(std::filesystem::exists(output_file));
    EXPECT_EQ(std::filesystem::file_size(output_file), 1024);
}

TEST_F(DownloadIntegrationTest, CancelDownload) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "cancel_test.bin";
    options.max_connections = 1;
    options.timeout_seconds = 30;

    // Use a large file
    std::string url = "https://httpbin.org/bytes/1048576"; // 1MB file
    auto task = engine.add_task(url, options);

    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    // Wait a bit then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(engine.cancel_task(task->id()));
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);

    // File might exist but should be incomplete
    std::filesystem::path output_file = test_dir_ / "cancel_test.bin";
    if (std::filesystem::exists(output_file)) {
        auto file_size = std::filesystem::file_size(output_file);
        EXPECT_LT(file_size, 1048576);
    }
}

TEST_F(DownloadIntegrationTest, GetStatistics) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.max_connections = 2;
    options.timeout_seconds = 30;

    // Add some tasks
    std::vector<std::string> urls = {
        "https://httpbin.org/uuid",
        "https://httpbin.org/ip",
        "https://httpbin.org/user-agent"
    };

    for (const auto& url : urls) {
        auto task = engine.add_task(url, options);
        ASSERT_NE(task, nullptr);
    }

    // Get statistics
    auto tasks = engine.get_all_tasks();
    EXPECT_EQ(tasks.size(), 3);

    auto active_tasks = engine.get_active_tasks();
    EXPECT_GE(active_tasks.size(), 0);

    auto total_speed = engine.get_total_speed();
    EXPECT_GE(total_speed, 0);
}
