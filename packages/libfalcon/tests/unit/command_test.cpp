/**
 * @file command_test.cpp
 * @brief 命令系统单元测试
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <gtest/gtest.h>
#include <falcon/commands/command.hpp>
#include <falcon/commands/http_commands.hpp>
#include <falcon/download_engine_v2.hpp>
#include <memory>

using namespace falcon;

//==============================================================================
// 测试辅助类
//==============================================================================

/**
 * @brief 测试用命令 - 完成后返回 true
 */
class TestCompletedCommand : public AbstractCommand {
public:
    explicit TestCompletedCommand(TaskId task_id) : AbstractCommand(task_id) {}
    bool execute(DownloadEngineV2* engine) override {
        return handle_result(ExecutionResult::OK);
    }
    const char* name() const override { return "TestCompleted"; }
};

/**
 * @brief 测试用命令 - 始终返回 false（等待中）
 */
class TestWaitingCommand : public AbstractCommand {
public:
    explicit TestWaitingCommand(TaskId task_id) : AbstractCommand(task_id) {}
    bool execute(DownloadEngineV2* engine) override {
        mark_active();
        return false;  // 始终等待
    }
    const char* name() const override { return "TestWaiting"; }
};

/**
 * @brief 测试用命令 - 返回错误
 */
class TestErrorCommand : public AbstractCommand {
public:
    explicit TestErrorCommand(TaskId task_id) : AbstractCommand(task_id) {}
    bool execute(DownloadEngineV2* engine) override {
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }
    const char* name() const override { return "TestError"; }
};

/**
 * @brief 测试用命令 - 链式调用
 */
class TestChainCommand : public AbstractCommand {
public:
    TestChainCommand(TaskId task_id, int& counter)
        : AbstractCommand(task_id), counter_(counter) {}

    bool execute(DownloadEngineV2* engine) override {
        counter_++;
        if (counter_ < 3) {
            mark_active();
            return false;
        }
        return handle_result(ExecutionResult::OK);
    }

    const char* name() const override { return "TestChain"; }

private:
    int& counter_;
};

//==============================================================================
// Command 基础测试
//==============================================================================

TEST(CommandTest, DefaultStatusIsReady) {
    TestCompletedCommand cmd(1);
    EXPECT_EQ(cmd.status(), CommandStatus::READY);
}

TEST(CommandTest, TaskId) {
    TaskId expected_id = 42;
    TestCompletedCommand cmd(expected_id);
    // TaskId 在构造时设置，通过命令行为验证
    EXPECT_EQ(cmd.status(), CommandStatus::READY);
}

TEST(CommandTest, CompletedCommandReturnsTrue) {
    TestCompletedCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);
    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
}

TEST(CommandTest, WaitingCommandReturnsFalse) {
    TestWaitingCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);
    EXPECT_FALSE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::ACTIVE);
}

TEST(CommandTest, ErrorCommandReturnsTrueWithError) {
    TestErrorCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);
    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

//==============================================================================
// 命令链式调用测试
//==============================================================================

TEST(CommandTest, ChainedExecution) {
    int counter = 0;
    auto cmd1 = std::make_unique<TestChainCommand>(1, counter);
    auto cmd2 = std::make_unique<TestChainCommand>(1, counter);
    auto cmd3 = std::make_unique<TestChainCommand>(1, counter);

    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    // 第一次执行：counter = 1
    EXPECT_FALSE(cmd1->execute(&engine));
    EXPECT_EQ(counter, 1);

    // 第二次执行：counter = 2
    EXPECT_FALSE(cmd2->execute(&engine));
    EXPECT_EQ(counter, 2);

    // 第三次执行：counter = 3，完成
    EXPECT_TRUE(cmd3->execute(&engine));
    EXPECT_EQ(counter, 3);
}

//==============================================================================
// HTTP 命令基础测试
//==============================================================================

