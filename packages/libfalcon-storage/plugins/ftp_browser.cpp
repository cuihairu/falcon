/**
 * @file ftp_browser.cpp
 * @brief FTP资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/storage/ftp_browser.hpp>
#include <falcon/logger.hpp>

#include <curl/curl.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <ctime>
#include <cstdlib>
#include <cstdint>

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

        // 剥离内嵌的 userinfo（ftp://user:pass@host 的凭据不属于 host）
        size_t at_pos = host_.find('@');
        if (at_pos != std::string::npos) {
            host_ = host_.substr(at_pos + 1);
        }

        if (host_end < url.length()) {
            current_path_ = url.substr(host_end);
        }

        // 移除查询参数
        size_t query_pos = current_path_.find('?');
        if (query_pos != std::string::npos) {
            current_path_ = current_path_.substr(0, query_pos);
        }
    }

    /// 把传入 path 按与 build_url 相同的语义解析为服务器绝对路径
    /// （列举行的 res.path 归属此前用的是 current_path_ 而非实际列举
    /// 的目录，path 参数与当前目录不一致时路径全错）
    std::string resolve_path(const std::string& path) const {
        if (path.empty()) return normalize_path(current_path_);
        if (path[0] == '/') return normalize_path(path);
        return normalize_path(current_path_ + "/" + path);
    }

    std::string parent_of(const std::string& path) const {
        size_t pos = path.find_last_of('/');
        return pos == std::string::npos ? std::string() : path.substr(0, pos);
    }

    std::string build_url(const std::string& path = "", bool is_dir = true) {
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
        std::string full_path = resolve_path(path);

        // 目录操作补尾部斜杠；文件操作（DELE/RMD/SIZE 等）必须不带，
        // 否则服务器按目录解析必失败
        if (is_dir && (full_path.empty() || full_path.back() != '/')) {
            full_path += "/";
        }

        url += full_path;

        return url;
    }

    void apply_ssl_option(const std::string& value) {
        std::string v = value;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        if (v == "false" || v == "0" || v == "none" || v == "off") {
            curl_easy_setopt(curl_, CURLOPT_USE_SSL, CURLUSESSL_NONE);
        } else if (v == "control") {
            curl_easy_setopt(curl_, CURLOPT_USE_SSL, CURLUSESSL_CONTROL);
        } else if (v == "all" || v == "true" || v == "1") {
            curl_easy_setopt(curl_, CURLOPT_USE_SSL, CURLUSESSL_ALL);
        }
        // 其他值保持构造时的 CURLUSESSL_TRY
    }

    std::string perform_list(const std::string& url) {
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);

        std::string response;
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

        // test_connection/写操作在复用的 handle 上残留 NOBODY=1 会让
        // LIST 收不到任何数据（连接之后列举恒空）——每次显式清设
        curl_easy_setopt(curl_, CURLOPT_NOBODY, 0L);

        // 使用LIST命令
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "LIST");

        CURLcode res = curl_easy_perform(curl_);
        if (res != CURLE_OK) {
            FALCON_LOG_ERROR("FTP LIST failed: {}", curl_easy_strerror(res));
            return "";
        }

        return response;
    }

    std::vector<RemoteResource> parse_ftp_listing(
        const std::string& listing,
        const ListOptions& options,
        const std::string& base_dir) {

        std::vector<RemoteResource> resources;
        std::istringstream iss(listing);
        std::string line;

        while (std::getline(iss, line)) {
            // 跳过空行和总行
            if (line.empty() || line.find("total") == 0) continue;

            // 解析每行
            RemoteResource res = parse_ftp_line(line, base_dir);
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

    /// days_from_civil（Howard Hinnant 算法）：y/m/d → 自 1970-01-01 的
    /// 天数，纯整数 UTC 算术，跨平台且不受本地时区影响
    static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const int64_t yoe = y - era * 400;                      // [0, 399]
        const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    /// 解析 Unix ls 的时间字段为 unix 秒字符串（与其他浏览器的
    /// modified_time 格式一致）：一年内文件是 "Mon DD HH:MM"（年份缺省
    /// 按当前 UTC 年近似），更早文件是 "Mon DD YYYY"；失败返回空串
    static std::string parse_ls_time(const std::string& month, const std::string& day,
                                     const std::string& tail) {
        static const std::map<std::string, int> kMonths = {
            {"Jan", 1}, {"Feb", 2}, {"Mar", 3}, {"Apr", 4}, {"May", 5}, {"Jun", 6},
            {"Jul", 7}, {"Aug", 8}, {"Sep", 9}, {"Oct", 10}, {"Nov", 11}, {"Dec", 12}};

        auto mit = kMonths.find(month);
        if (mit == kMonths.end()) return "";

        int mday = std::atoi(day.c_str());
        if (mday <= 0 || mday > 31) return "";

        int64_t secs;
        size_t colon = tail.find(':');
        if (colon != std::string::npos) {
            std::time_t now = std::time(nullptr);
            std::tm* utc = std::gmtime(&now);
            int64_t year = utc ? utc->tm_year + 1900 : 1970;
            int hour = std::atoi(tail.substr(0, colon).c_str());
            int minute = std::atoi(tail.substr(colon + 1).c_str());
            secs = days_from_civil(year, mit->second, mday) * 86400
                   + hour * 3600 + minute * 60;
        } else {
            int64_t year = std::atoi(tail.c_str());
            if (year <= 1900) return "";
            secs = days_from_civil(year, mit->second, mday) * 86400;
        }
        return std::to_string(secs);
    }

    RemoteResource parse_ftp_line(const std::string& line, const std::string& base_dir) {
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

        // 时间字段（此前读了但丢弃，modified_time 恒空）
        std::string month_str, day_str, tail_str;
        iss >> month_str >> day_str >> tail_str;
        res.modified_time = parse_ls_time(month_str, day_str, tail_str);

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

        // 构建完整路径（归属实际列举的目录，而非当前工作目录）
        res.path = normalize_path(base_dir + "/" + res.name);

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
                    if (options.sort_desc) {
                        return a.modified_time > b.modified_time;
                    }
                    return a.modified_time < b.modified_time;
                });
        }
    }

    static std::string normalize_path(const std::string& path) {
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

    /// NOBODY 探测类请求（连接测试/SIZE/QUOTE 控制命令）的统一 perform：
    /// 显式挂接本次调用的接收缓冲并清掉残留的 CUSTOMREQUEST。此前复用
    /// 的 handle 残留 perform_list 的 WRITEDATA（指向已销毁的局部
    /// string），NOBODY 应答触发 WriteCallback 即 stack-use-after-return
    CURLcode perform_nobody(const std::string& url) {
        std::string sink;
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);

        CURLcode res = curl_easy_perform(curl_);

        curl_easy_setopt(curl_, CURLOPT_NOBODY, 0L);
        return res;
    }

    bool test_connection() {
        return perform_nobody(build_url()) == CURLE_OK;
    }

    bool remove_recursive(const std::string& path) {
        ListOptions options;
        std::string base = resolve_path(path);
        auto contents = parse_ftp_listing(perform_list(build_url(path)), options, base);

        for (const auto& item : contents) {
            if (item.is_directory()) {
                remove_recursive(item.path);
            } else {
                remove(item.path);
            }
        }

        return remove(path);
    }

    bool remove(const std::string& path) {
        // 先列父目录拿到条目类型（目录 RMD / 文件 DELE）
        std::string parent = parent_of(path);
        std::string name = path.substr(path.find_last_of('/') + 1);

        RemoteResource info;
        ListOptions options;
        auto contents = parse_ftp_listing(
            perform_list(build_url(parent)), options, resolve_path(parent));

        for (const auto& res : contents) {
            if (res.name == name) {
                info = res;
                break;
            }
        }

        // DELE/RMD 走 QUOTE 下发：CUSTOMREQUEST 会让 curl 先 CWD 进目标
        // 路径再发命令，删除尚未发生 CWD 就已经失败
        std::string cmd =
            std::string(info.is_directory() ? "RMD " : "DELE ") + resolve_path(path);
        return perform_control_commands({cmd}, parent);
    }

    /// 以父目录为传输锚点执行控制命令（QUOTE 语义：登录后、传输阶段
    /// 前，任一命令被服务器拒绝即整体失败）。MKD/DELE/RMD/RNFR/RNTO
    /// 这类操作不能塞 CUSTOMREQUEST——curl 会先 CWD 进目标路径再发命令，
    /// 目标尚不存在（MKD）或即将消失（DELE/RMD）时 CWD 必然失败
    bool perform_control_commands(const std::vector<std::string>& cmds,
                                  const std::string& anchor_path) {
        struct curl_slist* commands = nullptr;
        for (const auto& c : cmds) {
            commands = curl_slist_append(commands, c.c_str());
        }

        curl_easy_setopt(curl_, CURLOPT_QUOTE, commands);
        CURLcode res = perform_nobody(build_url(anchor_path));
        curl_easy_setopt(curl_, CURLOPT_QUOTE, nullptr);
        curl_slist_free_all(commands);

        if (res != CURLE_OK) {
            FALCON_LOG_ERROR("FTP control command failed: {}", curl_easy_strerror(res));
            return false;
        }
        return true;
    }

    bool perform_rename(const std::string& old_path, const std::string& new_path) {
        // RNFR/RNTO 必须是两条独立的 FTP 命令按序下发；此前把整串
        // "RNFR a\r\nRNTO b" 塞进 CUSTOMREQUEST，协议上就是一条非法命令
        std::string rnfr = "RNFR " + resolve_path(old_path);
        std::string rnto = "RNTO " + resolve_path(new_path);
        return perform_control_commands({rnfr, rnto}, parent_of(old_path));
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

    // endpoint 选项覆盖主机（host[:port]，与其他浏览器保持一致）
    it = options.find("endpoint");
    if (it != options.end() && !it->second.empty()) {
        p_impl_->host_ = it->second;
    }

    // TLS 强度选项（none/control/all，缺省保持构造时的 try）
    it = options.find("ssl");
    if (it != options.end() && !it->second.empty()) {
        p_impl_->apply_ssl_option(it->second);
    }

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

    return p_impl_->parse_ftp_listing(listing, options, p_impl_->resolve_path(path));
}

RemoteResource FTPBrowser::get_resource_info(const std::string& path) {
    std::string parent = p_impl_->parent_of(path);
    std::vector<RemoteResource> resources = list_directory(parent);

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

    // 使用SIZE命令获取大小（NOBODY 探测统一走 perform_nobody，
    // 不复用 perform_list 残留的 WRITEDATA）
    CURLcode res = p_impl_->perform_nobody(
        p_impl_->build_url(path, /*is_dir=*/false));
    if (res != CURLE_OK) {
        // 父目录列举与 SIZE 双双失败：资源不存在（exists 以 name
        // 非空判定，恒真条件会让幽灵路径也报"存在"）
        info.name.clear();
        return info;
    }

    curl_off_t size = -1;
    curl_easy_getinfo(p_impl_->curl_, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &size);
    info.size = size >= 0 ? static_cast<uint64_t>(size) : 0;
    info.type = ResourceType::File;

    return info;
}

bool FTPBrowser::create_directory(const std::string& path, [[maybe_unused]] bool recursive) {
    // MKD 经 QUOTE 下发并以父目录为锚点（CUSTOMREQUEST 会先 CWD 进
    // 尚不存在的目标目录）；recursive 参数暂不支持级联建父目录
    return p_impl_->perform_control_commands(
        {"MKD " + p_impl_->resolve_path(path)}, p_impl_->parent_of(path));
}

bool FTPBrowser::remove(const std::string& path, bool recursive) {
    if (recursive) {
        return p_impl_->remove_recursive(path);
    }
    return p_impl_->remove(path);
}

bool FTPBrowser::rename(const std::string& old_path, const std::string& new_path) {
    return p_impl_->perform_rename(old_path, new_path);
}

bool FTPBrowser::copy([[maybe_unused]] const std::string& source_path,
                      [[maybe_unused]] const std::string& dest_path) {
    // FTP不直接支持复制，需要先下载再上传
    FALCON_LOG_ERROR("FTP does not support direct copy operation");
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