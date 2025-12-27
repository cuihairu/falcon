#pragma once

#include <atomic>
#include <iostream>
#include <string>

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

} // namespace falcon

#define FALCON_LOG_INFO(msg)                                                         \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Info)) {                            \
            std::cout << "[INFO] " << msg << std::endl;                              \
        }                                                                            \
    } while (0)

#define FALCON_LOG_DEBUG(msg)                                                        \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Debug)) {                           \
            std::cout << "[DEBUG] " << msg << std::endl;                             \
        }                                                                            \
    } while (0)

#define FALCON_LOG_WARN(msg)                                                         \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Warn)) {                            \
            std::cerr << "[WARN] " << msg << std::endl;                              \
        }                                                                            \
    } while (0)

#define FALCON_LOG_ERROR(msg)                                                        \
    do {                                                                             \
        if (static_cast<int>(::falcon::get_log_level()) >=                           \
            static_cast<int>(::falcon::LogLevel::Error)) {                           \
            std::cerr << "[ERROR] " << msg << std::endl;                             \
        }                                                                            \
    } while (0)
