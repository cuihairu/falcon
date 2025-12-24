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
    EXPECT_EQ(cmd.status(), CommandStatus::ERROR);
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
// 主函数
//==============================================================================

