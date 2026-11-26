#pragma once

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#ifdef FALCON_USE_SPDLOG
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#endif

namespace falcon {

enum class LogLevel : int {
    Off = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
    Trace = 5,
};

inline std::atomic<int>& global_log_level_storage() {
    static std::atomic<int> level{static_cast<int>(LogLevel::Info)};
    return level;
}

inline LogLevel get_log_level() {
    return static_cast<LogLevel>(global_log_level_storage().load(std::memory_order_relaxed));
}

#ifdef FALCON_USE_SPDLOG
//==============================================================================
// spdlog 后端（vcpkg 构建默认启用：见 libfalcon-core/CMakeLists.txt）
//
// 设计要点：
// - 公共接口（LogLevel / set_log_level / FALCON_LOG_* 宏）保持不变
// - FMT 风格宏沿用 detail::format_log_message 预格式化（ostream 渲染任意
//   类型参数），消息以纯文本 payload 进入 spdlog，避免 fmt 对非常规类型
//   的编译期格式化要求，保持全部既有调用点的语义
// - 自定义分流 sink 精确复刻旧输出格式："[LEVEL] message\n"，
//   WARN 及以上 → stderr，其余 → stdout（每条消息立即 flush，同旧 endl）
//==============================================================================

// 以下函数的实现集中在 src/logger.cpp：历史上全部 inline 于本头文件，
// 每个包含它的 TU 都生成弱符号实例而链接器只保留一份机器码，覆盖
// 计数只落在被选中的 TU，其余 TU 的实例永远显示未覆盖（测量水分）

/// falcon LogLevel → spdlog 级别映射
spdlog::level::level_enum to_spdlog_level(LogLevel level);

class FalconConsoleSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    static const char* level_tag(spdlog::level::level_enum level);
};

/// 全局共享 logger（FALCON_LOG_* 的后端；惰性创建，级别取自全局存储）
std::shared_ptr<spdlog::logger>& falcon_logger_storage();

/**
 * @brief 获取全局 spdlog logger
 *
 * 供高级用法：附加自定义 sink、flush、注册错误处理等。
 */
const std::shared_ptr<spdlog::logger>& falcon_logger();

void set_log_level(LogLevel level);

void set_log_level(int level);

// 日志输出函数（经 spdlog 分流 sink；消息作为纯文本数据传入，
// 内含 '{' '}' 等字符不会被当作格式占位符）
void log_info(const std::string& msg);

void log_debug(const std::string& msg);

void log_warn(const std::string& msg);

void log_error(const std::string& msg);

#else
//==============================================================================
// 内置回退后端（无 spdlog 的最小构建使用；行为与 spdlog 后端一致）
//==============================================================================

void set_log_level(LogLevel level);

void set_log_level(int level);

// Logging functions
void log_info(const std::string& msg);

void log_debug(const std::string& msg);

void log_warn(const std::string& msg);

void log_error(const std::string& msg);

#endif  // FALCON_USE_SPDLOG

namespace detail {

template <typename T>
std::string to_log_string(const T& value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

std::string to_log_string(const std::string& value);

std::string to_log_string(const char* value);

std::string to_log_string(char* value);

/// 格式串解析 + 占位符替换的核心循环（实现集中在 src/logger.cpp）。
/// 纯算法不依赖模板参数；若留在头文件，每个 TU 会生成一份实例，
/// 行级覆盖按实例分账产生不可消除的测量水分
std::string format_log_message_core(const std::string& format,
                                    const std::string* replacements,
                                    std::size_t replacement_count);

template <typename... Args>
std::string format_log_message(const std::string& format, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        return format;
    } else {
        const std::string replacements[] = {to_log_string(std::forward<Args>(args))...};
        return format_log_message_core(format, replacements, sizeof...(Args));
    }
}

template <typename... Args>
void log_infof(const std::string& format, Args&&... args) {
    log_info(format_log_message(format, std::forward<Args>(args)...));
}

template <typename... Args>
void log_debugf(const std::string& format, Args&&... args) {
    log_debug(format_log_message(format, std::forward<Args>(args)...));
}

template <typename... Args>
void log_warnf(const std::string& format, Args&&... args) {
    log_warn(format_log_message(format, std::forward<Args>(args)...));
}

template <typename... Args>
void log_errorf(const std::string& format, Args&&... args) {
    log_error(format_log_message(format, std::forward<Args>(args)...));
}

} // namespace detail

} // namespace falcon

// STREAM 风格宏：先做级别快速判断（避免 ostringstream 开销），再经
// log_* 输出（后端无关：spdlog 模式走 spdlog，回退模式走内置实现）
#define FALCON_LOG_INFO_STREAM(msg)                                                  \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Info)) {                            \
            std::ostringstream falcon_log_oss_;                                      \
            falcon_log_oss_ << msg;                                                  \
            ::falcon::log_info(falcon_log_oss_.str());                               \
        }                                                                            \
    } while (0)

#define FALCON_LOG_INFO_FMT(...) ::falcon::detail::log_infof(__VA_ARGS__)

