#pragma once

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#ifdef FALCON_USE_SPDLOG
#include <cstdio>
#include <cstring>
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

/// falcon LogLevel → spdlog 级别映射
inline spdlog::level::level_enum to_spdlog_level(LogLevel level) {
    switch (level) {
        case LogLevel::Off:   return spdlog::level::off;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Warn:  return spdlog::level::warn;
        case LogLevel::Info:  return spdlog::level::info;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Trace: return spdlog::level::trace;
    }
    return spdlog::level::info;
}

class FalconConsoleSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        FILE* out = msg.level >= spdlog::level::warn ? stderr : stdout;
        const char* tag = level_tag(msg.level);
        std::fwrite(tag, 1, std::strlen(tag), out);
        if (msg.payload.size() > 0) {
            std::fwrite(msg.payload.data(), 1, msg.payload.size(), out);
        }
        std::fputc('\n', out);
        std::fflush(out);
    }

    void flush_() override {
        std::fflush(stdout);
        std::fflush(stderr);
    }

private:
    static const char* level_tag(spdlog::level::level_enum level) {
        switch (level) {
            case spdlog::level::trace:    return "[TRACE] ";
            case spdlog::level::debug:    return "[DEBUG] ";
            case spdlog::level::info:     return "[INFO] ";
            case spdlog::level::warn:     return "[WARN] ";
            case spdlog::level::err:      return "[ERROR] ";
            case spdlog::level::critical: return "[CRITICAL] ";
            default:                      return "[OFF] ";
        }
    }
};

/// 全局共享 logger（FALCON_LOG_* 的后端；惰性创建，级别取自全局存储）
inline std::shared_ptr<spdlog::logger>& falcon_logger_storage() {
    static std::shared_ptr<spdlog::logger> instance = [] {
        auto logger = std::make_shared<spdlog::logger>(
            "falcon", std::make_shared<FalconConsoleSink>());
        logger->set_level(to_spdlog_level(get_log_level()));
        return logger;
    }();
    return instance;
}

/**
 * @brief 获取全局 spdlog logger
 *
 * 供高级用法：附加自定义 sink、flush、注册错误处理等。
 */
inline const std::shared_ptr<spdlog::logger>& falcon_logger() {
    return falcon_logger_storage();
}

inline void set_log_level(LogLevel level) {
    global_log_level_storage().store(static_cast<int>(level), std::memory_order_relaxed);
    if (auto logger = falcon_logger_storage()) {
        logger->set_level(to_spdlog_level(level));
    }
}

inline void set_log_level(int level) {
    global_log_level_storage().store(level, std::memory_order_relaxed);
    if (auto logger = falcon_logger_storage()) {
        if (level < 0) {
            logger->set_level(spdlog::level::off);
        } else if (level > static_cast<int>(LogLevel::Trace)) {
            logger->set_level(spdlog::level::trace);
        } else {
            logger->set_level(to_spdlog_level(static_cast<LogLevel>(level)));
        }
    }
}

// 日志输出函数（经 spdlog 分流 sink；消息作为纯文本数据传入，
// 内含 '{' '}' 等字符不会被当作格式占位符）
inline void log_info(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Info)) {
        return;
    }
    if (auto logger = falcon_logger_storage()) {
        logger->log(spdlog::level::info, "{}", msg);
    }
}

inline void log_debug(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Debug)) {
        return;
    }
    if (auto logger = falcon_logger_storage()) {
        logger->log(spdlog::level::debug, "{}", msg);
    }
}

inline void log_warn(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Warn)) {
        return;
    }
    if (auto logger = falcon_logger_storage()) {
        logger->log(spdlog::level::warn, "{}", msg);
    }
}

inline void log_error(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Error)) {
        return;
    }
    if (auto logger = falcon_logger_storage()) {
        logger->log(spdlog::level::err, "{}", msg);
    }
}

#else
//==============================================================================
// 内置回退后端（无 spdlog 的最小构建使用；行为与 spdlog 后端一致）
//==============================================================================

inline void set_log_level(LogLevel level) {
    global_log_level_storage().store(static_cast<int>(level), std::memory_order_relaxed);
}

inline void set_log_level(int level) {
    global_log_level_storage().store(level, std::memory_order_relaxed);
}

// Logging functions
inline void log_info(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Info)) {
        return;
    }
    std::cout << "[INFO] " << msg << std::endl;
}

inline void log_debug(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Debug)) {
        return;
    }
    std::cout << "[DEBUG] " << msg << std::endl;
}

inline void log_warn(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Warn)) {
        return;
    }
    std::cerr << "[WARN] " << msg << std::endl;
}

inline void log_error(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Error)) {
        return;
    }
    std::cerr << "[ERROR] " << msg << std::endl;
}

#endif  // FALCON_USE_SPDLOG

namespace detail {

template <typename T>
std::string to_log_string(const T& value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

inline std::string to_log_string(const std::string& value) {
    return value;
}

inline std::string to_log_string(const char* value) {
    return value == nullptr ? std::string("(null)") : std::string(value);
}

inline std::string to_log_string(char* value) {
    return value == nullptr ? std::string("(null)") : std::string(value);
}

template <typename... Args>
std::string format_log_message(const std::string& format, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        return format;
    } else {
        const std::string replacements[] = {to_log_string(std::forward<Args>(args))...};
        constexpr size_t replacement_count = sizeof...(Args);

        std::string result;
        result.reserve(format.size() + replacement_count * 8);

        size_t arg_index = 0;
        size_t i = 0;
        while (i < format.size()) {
            if (format[i] == '{') {
                const size_t close = format.find('}', i + 1);
                if (close != std::string::npos) {
                    if (arg_index < replacement_count) {
                        result += replacements[arg_index++];
                    } else {
                        result.append(format, i, close - i + 1);
                    }
                    i = close + 1;
                    continue;
                }
            }

            result.push_back(format[i]);
            ++i;
        }

        for (; arg_index < replacement_count; ++arg_index) {
            if (!result.empty() && result.back() != ' ') {
                result.push_back(' ');
            }
            result += replacements[arg_index];
        }

        return result;
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