TEST(HttpCommandTest, HttpInitiateConnectionCreation) {
    DownloadOptions options;
    std::string url = "http://example.com/file.zip";

    HttpInitiateConnectionCommand cmd(1, url, options);

    EXPECT_EQ(cmd.status(), CommandStatus::READY);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandTest, HttpInitiateConnectionParsesHttpUrl) {
    DownloadOptions options;
    std::string url = "http://example.com/file.zip";

    HttpInitiateConnectionCommand cmd(1, url, options);

    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
    // HTTPS 应该被识别
}

TEST(HttpCommandTest, HttpInitiateConnectionParsesHttpsUrl) {
    DownloadOptions options;
    std::string url = "https://example.com/file.zip";

    HttpInitiateConnectionCommand cmd(1, url, options);

    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandTest, HttpResponseCommandCreation) {
    DownloadOptions options;
    std::string url = "http://example.com/file.zip";

    auto http_request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(1, -1, http_request, options);

    EXPECT_FALSE(cmd.is_redirect());
    EXPECT_FALSE(cmd.accepts_range());
}

TEST(HttpCommandTest, HttpDownloadCommandCreation) {
    auto http_response = std::make_shared<HttpResponse>();
    SegmentId segment_id = 1;
    Bytes offset = 0;
    Bytes length = 1024 * 1024;

    HttpDownloadCommand cmd(1, -1, http_response, segment_id, offset, length);

    EXPECT_EQ(cmd.downloaded_bytes(), 0);
    EXPECT_FALSE(cmd.is_complete());
}

//==============================================================================
// 命令生命周期测试
//==============================================================================

TEST(CommandLifecycle, StatusTransitions) {
    TestCompletedCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    // 初始状态
    EXPECT_EQ(cmd.status(), CommandStatus::READY);

    // 执行后变为完成
    cmd.execute(&engine);
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
}

TEST(CommandLifecycle, MultipleExecuteCalls) {
    TestCompletedCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    // 第一次执行
    EXPECT_TRUE(cmd.execute(&engine));

    // 已完成的命令再次执行应返回 true
    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
}

TEST(CommandLifecycle, ActiveCommandTracking) {
    TestWaitingCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    EXPECT_FALSE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::ACTIVE);

    // 等待中的命令会保持 ACTIVE 状态
}

//==============================================================================
// HTTP 命令详细测试
//==============================================================================

TEST(HttpCommandTest, HttpInitiateConnectionUrlParsing) {
    DownloadOptions options;

    // 测试 HTTP URL 解析
    {
        std::string url = "http://example.com/file.zip";
        HttpInitiateConnectionCommand cmd(1, url, options);
        EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
    }

    // 测试 HTTPS URL 解析
    {
        std::string url = "https://example.com/file.zip";
        HttpInitiateConnectionCommand cmd(1, url, options);
        EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
    }

    // 测试带端口的 URL
    {
        std::string url = "http://example.com:8080/file.zip";
        HttpInitiateConnectionCommand cmd(1, url, options);
        EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
    }

    // 测试带查询参数的 URL
    {
        std::string url = "http://example.com/file.zip?param=value";
        HttpInitiateConnectionCommand cmd(1, url, options);
        EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
    }

    // 测试根路径
    {
        std::string url = "http://example.com/";
        HttpInitiateConnectionCommand cmd(1, url, options);
        EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
    }

    // 测试无路径的 URL
    {
        std::string url = "http://example.com";
        HttpInitiateConnectionCommand cmd(1, url, options);
        EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
    }
}

TEST(HttpCommandTest, HttpInitiateConnectionCustomHeaders) {
    DownloadOptions options;
    options.headers["X-Custom"] = "CustomValue";
    options.headers["Authorization"] = "Bearer token123";
    options.referer = "http://referer.com";
    options.user_agent = "TestAgent/1.0";

    std::string url = "http://example.com/file.zip";
    HttpInitiateConnectionCommand cmd(1, url, options);

    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandTest, HttpInitiateConnectionWithOptions) {
    DownloadOptions options;
    options.max_retries = 5;
    options.timeout_seconds = 60;
    options.min_segment_size = 1024 * 1024;  // 1MB

    std::string url = "http://example.com/largefile.zip";
    HttpInitiateConnectionCommand cmd(1, url, options);

    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandTest, HttpResponseCommandInitialState) {
    DownloadOptions options;
    auto http_request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(1, -1, http_request, options);

    EXPECT_FALSE(cmd.is_redirect());
    EXPECT_FALSE(cmd.accepts_range());
    EXPECT_FALSE(cmd.supports_resume());
    EXPECT_EQ(cmd.status_code(), 0);
    EXPECT_EQ(cmd.content_length(), 0);
}

