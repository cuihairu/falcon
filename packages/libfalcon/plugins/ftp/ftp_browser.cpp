/**
 * @file ftp_browser.cpp
 * @brief FTP资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/ftp_browser.hpp>
#include <falcon/logger.hpp>
#include <curl/curl.h>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace falcon {

/**
 * @brief FTP浏览器实现细节
 */
class FTPBrowser::Impl {
public:
    Impl() : curl_(curl_easy_init()) {
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        // 设置FTP选项
        curl_easy_setopt(curl_, CURLOPT_USE_SSL, CURLUSESSL_TRY);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    ~Impl() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    void parse_url(const std::string& url) {
        // 简单的URL解析
        size_t protocol_end = url.find("://");
        if (protocol_end == std::string::npos) return;

        size_t host_start = protocol_end + 3;
        size_t host_end = url.find('/', host_start);
        if (host_end == std::string::npos) host_end = url.length();

        host_ = url.substr(host_start, host_end - host_start);

        if (host_end < url.length()) {
            current_path_ = url.substr(host_end);
        }

        // 移除查询参数
        size_t query_pos = current_path_.find('?');
        if (query_pos != std::string::npos) {
            current_path_ = current_path_.substr(0, query_pos);
        }
    }

    std::string build_url(const std::string& path = "") {
        std::string url = "ftp://";

        // 添加用户名密码（如果有）
        if (!username_.empty()) {
            url += username_;
            if (!password_.empty()) {
                url += ":" + password_;
            }
            url += "@";
        }

        url += host_;

        // 添加路径
        std::string full_path = current_path_;
        if (!path.empty()) {
            if (path[0] == '/') {
                full_path = path;
            } else {
                full_path = normalize_path(current_path_ + "/" + path);
            }
        }

        if (full_path.empty() || full_path.back() != '/') {
            full_path += "/";
        }

        url += full_path;

        return url;
    }

    std::string perform_list(const std::string& url) {
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);

        std::string response;
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

        // 使用LIST命令
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "LIST");

        CURLcode res = curl_easy_perform(curl_);
        if (res != CURLE_OK) {
            Logger::error("FTP LIST failed: {}", curl_easy_strerror(res));
            return "";
        }

