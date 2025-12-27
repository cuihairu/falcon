/**
 * @file resource_browser.cpp
 * @brief 远程资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/resource_browser.hpp>
#include <falcon/logger.hpp>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace falcon {

// FilePermissions 实现
std::string FilePermissions::to_string() const {
    std::string result(9, '-');

    if (owner_read) result[0] = 'r';
    if (owner_write) result[1] = 'w';
    if (owner_execute) result[2] = 'x';

    if (group_read) result[3] = 'r';
    if (group_write) result[4] = 'w';
    if (group_execute) result[5] = 'x';

    if (other_read) result[6] = 'r';
    if (other_write) result[7] = 'w';
    if (other_execute) result[8] = 'x';

    return result;
}

FilePermissions FilePermissions::from_octal(int mode) {
    FilePermissions perms;
    perms.owner_read = (mode & 0400) != 0;
    perms.owner_write = (mode & 0200) != 0;
    perms.owner_execute = (mode & 0100) != 0;
    perms.group_read = (mode & 0040) != 0;
    perms.group_write = (mode & 0020) != 0;
    perms.group_execute = (mode & 0010) != 0;
    perms.other_read = (mode & 0004) != 0;
    perms.other_write = (mode & 0002) != 0;
    perms.other_execute = (mode & 0001) != 0;
    return perms;
}

// RemoteResource 实现
std::string RemoteResource::display_name() const {
    if (is_directory()) {
        return name + "/";
    }
    if (type == ResourceType::Symlink) {
        return name + " -> " + symlink_target;
    }
    return name;
}

std::string RemoteResource::formatted_size() const {
    if (type != ResourceType::File) return "-";

    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double size_bytes = static_cast<double>(size);
    int unit_index = 0;

    while (size_bytes >= 1024.0 && unit_index < 4) {
        size_bytes /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    if (unit_index == 0) {
        oss << static_cast<int64_t>(size);
    } else {
        oss << std::fixed << std::setprecision(1) << size_bytes;
    }
    oss << " " << units[unit_index];

    return oss.str();
}

// BrowserFormatter 实现
std::string BrowserFormatter::format_short(const std::vector<RemoteResource>& resources) {
    std::ostringstream oss;
    oss << "total " << resources.size() << "\n";

    for (const auto& res : resources) {
        oss << res.display_name() << "\n";
    }

    return oss.str();
}

std::string BrowserFormatter::format_long(const std::vector<RemoteResource>& resources) {
    std::ostringstream oss;

    // 列标题
    oss << "Permissions  Size    Modified            Owner  Group  Name\n";
    oss << "----------  ------  -------------------  -----  -----  ----\n";

    for (const auto& res : resources) {
        // 权限
        oss << res.permissions.to_string() << " ";

        // 大小
        if (res.is_file()) {
            std::ostringstream size_oss;
            size_oss << std::setw(6) << res.formatted_size();
            oss << size_oss.str();
        } else {
            oss << std::setw(7) << "-";
        }
        oss << " ";

        // 修改时间
        oss << std::setw(19) << res.modified_time.substr(0, 19) << " ";

        // 所有者
        oss << std::setw(5) << res.owner.substr(0, 5) << " ";

        // 组
        oss << std::setw(5) << res.group.substr(0, 5) << " ";

        // 名称
        oss << res.display_name() << "\n";
    }

    return oss.str();
}

std::string BrowserFormatter::format_tree(const std::vector<RemoteResource>& resources,
                                         const std::string& base_path,
                                         int max_depth) {
    std::ostringstream oss;

    // 简单的树形显示实现
    std::function<void(const std::string&, int)> print_tree;
    print_tree = [&](const std::string& /* path */, int depth) {
        if (max_depth > 0 && depth >= max_depth) return;

        // 这里应该递归获取子目录
        // 为了简化，只显示当前层级的树形结构
        for (size_t i = 0; i < resources.size(); ++i) {
            const auto& res = resources[i];
            std::string prefix(static_cast<size_t>(depth) * 2, ' ');

            if (i == resources.size() - 1) {
                prefix += "└── ";
            } else {
                prefix += "├── ";
            }

            oss << prefix << res.display_name() << "\n";
        }
    };

    oss << base_path << "\n";
    print_tree(base_path, 0);

    return oss.str();
}

std::string BrowserFormatter::format_table(const std::vector<RemoteResource>& resources) {
    std::ostringstream oss;

    // 使用类似ls的格式
    for (const auto& res : resources) {
        // 类型标识
        if (res.is_directory()) {
            oss << "d";
        } else if (res.type == ResourceType::Symlink) {
            oss << "l";
        } else {
            oss << "-";
        }

        // 权限
        oss << res.permissions.to_string() << " ";

        // 所有者
        oss << std::setw(8) << std::left << res.owner.substr(0, 8) << " ";

        // 组
        oss << std::setw(8) << std::left << res.group.substr(0, 8) << " ";

        // 大小
        if (res.is_file()) {
            oss << std::setw(10) << std::right << res.size;
        } else {
            oss << std::setw(10) << std::right << "-";
        }
        oss << " ";

        // 月份和日期（简化显示）
        std::tm tm = {};
        std::istringstream ss(res.modified_time);
        if (ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S")) {
            char date_str[13];
            std::strftime(date_str, sizeof(date_str), "%b %d %H:%M", &tm);
            oss << std::setw(12) << std::left << date_str << " ";
        } else {
            oss << std::setw(12) << std::left << "- " << " ";
        }

        // 名称
        oss << res.display_name() << "\n";
    }

    return oss.str();
}

std::string BrowserFormatter::format_custom(
    const std::vector<RemoteResource>& resources,
    const std::vector<std::string>& columns) {

    std::ostringstream oss;

    // 构建表头
    for (const auto& col : columns) {
        oss << std::setw(15) << std::left << col << " ";
    }
    oss << "\n";

    // 分隔线
    for (size_t i = 0; i < columns.size(); ++i) {
        oss << std::string(15, '-');
        if (i < columns.size() - 1) oss << " ";
    }
    oss << "\n";

    // 数据行
    for (const auto& res : resources) {
        for (const auto& col : columns) {
            if (col == "name") {
                oss << std::setw(15) << std::left << res.name.substr(0, 15) << " ";
            } else if (col == "size") {
                oss << std::setw(15) << std::right << res.formatted_size() << " ";
            } else if (col == "type") {
                oss << std::setw(15) << std::left <<
                    (res.is_directory() ? "directory" :
                     res.is_file() ? "file" : "other") << " ";
            } else if (col == "modified") {
                oss << std::setw(15) << std::left <<
                    res.modified_time.substr(0, 15) << " ";
            } else if (col == "owner") {
                oss << std::setw(15) << std::left << res.owner.substr(0, 15) << " ";
            } else {
                oss << std::setw(15) << std::left << "-" << " ";
            }
        }
        oss << "\n";
    }

    return oss.str();
}

} // namespace falcon