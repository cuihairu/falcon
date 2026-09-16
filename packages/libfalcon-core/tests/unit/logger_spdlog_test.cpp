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

// to_spdlog_level 全枚举往返 + 越界枚举的兜底返回（switch 穿透防御）
TEST_F(LoggerSpdlogTest, ToSpdlogLevelFullEnumAndInvalidFallback) {
    EXPECT_EQ(::falcon::to_spdlog_level(::falcon::LogLevel::Off), spdlog::level::off);
    EXPECT_EQ(::falcon::to_spdlog_level(::falcon::LogLevel::Error), spdlog::level::err);
    EXPECT_EQ(::falcon::to_spdlog_level(::falcon::LogLevel::Warn), spdlog::level::warn);
    EXPECT_EQ(::falcon::to_spdlog_level(::falcon::LogLevel::Info), spdlog::level::info);
    EXPECT_EQ(::falcon::to_spdlog_level(::falcon::LogLevel::Debug), spdlog::level::debug);
    EXPECT_EQ(::falcon::to_spdlog_level(::falcon::LogLevel::Trace), spdlog::level::trace);
    // 非法枚举值（内存损坏/反序列化脏数据防御）：default 返回 info
    EXPECT_EQ(::falcon::to_spdlog_level(static_cast<::falcon::LogLevel>(99)),
              spdlog::level::info);
}

// FalconConsoleSink 直接驱动：六个级别 tag + off 兜底 tag 都走真实
// sink_it_ 输出路径；base_sink::flush() 触发 flush_()（双通道 fflush）
TEST_F(LoggerSpdlogTest, ConsoleSinkEmitsAllLevelTagsAndFlushes) {
    auto sink = std::make_shared<::falcon::FalconConsoleSink>();
    sink->set_level(spdlog::level::trace);
    spdlog::logger direct("console-direct", sink);
    direct.set_level(spdlog::level::trace);

    // 覆盖 level_tag 的 trace/critical/off 三个冷门分支
    // （trace/critical 在 falcon 生产代码中从不使用；off 经显式 log 注入）
    direct.log(spdlog::level::trace, "tag-trace");
    direct.log(spdlog::level::debug, "tag-debug");
    direct.log(spdlog::level::info, "tag-info");
    direct.log(spdlog::level::warn, "tag-warn");
    direct.log(spdlog::level::err, "tag-error");
    direct.log(spdlog::level::critical, "tag-critical");
    direct.log(spdlog::level::off, "tag-off");

    // 空载荷消息：sink_it_ 的 payload.size() == 0 分支（只输出 tag + 换行）
    direct.log(spdlog::level::info, "");

    // base_sink::flush() 公开入口 → flush_() override
    sink->flush();
    SUCCEED();
}

// log_* 函数级门禁（不经过 FALCON_LOG_* 宏的快速判断）：
// 全局级别抬到 Off 后 info/debug/warn/error 四函数在入口提前 return
TEST_F(LoggerSpdlogTest, LogFunctionsGateBeforeReachingSink) {
    ::falcon::set_log_level(::falcon::LogLevel::Off);
    ::falcon::log_info("gated info");
    ::falcon::log_debug("gated debug");
    ::falcon::log_warn("gated warn");
    ::falcon::log_error("gated error");
    EXPECT_FALSE(sink_contains("gated"));

    // 恢复 Trace 后 FMT 门函数族四链全走（log_infof/debugf/warnf/errorf
    // → format_log_message → log_* → spdlog）
    ::falcon::set_log_level(::falcon::LogLevel::Trace);
    ::falcon::detail::log_infof("infof {} {}", "a", 1);
    ::falcon::detail::log_debugf("debugf {}", std::string("b"));
    ::falcon::detail::log_warnf("warnf {}", 2.5);
    ::falcon::detail::log_errorf("errorf {}", "c");
    EXPECT_TRUE(sink_contains("infof a 1"));
    EXPECT_TRUE(sink_contains("debugf b"));
    EXPECT_TRUE(sink_contains("warnf 2.5"));
    EXPECT_TRUE(sink_contains("errorf c"));
}

// format_log_message 分支矩阵（含 to_log_string 各重载）：
// 零参数/常规替换/占位符过剩/参数过剩/未闭合 '{'/char* 与 null 指针
TEST(LoggerFormatMessage, CoversAllBranchShapes) {
    namespace d = ::falcon::detail;
    using std::string;

    // 零参数：format 原样返回（if constexpr 空参数包分支）
    EXPECT_EQ(d::format_log_message("plain"), "plain");

    // 常规替换：const char* / std::string / int（三种 to_log_string 路径）
    EXPECT_EQ(d::format_log_message("a={} b={} c={}", "x", string("y"), 42),
              "a=x b=y c=42");

    // 占位符多于参数：第二个 {} 无参可用，原样保留
    EXPECT_EQ(d::format_log_message("{} {}", "only"), "only {}");

    // 参数多于占位符：尾部追加（result 非空时补空格分隔）
    EXPECT_EQ(d::format_log_message("head", 1, 2), "head 1 2");
    // 空 format：首个多余参数不加前导空格
    EXPECT_EQ(d::format_log_message("", 7), "7");
    // result 尾字符恰为空格：不重复补
    EXPECT_EQ(d::format_log_message("x ", 7), "x 7");

    // 未闭合 '{'（无 '}'）：按普通字符逐个输出，参数仍走尾部追加
    EXPECT_EQ(d::format_log_message("a { b", "v"), "a { b v");

    // to_log_string 显式变体：char*（非 const）/ 两个 null 指针形态 /
    // 数值模板实例 / string 特化
    char buf[4] = {'c', 'h', 'p', '\0'};
    char* ptr = buf;
    EXPECT_EQ(d::to_log_string(ptr), "chp");
    char* null_ptr = nullptr;
    EXPECT_EQ(d::to_log_string(null_ptr), "(null)");
    const char* null_const = nullptr;
    EXPECT_EQ(d::to_log_string(null_const), "(null)");
    EXPECT_EQ(d::to_log_string(12345), "12345");
    EXPECT_EQ(d::to_log_string(string("str")), "str");
    EXPECT_EQ(d::format_log_message("p={}", ptr), "p=chp");
}

} // namespace

#else

// 未启用 spdlog 的构建：占位测试保持 TU 非空
TEST(LoggerSpdlogBackend, BuiltInFallbackActive) {
    SUCCEED() << "FALCON_USE_SPDLOG not defined: built-in logger backend in use";
}

#endif  // FALCON_USE_SPDLOG
