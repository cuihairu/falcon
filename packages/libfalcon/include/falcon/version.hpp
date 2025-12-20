#pragma once

namespace falcon {

/// 版本信息
constexpr int VERSION_MAJOR = 0;
constexpr int VERSION_MINOR = 1;
constexpr int VERSION_PATCH = 0;

/// 版本字符串
constexpr const char* VERSION_STRING = "0.1.0";

/// 获取完整版本字符串（包含编译信息）
const char* get_version_string();

/// 获取构建时间
const char* get_build_timestamp();

} // namespace falcon
