#pragma once

#include <string>

namespace falcon {

/// 版本信息
constexpr int VERSION_MAJOR = 0;
constexpr int VERSION_MINOR = 1;
constexpr int VERSION_PATCH = 0;

/// 版本字符串
constexpr const char* VERSION_STRING = "0.1.0";

/// 结构化版本信息
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    std::string to_string() const;
    std::string to_full_string() const;
};

extern const Version FALCON_VERSION;

/// 获取完整版本字符串（包含编译信息）
const char* get_version_string();

/// 获取构建时间
const char* get_build_timestamp();

} // namespace falcon