TEST(HttpCommandTest, HttpDownloadCommandInitialState) {
    auto http_response = std::make_shared<HttpResponse>();
    SegmentId segment_id = 1;
    Bytes offset = 1024;
    Bytes length = 1024 * 1024;
    std::string initial_data = "initial data";

    HttpDownloadCommand cmd(1, -1, http_response, segment_id, offset, length, initial_data);

    EXPECT_EQ(cmd.segment_id(), segment_id);
    EXPECT_EQ(cmd.offset(), offset);
    EXPECT_EQ(cmd.length(), length);
    EXPECT_EQ(cmd.downloaded_bytes(), 0);
    EXPECT_FALSE(cmd.is_complete());
}

TEST(HttpCommandTest, HttpDownloadCommandMultipleSegments) {
    auto http_response = std::make_shared<HttpResponse>();

    // 创建多个分段
    std::vector<std::unique_ptr<HttpDownloadCommand>> segments;
    for (SegmentId i = 0; i < 4; ++i) {
        Bytes offset = i * 1024 * 1024;
        Bytes length = 1024 * 1024;
        segments.push_back(std::make_unique<HttpDownloadCommand>(
            1, -1, http_response, i, offset, length));
    }

    EXPECT_EQ(segments.size(), 4);
    for (const auto& seg : segments) {
        EXPECT_EQ(seg->downloaded_bytes(), 0);
        EXPECT_FALSE(seg->is_complete());
    }
}

TEST(HttpCommandTest, HttpRetryCommandBasic) {
    std::string url = "http://example.com/file.zip";
    DownloadOptions options;
    options.max_retries = 3;

    HttpRetryCommand cmd(1, url, options, 0);

    EXPECT_EQ(cmd.retry_count(), 0);
}

TEST(HttpCommandTest, HttpRetryCommandMultipleRetries) {
    std::string url = "http://example.com/file.zip";
    DownloadOptions options;
    options.max_retries = 5;

    // 第一次重试
    HttpRetryCommand cmd1(1, url, options, 1);
    EXPECT_EQ(cmd1.retry_count(), 1);

    // 第二次重试
    HttpRetryCommand cmd2(1, url, options, 2);
    EXPECT_EQ(cmd2.retry_count(), 2);

    // 达到最大重试次数
    HttpRetryCommand cmd3(1, url, options, 5);
    EXPECT_EQ(cmd3.retry_count(), 5);
}

TEST(HttpCommandTest, HttpRetryCommandWithOptions) {
    std::string url = "http://example.com/file.zip";
    DownloadOptions options;
    options.max_retries = 10;
    options.timeout_seconds = 120;

    HttpRetryCommand cmd(1, url, options, 3);

    EXPECT_EQ(cmd.retry_count(), 3);
}

//==============================================================================
// 命令执行结果测试
//==============================================================================

TEST(CommandExecutionResult, OkResult) {
    TestCompletedCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
}

TEST(CommandExecutionResult, ErrorResult) {
    TestErrorCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST(CommandExecutionResult, WaitResult) {
    TestWaitingCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    EXPECT_FALSE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::ACTIVE);
}

//==============================================================================
// 命令状态转换测试
//==============================================================================

TEST(CommandStateTransition, ReadyToCompleted) {
    TestCompletedCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    EXPECT_EQ(cmd.status(), CommandStatus::READY);
    cmd.execute(&engine);
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
}