        return response;
    }

    std::vector<RemoteResource> parse_ftp_listing(
        const std::string& listing,
        const ListOptions& options) {

        std::vector<RemoteResource> resources;
        std::istringstream iss(listing);
        std::string line;

        while (std::getline(iss, line)) {
            // 跳过空行和总行
            if (line.empty() || line.find("total") == 0) continue;

            // 解析每行
            RemoteResource res = parse_ftp_line(line);
            if (!res.name.empty()) {
                // 应用过滤器
                if (apply_filter(res, options)) {
                    resources.push_back(res);
                }
            }
        }

        // 排序
        sort_resources(resources, options);

        return resources;
    }

    RemoteResource parse_ftp_line(const std::string& line) {
        RemoteResource res;

        // Unix风格FTP列表
        if (line.length() < 10) return res;

        // 解析权限
        res.permissions = FilePermissions();
        if (line[0] == 'd') {
            res.type = ResourceType::Directory;
        } else if (line[0] == 'l') {
            res.type = ResourceType::Symlink;
        } else if (line[0] == '-') {
            res.type = ResourceType::File;
        } else {
            return res; // 未知格式
        }

        // 解析权限位
        if (line.length() >= 10) {
            if (line[1] == 'r') res.permissions.owner_read = true;
            if (line[2] == 'w') res.permissions.owner_write = true;
            if (line[3] == 'x') res.permissions.owner_execute = true;
            if (line[4] == 'r') res.permissions.group_read = true;
            if (line[5] == 'w') res.permissions.group_write = true;
            if (line[6] == 'x') res.permissions.group_execute = true;
            if (line[7] == 'r') res.permissions.other_read = true;
            if (line[8] == 'w') res.permissions.other_write = true;
            if (line[9] == 'x') res.permissions.other_execute = true;
        }

        // 解析其他字段（简化实现）
        std::istringstream iss(line.substr(10));
        std::string token;

        // 跳过硬链接数
        iss >> token;

        // 所有者
        iss >> res.owner;

        // 组
        iss >> res.group;

        // 大小
        iss >> res.size;

        // 月份
        iss >> token;

        // 日期
        iss >> token;
        std::string time_str;
        iss >> time_str;

        // 名称（包含空格）
        std::string rest;
        std::getline(iss, rest);

        // 跳过空格
        size_t name_start = rest.find_first_not_of(" \t");
        if (name_start != std::string::npos) {
            res.name = rest.substr(name_start);
        }

        // 符号链接特殊处理
        if (res.type == ResourceType::Symlink) {
            size_t arrow_pos = res.name.find(" -> ");
            if (arrow_pos != std::string::npos) {
                res.symlink_target = res.name.substr(arrow_pos + 4);
                res.name = res.name.substr(0, arrow_pos);
            }
        }

        // 构建完整路径
        res.path = normalize_path(current_path_ + "/" + res.name);

        return res;
    }

    bool apply_filter(const RemoteResource& res, const ListOptions& options) {
        // 隐藏文件过滤
        if (!options.show_hidden && res.name[0] == '.') {
            return false;
        }

        // 通配符过滤
        if (!options.filter.empty() && !match_wildcard(res.name, options.filter)) {
            return false;
        }

        return true;
    }

    bool match_wildcard(const std::string& str, const std::string& pattern) {
        // 简单的通配符匹配（只支持*）
        if (pattern == "*") return true;

        size_t pos = pattern.find('*');
        if (pos == std::string::npos) {
            return str == pattern;
        }

        std::string prefix = pattern.substr(0, pos);
        std::string suffix = pattern.substr(pos + 1);

        return str.length() >= prefix.length() + suffix.length() &&
               str.substr(0, prefix.length()) == prefix &&
               str.substr(str.length() - suffix.length()) == suffix;
    }

    void sort_resources(std::vector<RemoteResource>& resources, const ListOptions& options) {
        if (options.sort_by == "name") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    if (options.sort_desc) {
                        return a.name > b.name;
                    }
                    return a.name < b.name;
                });
        } else if (options.sort_by == "size") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    if (options.sort_desc) {
                        return a.size > b.size;
                    }
                    return a.size < b.size;
                });
        } else if (options.sort_by == "modified_time") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    return a.modified_time < b.modified_time;
                });
        }
    }

    std::string normalize_path(const std::string& path) {
        std::string result = path;

        // 替换\\为/
        std::replace(result.begin(), result.end(), '\\', '/');

        // 压缩多个/
        size_t pos = 0;
        while ((pos = result.find("//", pos)) != std::string::npos) {
            result.replace(pos, 2, "/");
        }

        // 处理相对路径
        if (result == "./") {
            result = "";
        } else if (result.length() > 1 && result.substr(0, 2) == "./") {
            result = result.substr(2);
        }

        return result;
    }

    bool test_connection() {
        std::string test_url = build_url();
        curl_easy_setopt(curl_, CURLOPT_URL, test_url.c_str());
        curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);

        CURLcode res = curl_easy_perform(curl_);
        return res == CURLE_OK;
    }

    bool remove_recursive(const std::string& path) {
        // 递归删除实现（简化版）
        ListOptions options;
        options.recursive = true;
        std::vector<RemoteResource> contents = parse_ftp_listing(
            perform_list(build_url(path)), options);

        for (const auto& item : contents) {
            std::string item_path = path + "/" + item.name;

            if (item.is_directory()) {
                remove_recursive(item_path);
            } else {
                remove(item_path);
            }
        }

        return remove(path);
    }

    bool remove(const std::string& path) {
        std::string url = build_url(path);

        RemoteResource info;
        ListOptions options;
        auto contents = parse_ftp_listing(perform_list(build_url(
            path.substr(0, path.find_last_of('/')))), options);

        std::string name = path.substr(path.find_last_of('/') + 1);
        for (const auto& res : contents) {
            if (res.name == name) {
                info = res;
                break;
            }
        }

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());

        if (info.is_directory()) {
            // 删除目录（需要支持RMD命令）
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "RMD");
        } else {
            // 删除文件
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELE");
        }

        CURLcode res = curl_easy_perform(curl_);
        return res == CURLE_OK;
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    CURL* curl_;
    std::string host_;
    std::string current_path_;
    std::string username_;
    std::string password_;
    std::string current_url_;
};

// FTPBrowser 公共接口实现
FTPBrowser::FTPBrowser() : p_impl_(std::make_unique<Impl>()) {}

FTPBrowser::~FTPBrowser() = default;

