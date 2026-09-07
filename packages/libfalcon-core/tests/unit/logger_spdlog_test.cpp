/**
 * @file logger_spdlog_test.cpp
 * @brief spdlog 日志后端测试（仅在 FALCON_USE_SPDLOG 构建下编译；
 *        无 spdlog 的最小构建此 TU 仅含占位测试）
 * @author Falcon Team
 * @date 2026-09-07
 */

#include <gtest/gtest.h>

#include <falcon/logger.hpp>

#ifdef FALCON_USE_SPDLOG

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

/// 临时向全局 logger 附加 callback sink 捕获消息，TearDown 恢复原始 sink 集合
class LoggerSpdlogTest : public ::testing::Test {
protected:
    void SetUp() override {
        captured().clear();

        logger_ = ::falcon::falcon_logger();
        original_sinks_ = logger_->sinks();
        original_level_ = ::falcon::get_log_level();

        auto callback_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [](const spdlog::details::log_msg& msg) {
                captured().emplace_back(msg.payload.begin(), msg.payload.end());
            });
        callback_sink->set_level(spdlog::level::trace);
        logger_->sinks().push_back(callback_sink);
    }

    void TearDown() override {
        logger_->sinks() = original_sinks_;
        ::falcon::set_log_level(original_level_);
        logger_->set_level(::falcon::to_spdlog_level(original_level_));
    }

    /// sink 中是否包含指定子串
    static bool sink_contains(const std::string& needle) {
        const auto& lines = captured();
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }

    static std::vector<std::string>& captured() {
        static std::vector<std::string> lines;
        return lines;
    }

    std::shared_ptr<spdlog::logger> logger_;
    std::vector<spdlog::sink_ptr> original_sinks_;
    ::falcon::LogLevel original_level_ = ::falcon::LogLevel::Info;
};

TEST_F(LoggerSpdlogTest, MacroOutputRoutesThroughSpdlog) {
    ::falcon::set_log_level(::falcon::LogLevel::Trace);

    FALCON_LOG_INFO("fmt style value={}", 42);
    FALCON_LOG_INFO_STREAM("stream style " << 7 << " bytes");
    FALCON_LOG_WARN("warning text");
    FALCON_LOG_ERROR("error text");
    ::falcon::log_debug("plain debug");
    ::falcon::log_info("plain info");

    EXPECT_TRUE(sink_contains("fmt style value=42"));
    EXPECT_TRUE(sink_contains("stream style 7 bytes"));
    EXPECT_TRUE(sink_contains("warning text"));
    EXPECT_TRUE(sink_contains("error text"));
    EXPECT_TRUE(sink_contains("plain debug"));
    EXPECT_TRUE(sink_contains("plain info"));
}

TEST_F(LoggerSpdlogTest, LevelFilteringAppliesToAllMacros) {
    ::falcon::set_log_level(::falcon::LogLevel::Warn);

    FALCON_LOG_INFO("should be filtered");
    FALCON_LOG_INFO_STREAM("stream filtered " << 1);
    ::falcon::log_debug("debug filtered");
    FALCON_LOG_WARN("warn passes");
    FALCON_LOG_ERROR("error passes");

    EXPECT_FALSE(sink_contains("should be filtered"));
    EXPECT_FALSE(sink_contains("stream filtered"));
    EXPECT_FALSE(sink_contains("debug filtered"));
    EXPECT_TRUE(sink_contains("warn passes"));
    EXPECT_TRUE(sink_contains("error passes"));
}

TEST_F(LoggerSpdlogTest, SetLogLevelSyncsSpdlogLogger) {
    ::falcon::set_log_level(::falcon::LogLevel::Error);
    EXPECT_EQ(logger_->level(), spdlog::level::err);
    EXPECT_EQ(::falcon::get_log_level(), ::falcon::LogLevel::Error);

    ::falcon::set_log_level(static_cast<int>(::falcon::LogLevel::Debug));
    EXPECT_EQ(logger_->level(), spdlog::level::debug);

    // 越界 int：spdlog 端 clamp 到 off/trace，全局存储保留原值
    ::falcon::set_log_level(-5);
    EXPECT_EQ(logger_->level(), spdlog::level::off);
    ::falcon::set_log_level(99);
    EXPECT_EQ(logger_->level(), spdlog::level::trace);
}

TEST_F(LoggerSpdlogTest, BracesInMessageAreDataNotFormat) {
    ::falcon::set_log_level(::falcon::LogLevel::Info);

    // 消息文本包含 '{}'：以数据形式进入 spdlog，不应触发 fmt 错误
    const std::string weird = "literal {} braces {0} {bad";
    ::falcon::log_info(weird);

    EXPECT_TRUE(sink_contains(weird));
}

TEST_F(LoggerSpdlogTest, FalconLoggerIsNamedFalcon) {
    EXPECT_EQ(logger_->name(), "falcon");
    EXPECT_FALSE(::falcon::falcon_logger()->sinks().empty());
}

} // namespace

#else

// 未启用 spdlog 的构建：占位测试保持 TU 非空
TEST(LoggerSpdlogBackend, BuiltInFallbackActive) {
    SUCCEED() << "FALCON_USE_SPDLOG not defined: built-in logger backend in use";
}

#endif  // FALCON_USE_SPDLOG