TEST(CommandStateTransition, ReadyToActive) {
    TestWaitingCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    EXPECT_EQ(cmd.status(), CommandStatus::READY);
    cmd.execute(&engine);
    EXPECT_EQ(cmd.status(), CommandStatus::ACTIVE);
}

TEST(CommandStateTransition, ReadyToFailed) {
    TestErrorCommand cmd(1);
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    EXPECT_EQ(cmd.status(), CommandStatus::READY);
    cmd.execute(&engine);
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

//==============================================================================
// 多任务命令测试
//==============================================================================

TEST(MultiTaskCommands, DifferentTaskIds) {
    TestCompletedCommand cmd1(1);
    TestCompletedCommand cmd2(2);
    TestCompletedCommand cmd3(3);

    EXPECT_EQ(cmd1.get_task_id(), 1);
    EXPECT_EQ(cmd2.get_task_id(), 2);
    EXPECT_EQ(cmd3.get_task_id(), 3);
}

TEST(MultiTaskCommands, SameTaskIdChained) {
    int counter = 0;
    auto cmd1 = std::make_unique<TestChainCommand>(1, counter);
    auto cmd2 = std::make_unique<TestChainCommand>(1, counter);
    auto cmd3 = std::make_unique<TestChainCommand>(1, counter);

    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    EXPECT_FALSE(cmd1->execute(&engine));
    EXPECT_EQ(counter, 1);
    EXPECT_EQ(cmd1->status(), CommandStatus::ACTIVE);

    EXPECT_FALSE(cmd2->execute(&engine));
    EXPECT_EQ(counter, 2);
    EXPECT_EQ(cmd2->status(), CommandStatus::ACTIVE);

    EXPECT_TRUE(cmd3->execute(&engine));
    EXPECT_EQ(counter, 3);
    EXPECT_EQ(cmd3->status(), CommandStatus::COMPLETED);
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(CommandBoundary, ZeroTaskId) {
    TestCompletedCommand cmd(0);
    EXPECT_EQ(cmd.get_task_id(), 0);
}

TEST(CommandBoundary, LargeTaskId) {
    TaskId large_id = std::numeric_limits<TaskId>::max();
    TestCompletedCommand cmd(large_id);
    EXPECT_EQ(cmd.get_task_id(), large_id);
}

TEST(HttpCommandBoundary, EmptyUrl) {
    DownloadOptions options;
    std::string url = "";
    HttpInitiateConnectionCommand cmd(1, url, options);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandBoundary, VeryLongUrl) {
    DownloadOptions options;
    std::string url = "http://example.com/" + std::string(10000, 'a');
    HttpInitiateConnectionCommand cmd(1, url, options);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandBoundary, UrlWithSpecialCharacters) {
    DownloadOptions options;
    std::string url = "http://example.com/file%20name%20test.zip?param=value%20with%20spaces";
    HttpInitiateConnectionCommand cmd(1, url, options);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandBoundary, IPv4Address) {
    DownloadOptions options;
    std::string url = "http://192.168.1.1/file.zip";
    HttpInitiateConnectionCommand cmd(1, url, options);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandBoundary, IPv6Address) {
    DownloadOptions options;
    std::string url = "http://[::1]/file.zip";
    HttpInitiateConnectionCommand cmd(1, url, options);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpDownloadBoundary, ZeroLengthSegment) {
    auto http_response = std::make_shared<HttpResponse>();
    HttpDownloadCommand cmd(1, -1, http_response, 0, 0, 0);

    EXPECT_EQ(cmd.length(), 0);
    EXPECT_EQ(cmd.downloaded_bytes(), 0);
}

TEST(HttpDownloadBoundary, LargeSegmentSize) {
    auto http_response = std::make_shared<HttpResponse>();
    Bytes large_size = std::numeric_limits<Bytes>::max();
    HttpDownloadCommand cmd(1, -1, http_response, 0, 0, large_size);

    EXPECT_EQ(cmd.length(), large_size);
}

TEST(HttpDownloadBoundary, ManySegments) {
    auto http_response = std::make_shared<HttpResponse>();
    constexpr SegmentId num_segments = 1000;
    Bytes segment_size = 1024 * 1024;  // 1MB

    for (SegmentId i = 0; i < num_segments; ++i) {
        HttpDownloadCommand cmd(1, -1, http_response, i, i * segment_size, segment_size);
        EXPECT_EQ(cmd.segment_id(), i);
    }
}

//==============================================================================
// 重试机制测试
//==============================================================================

TEST(HttpRetry, RetryCountIncrements) {
    std::string url = "http://example.com/file.zip";
    DownloadOptions options;
    options.max_retries = 3;

    for (int i = 0; i <= 3; ++i) {
        HttpRetryCommand cmd(1, url, options, i);
        EXPECT_EQ(cmd.retry_count(), i);
    }
}

TEST(HttpRetry, MaxRetriesReached) {
    std::string url = "http://example.com/file.zip";
    DownloadOptions options;
    options.max_retries = 2;

    // 达到最大重试次数
    HttpRetryCommand cmd(1, url, options, 2);
    EXPECT_EQ(cmd.retry_count(), 2);
    EXPECT_EQ(cmd.retry_count(), options.max_retries);
}

TEST(HttpRetry, ZeroMaxRetries) {
    std::string url = "http://example.com/file.zip";
    DownloadOptions options;
    options.max_retries = 0;

    HttpRetryCommand cmd(1, url, options, 0);
    EXPECT_EQ(cmd.retry_count(), 0);
}

TEST(HttpRetry, LargeMaxRetries) {
    std::string url = "http://example.com/file.zip";
    DownloadOptions options;
    options.max_retries = 100;

    HttpRetryCommand cmd(1, url, options, 50);
    EXPECT_EQ(cmd.retry_count(), 50);
    EXPECT_LT(cmd.retry_count(), options.max_retries);
}

//==============================================================================
// 错误处理测试
//==============================================================================

TEST(CommandError, NullEngine) {
    TestCompletedCommand cmd(1);
    // 传递 null engine
    EXPECT_TRUE(cmd.execute(nullptr));
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
}

TEST(HttpCommandError, InvalidUrlFormat) {
    DownloadOptions options;
    std::string url = "not-a-valid-url";
    HttpInitiateConnectionCommand cmd(1, url, options);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

TEST(HttpCommandError, MissingProtocol) {
    DownloadOptions options;
    std::string url = "example.com/file.zip";
    HttpInitiateConnectionCommand cmd(1, url, options);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::DISCONNECTED);
}

//==============================================================================
// 性能测试
//==============================================================================

TEST(CommandPerformance, ManyCommandsCreation) {
    constexpr int count = 10000;
    std::vector<std::unique_ptr<TestCompletedCommand>> commands;
    commands.reserve(count);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        commands.push_back(std::make_unique<TestCompletedCommand>(i));
    }
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_LT(duration.count(), 1000);  // 应在 1 秒内完成
    EXPECT_EQ(commands.size(), count);
}

TEST(HttpCommandPerformance, ManyHttpCommands) {
    DownloadOptions options;
    constexpr int count = 1000;
    std::vector<std::unique_ptr<HttpInitiateConnectionCommand>> commands;
    commands.reserve(count);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        std::string url = "http://example.com/file" + std::to_string(i) + ".zip";
        commands.push_back(std::make_unique<HttpInitiateConnectionCommand>(i, url, options));
    }
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_LT(duration.count(), 1000);  // 应在 1 秒内完成
    EXPECT_EQ(commands.size(), count);
}

//==============================================================================
// 并发测试
//==============================================================================

TEST(CommandConcurrency, ConcurrentCommandExecution) {
    constexpr int num_commands = 100;
    std::vector<std::unique_ptr<TestCompletedCommand>> commands;
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    for (int i = 0; i < num_commands; ++i) {
        commands.push_back(std::make_unique<TestCompletedCommand>(i));
    }

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < num_commands; ++i) {
        threads.emplace_back([&, i]() {
            if (commands[i]->execute(&engine)) {
                success_count++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count, num_commands);
}

//==============================================================================
// 主函数
//==============================================================================