std::string FTPBrowser::get_name() const {
    return "FTP";
}

std::vector<std::string> FTPBrowser::get_supported_protocols() const {
    return {"ftp", "ftps"};
}

bool FTPBrowser::can_handle(const std::string& url) const {
    return url.find("ftp://") == 0 || url.find("ftps://") == 0;
}

bool FTPBrowser::connect(const std::string& url,
                         const std::map<std::string, std::string>& options) {
    p_impl_->current_url_ = url;

    // 提取认证信息
    auto it = options.find("username");
    if (it != options.end()) {
        p_impl_->username_ = it->second;
    }

    it = options.find("password");
    if (it != options.end()) {
        p_impl_->password_ = it->second;
    }

    // 解析URL获取主机信息
    p_impl_->parse_url(url);

    // 测试连接
    return p_impl_->test_connection();
}

void FTPBrowser::disconnect() {
    // FTP是无状态协议，不需要显式断开
}

std::vector<RemoteResource> FTPBrowser::list_directory(
    const std::string& path,
    const ListOptions& options) {

    std::string list_url = p_impl_->build_url(path);
    std::string listing = p_impl_->perform_list(list_url);

    return p_impl_->parse_ftp_listing(listing, options);
}

RemoteResource FTPBrowser::get_resource_info(const std::string& path) {
    std::vector<RemoteResource> resources = list_directory(
        path.substr(0, path.find_last_of('/')));

    std::string name = path.substr(path.find_last_of('/') + 1);
    for (const auto& res : resources) {
        if (res.name == name) {
            return res;
        }
    }

    // 如果找不到，尝试获取文件大小
    RemoteResource info;
    info.path = path;
    info.name = name;

    // 使用SIZE命令获取大小
    std::string size_url = p_impl_->build_url(path);
    curl_easy_setopt(p_impl_->curl_, CURLOPT_URL, size_url.c_str());
    curl_easy_setopt(p_impl_->curl_, CURLOPT_NOBODY, 1L);

    CURLcode res = curl_easy_perform(p_impl_->curl_);
    if (res == CURLE_OK) {
        curl_off_t size;
        curl_easy_getinfo(p_impl_->curl_, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &size);
        info.size = static_cast<uint64_t>(size);
        info.type = ResourceType::File;
    }

    return info;
}

bool FTPBrowser::create_directory(const std::string& path, bool recursive) {
    std::string url = p_impl_->build_url(path);

    // 使用MKD命令创建目录
    curl_easy_setopt(p_impl_->curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(p_impl_->curl_, CURLOPT_CUSTOMREQUEST, "MKD");

    CURLcode res = curl_easy_perform(p_impl_->curl_);
    return res == CURLE_OK;
}

bool FTPBrowser::remove(const std::string& path, bool recursive) {
    if (recursive) {
        return p_impl_->remove_recursive(path);
    }
    return p_impl_->remove(path);
}

bool FTPBrowser::rename(const std::string& old_path, const std::string& new_path) {
    std::string old_url = p_impl_->build_url(old_path);
    std::string new_url = p_impl_->build_url(new_path);

    // 使用RNFR命令重命名
    std::string command = "RNFR " + old_url + "\r\nRNTO " + new_url;

    curl_easy_setopt(p_impl_->curl_, CURLOPT_URL, old_url.c_str());
    curl_easy_setopt(p_impl_->curl_, CURLOPT_CUSTOMREQUEST, command.c_str());

    CURLcode res = curl_easy_perform(p_impl_->curl_);
    return res == CURLE_OK;
}

bool FTPBrowser::copy(const std::string& source_path, const std::string& dest_path) {
    // FTP不直接支持复制，需要先下载再上传
    Logger::error("FTP does not support direct copy operation");
    return false;
}

bool FTPBrowser::exists(const std::string& path) {
    RemoteResource info = get_resource_info(path);
    return !info.name.empty();
}

std::string FTPBrowser::get_current_directory() {
    return p_impl_->current_path_;
}

bool FTPBrowser::change_directory(const std::string& path) {
    p_impl_->current_path_ = p_impl_->normalize_path(path);
    return true; // FTP会在下次操作时应用新路径
}

std::string FTPBrowser::get_root_path() {
    return "/";
}

std::map<std::string, uint64_t> FTPBrowser::get_quota_info() {
    // FTP不支持配额查询
    return {};
}

} // namespace falcon