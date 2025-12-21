#pragma once

#include <iostream>
#include <string>

// Simple logger implementation for now
// TODO: Replace with proper logging library (spdlog)

#define FALCON_LOG_INFO(msg) std::cout << "[INFO] " << msg << std::endl
#define FALCON_LOG_DEBUG(msg) std::cout << "[DEBUG] " << msg << std::endl
#define FALCON_LOG_WARN(msg) std::cerr << "[WARN] " << msg << std::endl
#define FALCON_LOG_ERROR(msg) std::cerr << "[ERROR] " << msg << std::endl

namespace falcon {

// Logging functions
inline void log_info(const std::string& msg) {
    std::cout << "[INFO] " << msg << std::endl;
}

inline void log_debug(const std::string& msg) {
    std::cout << "[DEBUG] " << msg << std::endl;
}

inline void log_warn(const std::string& msg) {
    std::cerr << "[WARN] " << msg << std::endl;
}

inline void log_error(const std::string& msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
}

} // namespace falcon