#pragma once

#include <optional>
#include <ostream>
#include <string>

// Version string macro (must be outside namespace)
#define FALCON_VERSION_STRING "0.1.0"

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

    [[nodiscard]] static std::optional<Version> parse(const std::string& version);

    bool operator==(const Version& other) const noexcept;
    bool operator!=(const Version& other) const noexcept;
    bool operator<(const Version& other) const noexcept;
    bool operator>(const Version& other) const noexcept;
    bool operator<=(const Version& other) const noexcept;
    bool operator>=(const Version& other) const noexcept;
};

std::ostream& operator<<(std::ostream& os, const Version& version);

// C++17 inline 变量（多 TU 单实例）：公共 API 中唯一的 extern 数据——
// 共享构建下 extern 数据需要跨 DLL 导入导出接线，inline 变量彻底消除
// dllimport 面，SHARED/STATIC 两形态零差别
inline const Version FALCON_VERSION{0, 1, 0};

/// 获取完整版本字符串（包含编译信息）
const char* get_version_string();

/// 获取构建时间
const char* get_build_timestamp();

} // namespace falcon