#define FALCON_LOG_DEBUG_STREAM(msg)                                                 \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Debug)) {                           \
            std::ostringstream falcon_log_oss_;                                      \
            falcon_log_oss_ << msg;                                                  \
            ::falcon::log_debug(falcon_log_oss_.str());                              \
        }                                                                            \
    } while (0)

#define FALCON_LOG_DEBUG_FMT(...) ::falcon::detail::log_debugf(__VA_ARGS__)

#define FALCON_LOG_WARN_STREAM(msg)                                                  \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Warn)) {                            \
            std::ostringstream falcon_log_oss_;                                      \
            falcon_log_oss_ << msg;                                                  \
            ::falcon::log_warn(falcon_log_oss_.str());                               \
        }                                                                            \
    } while (0)

#define FALCON_LOG_WARN_FMT(...) ::falcon::detail::log_warnf(__VA_ARGS__)

#define FALCON_LOG_ERROR_STREAM(msg)                                                 \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Error)) {                           \
            std::ostringstream falcon_log_oss_;                                      \
            falcon_log_oss_ << msg;                                                  \
            ::falcon::log_error(falcon_log_oss_.str());                              \
        }                                                                            \
    } while (0)

#define FALCON_LOG_ERROR_FMT(...) ::falcon::detail::log_errorf(__VA_ARGS__)

#define FALCON_DETAIL_ARG_N(                                                          \
    _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, N, ...) N

// MSVC treats __VA_ARGS__ as a single token in macro expansion.
// An extra expansion level forces proper argument splitting.
#define FALCON_DETAIL_EXPAND(x) x
#define FALCON_DETAIL_NARG(...)                                                       \
    FALCON_DETAIL_EXPAND(FALCON_DETAIL_ARG_N(__VA_ARGS__, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))

#define FALCON_DETAIL_LOG_INFO_1(msg) FALCON_LOG_INFO_STREAM(msg)
#define FALCON_DETAIL_LOG_INFO_2(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_INFO_3(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_INFO_4(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_INFO_5(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_INFO_6(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_INFO_7(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_INFO_8(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_INFO_9(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_INFO_10(...) FALCON_LOG_INFO_FMT(__VA_ARGS__)

#define FALCON_DETAIL_LOG_DEBUG_1(msg) FALCON_LOG_DEBUG_STREAM(msg)
#define FALCON_DETAIL_LOG_DEBUG_2(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_DEBUG_3(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_DEBUG_4(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_DEBUG_5(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_DEBUG_6(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_DEBUG_7(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_DEBUG_8(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_DEBUG_9(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_DEBUG_10(...) FALCON_LOG_DEBUG_FMT(__VA_ARGS__)

#define FALCON_DETAIL_LOG_WARN_1(msg) FALCON_LOG_WARN_STREAM(msg)
#define FALCON_DETAIL_LOG_WARN_2(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_WARN_3(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_WARN_4(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_WARN_5(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_WARN_6(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_WARN_7(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_WARN_8(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_WARN_9(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_WARN_10(...) FALCON_LOG_WARN_FMT(__VA_ARGS__)

#define FALCON_DETAIL_LOG_ERROR_1(msg) FALCON_LOG_ERROR_STREAM(msg)
#define FALCON_DETAIL_LOG_ERROR_2(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_ERROR_3(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_ERROR_4(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_ERROR_5(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_ERROR_6(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_ERROR_7(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_ERROR_8(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_ERROR_9(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)
#define FALCON_DETAIL_LOG_ERROR_10(...) FALCON_LOG_ERROR_FMT(__VA_ARGS__)

// Two-level indirection so the argument count is fully expanded before
// token pasting. The level is part of the literal prefix token, never a
// standalone identifier: windows.h (wingdi.h) does `#define ERROR 0`,
// which would poison a dispatch macro taking `ERROR` as an argument and
// paste a bogus name like FALCON_DETAIL_LOG_0_2 on Windows.
#define FALCON_DETAIL_PASTE_(a, b) a##b
#define FALCON_DETAIL_PASTE(a, b) FALCON_DETAIL_PASTE_(a, b)

#define FALCON_LOG_INFO(...)                                                         \
    FALCON_DETAIL_PASTE(FALCON_DETAIL_LOG_INFO_,                                     \
                        FALCON_DETAIL_NARG(__VA_ARGS__))(__VA_ARGS__)
#define FALCON_LOG_DEBUG(...)                                                        \
    FALCON_DETAIL_PASTE(FALCON_DETAIL_LOG_DEBUG_,                                    \
                        FALCON_DETAIL_NARG(__VA_ARGS__))(__VA_ARGS__)
#define FALCON_LOG_WARN(...)                                                         \
    FALCON_DETAIL_PASTE(FALCON_DETAIL_LOG_WARN_,                                     \
                        FALCON_DETAIL_NARG(__VA_ARGS__))(__VA_ARGS__)
#define FALCON_LOG_ERROR(...)                                                        \
    FALCON_DETAIL_PASTE(FALCON_DETAIL_LOG_ERROR_,                                    \
                        FALCON_DETAIL_NARG(__VA_ARGS__))(__VA_ARGS__)
