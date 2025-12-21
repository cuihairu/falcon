/**
 * @file version.cpp
 * @brief 版本信息实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/version.hpp>
#include <sstream>
#include <ctime>

namespace falcon {

std::string Version::to_string() const {
    std::ostringstream oss;
    oss << major << "." << minor << "." << patch;
    return oss.str();
}

std::string Version::to_full_string() const {
    std::ostringstream oss;
    oss << "Falcon " << to_string();

#ifdef FALCON_VERSION_SUFFIX
    oss << "-" << FALCON_VERSION_SUFFIX;
#endif

#ifdef __DATE__
    oss << " (built " << __DATE__;
#ifdef __TIME__
    oss << " " << __TIME__;
#endif
    oss << ")";
#endif

    return oss.str();
}

const Version FALCON_VERSION{0, 1, 0};

// Legacy functions for backward compatibility
constexpr int VERSION_MAJOR = 0;
constexpr int VERSION_MINOR = 1;
constexpr int VERSION_PATCH = 0;
constexpr const char* VERSION_STRING = "0.1.0";

const char* get_version_string() {
    static std::string version = FALCON_VERSION.to_full_string();
    return version.c_str();
}

const char* get_build_timestamp() {
    return __DATE__ " " __TIME__;
}

} // namespace falcon