#pragma once

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

// Simple logger implementation for now
// TODO: Replace with proper logging library (spdlog)

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

#define FALCON_LOG_INFO_STREAM(msg)                                                  \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Info)) {                            \
            std::cout << "[INFO] " << msg << std::endl;                              \
        }                                                                            \
    } while (0)

#define FALCON_LOG_INFO_FMT(...) ::falcon::detail::log_infof(__VA_ARGS__)

#define FALCON_LOG_DEBUG_STREAM(msg)                                                 \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Debug)) {                           \
            std::cout << "[DEBUG] " << msg << std::endl;                             \
        }                                                                            \
    } while (0)

#define FALCON_LOG_DEBUG_FMT(...) ::falcon::detail::log_debugf(__VA_ARGS__)

#define FALCON_LOG_WARN_STREAM(msg)                                                  \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Warn)) {                            \
            std::cerr << "[WARN] " << msg << std::endl;                              \
        }                                                                            \
    } while (0)

#define FALCON_LOG_WARN_FMT(...) ::falcon::detail::log_warnf(__VA_ARGS__)

#define FALCON_LOG_ERROR_STREAM(msg)                                                 \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Error)) {                           \
            std::cerr << "[ERROR] " << msg << std::endl;                             \
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

#define FALCON_DETAIL_DISPATCH_LOG(level, count, ...) FALCON_DETAIL_DISPATCH_LOG_(level, count, __VA_ARGS__)
#define FALCON_DETAIL_DISPATCH_LOG_(level, count, ...) FALCON_DETAIL_LOG_##level##_##count(__VA_ARGS__)

#define FALCON_LOG_INFO(...)                                                         \
    FALCON_DETAIL_DISPATCH_LOG(INFO, FALCON_DETAIL_NARG(__VA_ARGS__), __VA_ARGS__)
#define FALCON_LOG_DEBUG(...)                                                        \
    FALCON_DETAIL_DISPATCH_LOG(DEBUG, FALCON_DETAIL_NARG(__VA_ARGS__), __VA_ARGS__)
#define FALCON_LOG_WARN(...)                                                         \
    FALCON_DETAIL_DISPATCH_LOG(WARN, FALCON_DETAIL_NARG(__VA_ARGS__), __VA_ARGS__)
#define FALCON_LOG_ERROR(...)                                                        \
    FALCON_DETAIL_DISPATCH_LOG(ERROR, FALCON_DETAIL_NARG(__VA_ARGS__), __VA_ARGS__)
