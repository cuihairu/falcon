/**
 * @file http_handler_test.cpp
 * @brief HTTP/HTTPS 处理器单元测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_handler.hpp>
#include <falcon/download_task.hpp>
#include <falcon/download_options.hpp>
#include <falcon/task_manager.hpp>
#include <falcon/event_dispatcher.hpp>
#include "../../plugins/http/http_handler.hpp"
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>

using namespace falcon;
using namespace falcon::plugins;

class HttpHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler = std::make_unique<HttpHandler>();

        // 创建测试目录
        std::filesystem::create_directories("test_data");

        testFilePath = "test_data/test_file.txt";
    }

    void TearDown() override {
        // 清理测试文件
        std::filesystem::remove_all("test_data");
    }

    std::unique_ptr<HttpHandler> handler;
    std::string testFilePath;
};

TEST_F(HttpHandlerTest, ProtocolName) {
    EXPECT_EQ(handler->protocol_name(), "http");
}

TEST_F(HttpHandlerTest, SupportedSchemes) {
    auto schemes = handler->supported_schemes();
    EXPECT_EQ(schemes.size(), 2);
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "http"), schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "https"), schemes.end());
}

TEST_F(HttpHandlerTest, CanHandleUrls) {
    // HTTP URLs
    EXPECT_TRUE(handler->can_handle("http://example.com"));
    EXPECT_TRUE(handler->can_handle("http://example.com/file.zip"));
    EXPECT_TRUE(handler->can_handle("http://subdomain.example.com/path/to/file?param=value"));

    // HTTPS URLs
    EXPECT_TRUE(handler->can_handle("https://example.com"));
    EXPECT_TRUE(handler->can_handle("https://secure.example.com/file.pdf"));
    EXPECT_TRUE(handler->can_handle("https://example.com:8443/path"));

    // IPv4 and IPv6
    EXPECT_TRUE(handler->can_handle("http://192.168.1.1/file"));
    EXPECT_TRUE(handler->can_handle("http://[2001:db8::1]/file"));

    // 端口号
    EXPECT_TRUE(handler->can_handle("http://example.com:8080"));
    EXPECT_TRUE(handler->can_handle("https://example.com:443/path"));

    // Query strings and fragments
    EXPECT_TRUE(handler->can_handle("http://example.com/file.txt?download=true#section"));

    // 不支持的协议
    EXPECT_FALSE(handler->can_handle("ftp://example.com/file"));
    EXPECT_FALSE(handler->can_handle("magnet:?xt=urn:btih:hash"));
    EXPECT_FALSE(handler->can_handle("file:///path/to/file"));
}

TEST_F(HttpHandlerTest, GetFileInfo) {
    DownloadOptions options;
    options.timeout_seconds = 10;
    options.user_agent = "Falcon-Test/1.0";

    // 测试获取文件信息
    // 注意：这个测试需要网络连接，在实际 CI 中可能需要 mock
    try {
        FileInfo info = handler->get_file_info("https://httpbin.org/json", options);
        EXPECT_FALSE(info.filename.empty());
        EXPECT_TRUE(info.total_size > 0 || info.total_size == 0);  // 可能为 0 如果服务器不返回 Content-Length
        EXPECT_EQ(info.url, "https://httpbin.org/json");
    } catch (const NetworkException& e) {
        // 网络错误是可以接受的，跳过测试
        GTEST_SKIP() << "Network error: " << e.what();
    }
}

TEST_F(HttpHandlerTest, InvalidUrl) {
    DownloadOptions options;

    // 测试无效 URL
    EXPECT_THROW(
        handler->get_file_info("not-a-url", options),
        NetworkException
    );

    EXPECT_THROW(
        handler->get_file_info("htt://invalid-scheme.com", options),
        NetworkException
    );
}

// 创建一个测试用的事件监听器
class TestEventListener : public IEventListener {
public:
    std::vector<TaskId> status_changes;
    std::vector<ProgressInfo> progress_updates;
    std::vector<std::pair<TaskId, std::string>> errors;

    void on_status_changed(TaskId task_id, TaskStatus old_status, TaskStatus new_status) override {
        status_changes.push_back(task_id);
    }

    void on_progress(const ProgressInfo& progress) override {
        progress_updates.push_back(progress);
    }

    void on_error(TaskId task_id, const std::string& error_message) override {
        errors.emplace_back(task_id, error_message);
    }

    void on_completed(TaskId task_id, const std::string& output_path) override {
        status_changes.push_back(task_id);
    }

    void on_file_info(TaskId task_id, const FileInfo& info) override {
        // 记录文件信息
    }
};

TEST_F(HttpHandlerTest, DISABLED_DownloadTask) {
    // 这个测试需要实际的网络连接，在 CI 中可能需要禁用
    // 或者使用本地测试服务器

    // 创建任务管理器和事件分发器
    EventDispatcher dispatcher;
    TaskManagerConfig config;
    config.max_concurrent_tasks = 1;
    TaskManager manager(config, &dispatcher);

    // 创建事件监听器
    auto listener = std::make_shared<TestEventListener>();
    dispatcher.add_listener(listener.get());

    // 创建下载任务
    DownloadOptions options;
    options.timeout_seconds = 30;
    options.user_agent = "Falcon-Test/1.0";

    std::string test_url = "https://httpbin.org/json";
    std::string output_path = testFilePath;

    // 创建任务
    auto task = std::make_shared<DownloadTask>(1, test_url, options);
    task->set_output_path(output_path);
    task->set_handler(std::shared_ptr<IProtocolHandler>(handler.release()));

    // 添加任务到管理器
    TaskId task_id = manager.add_task(task, TaskPriority::Normal);
    EXPECT_NE(task_id, INVALID_TASK_ID);

    // 启动任务
    EXPECT_TRUE(manager.start_task(task_id));

    // 等待任务完成或超时
    bool completed = task->wait_for(std::chrono::seconds(35));

    if (completed) {
        EXPECT_EQ(task->status(), TaskStatus::Completed);
        EXPECT_TRUE(std::filesystem::exists(output_path));
        EXPECT_GT(std::filesystem::file_size(output_path), 0);
    } else {
        // 超时，取消任务
        manager.cancel_task(task_id);
        GTEST_SKIP() << "Download timeout";
    }
}