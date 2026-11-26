/**
 * @file resource_browser_utils.cpp
 * @brief 资源浏览器工具类实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/storage/resource_browser.hpp>
#include <algorithm>
#include <cctype>

namespace falcon {

//==============================================================================
// ResourceBrowserUtils 实现
//==============================================================================

bool ResourceBrowserUtils::is_valid_path(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    // 检查路径是否包含无效字符（Windows上不允许的字符）
    // 简单检查：不允许控制字符（除了制表符在极少数情况下）
    for (char c : path) {
        if (static_cast<unsigned char>(c) < 32 && c != '\t') {
            return false;
        }
    }

    return true;
}

std::string ResourceBrowserUtils::normalize_path(const std::string& path) {
    if (path.empty()) {
        return ".";
    }

    std::string result;
    result.reserve(path.length());

    bool is_absolute = !path.empty() && (path[0] == '/' || (path.length() > 1 && path[1] == ':'));

    // Windows drive letter path (e.g., "C:/")
    size_t start = 0;
    if (path.length() > 1 && path[1] == ':') {
        result += path[0];
        result += ':';
        start = 2;
    }

    std::vector<std::string> components;
    std::string current;

    // 检查原始路径是否以斜杠结尾（用于保留目录标记）
    bool ends_with_slash = false;
    size_t path_len = path.length();
    if (path_len > start) {
        char last_char = path[path_len - 1];
        ends_with_slash = (last_char == '/' || last_char == '\\');
    }

    for (size_t i = start; i < path.length(); ++i) {
        char c = path[i];

        if (c == '/' || c == '\\') {
            // 跳过空组件，但处理连续分隔符
            if (!current.empty()) {
                if (current == ".") {
                    // 当前目录，跳过
                } else if (current == "..") {
                    // 父目录
                    if (!components.empty() && components.back() != "..") {
                        components.pop_back();
                    } else {
                        components.push_back("..");
                    }
                } else {
                    components.push_back(current);
                }
                current.clear();
            }
        } else {
            current += c;
        }
    }

    // 添加最后一个组件
    if (!current.empty()) {
        if (current == ".") {
            // 跳过
        } else if (current == "..") {
            if (!components.empty() && components.back() != "..") {
                components.pop_back();
            } else {
                components.push_back("..");
            }
        } else {
            components.push_back(current);
        }
    }

    // 构建结果
    if (is_absolute) {
        result += "/";
    }

    for (size_t i = 0; i < components.size(); ++i) {
        if (i > 0 || !is_absolute) {
            result += "/";
        }
        result += components[i];
    }

    // 空路径返回 "."
    if (result.empty()) {
        return ".";
    }

    // 保留尾部斜杠表示目录
    if (ends_with_slash && !result.empty() && result.back() != '/' && result != "/") {
        result += "/";
    }

    return result;
}

std::string ResourceBrowserUtils::join_path(const std::string& base, const std::string& name) {
    if (base.empty()) {
        return name;
    }

    if (name.empty()) {
        return base;
    }

    // 检查是否是绝对路径
    bool name_is_absolute = (!name.empty() && (name[0] == '/' ||
                                   (name.length() > 1 && name[1] == ':')));
    if (name_is_absolute) {
        return name;
    }

    std::string result = base;

    // 如果基础路径不以 / 结尾且名称不以 / 开头，添加分隔符
    if (result.back() != '/' && result.back() != '\\') {
        result += "/";
    }

    // Windows: 需要处理驱动器根目录的情况
    // 例如 "C:" + "file" 应该是 "C:file" 不是 "C:/file"
    bool needs_separator = true;
    if (result.length() > 1 && result[1] == ':') {
        // 可能是 "C:" 格式
        if (result.length() == 2 || (result[2] == '/' || result[2] == '\\')) {
            needs_separator = false;
        }
    }

    if (needs_separator && result.back() != ':' && result.back() != '/' && result.back() != '\\') {
        result += "/";
    }

    result += name;
    return result;
}

std::string ResourceBrowserUtils::get_parent_path(const std::string& path) {
    if (path.empty() || path == "/" || path == ".") {
        return "";  // 根目录没有父目录
    }

    size_t end = path.length() - 1;

    // 移除尾部斜杠
    while (end > 0 && (path[end] == '/' || path[end] == '\\')) {
        --end;
    }

    // 查找最后一个分隔符
    while (end > 0) {
        char c = path[end];
        if (c == '/' || c == '\\') {
            ++end;  // 保留分隔符
            break;
        }
        --end;
    }

    if (end == 0 && path[0] != '/' && path[0] != '\\') {
        return ".";  // 当前目录
    }

    // Windows drive letter case
    if (end == 2 && path[1] == ':') {
        return path.substr(0, 2);
    }

    // 处理根目录
    if (end == 0 && (path[0] == '/' || path[0] == '\\')) {
        return "/";
    }

    return path.substr(0, end);
}

std::string ResourceBrowserUtils::get_filename(const std::string& path) {
    if (path.empty()) {
        return "";
    }

    size_t start = path.length() - 1;

    // 移除尾部斜杠
    while (start > 0 && (path[start] == '/' || path[start] == '\\')) {
        --start;
    }

    // 查找最后一个分隔符
    size_t end = start + 1;
    while (end > 0) {
        char c = path[end - 1];
        if (c == '/' || c == '\\' || c == ':') {
            break;
        }
        --end;
    }

    return path.substr(end, start - end + 1);
}

} // namespace falcon
