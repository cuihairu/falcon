/**
 * @file webdav_browser.cpp
 * @brief WebDAV资源浏览器实现
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 协议面：PROPFIND（浏览/元数据，Depth 0/1）、MKCOL（建目录）、
 * DELETE（删除，递归附 Depth: infinity + 逐项兜底）、MOVE/COPY
 * （重命名/复制，Destination 绝对 URL）。鉴权交由 curl 协商
 * （CURLOPT_HTTPAUTH ANY：Basic/Digest 均可，坚果云为 Basic、
 * 部分 NAS 为 Digest）。命名空间各异的 207 multistatus 解析见
 * webdav_propfind_xml.hpp。
 */

#include <falcon/storage/webdav_browser.hpp>
#include <falcon/detail/injection.hpp>
#include <falcon/logger.hpp>

#include "webdav_propfind_xml.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace falcon {

// WebDavUrlParser 实现
WebDavUrl WebDavUrlParser::parse(const std::string& url) {
    WebDavUrl out;
    out.scheme.clear();  // 失败路径的标记：struct 默认值是 "http"
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return out;
    }
    const std::string scheme = url.substr(0, scheme_end);
    if (scheme == "webdav" || scheme == "dav") {
        out.scheme = "http";
    } else if (scheme == "webdavs" || scheme == "davs") {
        out.scheme = "https";
    } else {
        out.scheme.clear();  // 不认识的 scheme：connect 时报错
        return out;
    }

    std::string rest = url.substr(scheme_end + 3);
    // URL 内嵌凭据 user:pass@（options 显式传入时优先）
    const auto at = rest.rfind('@');
    if (at != std::string::npos && rest.find('/') > at) {
        const std::string userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        const auto colon = userinfo.find(':');
        if (colon == std::string::npos) {
            out.username = detail::percent_decode(userinfo);
        } else {
            out.username = detail::percent_decode(userinfo.substr(0, colon));
            out.password = detail::percent_decode(userinfo.substr(colon + 1));
        }
    }
    // host[:port][/base_path]
    const auto slash = rest.find('/');
    const std::string authority =
        (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.base_path = (slash == std::string::npos) ? "" : rest.substr(slash);
    while (!out.base_path.empty() && out.base_path.back() == '/') {
        out.base_path.pop_back();
    }
    if (out.base_path.empty() || out.base_path[0] != '/') {
        out.base_path.insert(out.base_path.begin(), '/');
    }
    if (authority.empty()) {
        return out;
    }
    if (!authority.empty() && authority[0] == '[') {
        // IPv6 字面量 [::1]:5244
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            return out;
        }
        out.host = authority.substr(0, close + 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            out.port = authority.substr(close + 2);
        }
        return out;
    }
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos &&
        authority.find(':') == colon) {  // 只有一个冒号才是 host:port
        out.host = authority.substr(0, colon);
        out.port = authority.substr(colon + 1);
    } else {
        out.host = authority;
    }
    return out;
}

namespace {

/// PROPFIND 请求体：命名属性集（浏览器列表/元数据所需的最小面）
const char* kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
    "<d:displayname/><d:getcontentlength/><d:getlastmodified/>"
    "<d:resourcetype/><d:getetag/><d:getcontenttype/>"
    "</d:prop></d:propfind>";

/// PROPFIND 请求体：allprop（配额字段 RFC 4331 仅在 allprop/命名请求下返回）
const char* kPropfindAllpropBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:allprop/></d:propfind>";

} // namespace

/**
 * @brief WebDAV浏览器实现细节
 */
class WebDavBrowser::Impl {
public:
    Impl() {
        // 注入命中时短路真实调用，避免已创建句柄在 throw 路径泄漏
        curl_ = detail::inject_failure(detail::InjectPoint::CurlEasyInit)
                    ? nullptr
                    : curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }
    }

    ~Impl() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    /// 虚拟路径规范化：'' 与 '/' 均为根；保证前导 '/'、去尾 '/'（根除外）
    std::string normalize_virtual(const std::string& path) const {
        if (path.empty() || path == "/") {
            return "/";
        }
        std::string result = path;
        while (!result.empty() && result.back() == '/') {
            result.pop_back();
        }
        if (result.empty() || result[0] != '/') {
            result.insert(result.begin(), '/');
        }
        return result;
    }

    /// 路径逐段编码、保留 '/'（整段编码会把 '/' 变 %2F，真服务器必然 404）
    std::string encode_path(const std::string& path) const {
        std::ostringstream encoded;
        encoded.fill('0');
        encoded << std::hex;
        for (char c : path) {
            if (c == '/') {
                encoded << '/';
            } else if (std::isalnum(static_cast<unsigned char>(c)) ||
                       c == '-' || c == '_' || c == '.' || c == '~') {
                encoded << c;
            } else {
                encoded << '%' << std::uppercase << std::setw(2)
                        << static_cast<int>(static_cast<unsigned char>(c));
            }
        }
        return encoded.str();
    }

    /// 虚拟路径 → 完整请求 URL（endpoint base_path 前缀 + 编码路径）
    std::string build_url(const std::string& virtual_path) const {
        std::string url = url_.scheme + "://" + url_.host;
        if (!url_.port.empty()) {
            url += ":" + url_.port;
        }
        std::string path = normalize_virtual(virtual_path);
        if (path == "/") {
            path = "";
        }
        url += url_.base_path + encode_path(path);
        return url;
    }

    /// base_path 前缀剥离：服务器路径 → 虚拟路径（无前导/尾随 '/'）
    std::string strip_base(const std::string& server_path) const {
        std::string path = server_path;
        if (!url_.base_path.empty() &&
            path.rfind(url_.base_path, 0) == 0) {
            path = path.substr(url_.base_path.size());
        }
        while (!path.empty() && path.front() == '/') {
            path.erase(path.begin());
        }
        while (!path.empty() && path.back() == '/') {
            path.pop_back();
        }
        return path;
    }

    /// 请求路径（解码形态），用于识别 multistatus 里的自条目
    std::string request_path(const std::string& virtual_path) const {
        std::string path = normalize_virtual(virtual_path);
        if (path == "/") {
            path = "";
        }
        return url_.base_path + path;
    }

    bool perform_request(
        const std::string& method,
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& body,
        long* status_out = nullptr,
        bool* ok = nullptr) {

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());
        // handle 复用：空 body 请求传合法空串指针清除残留——置 nullptr
        // 会让 curl 退回 read-callback 传输，MKCOL/DELETE 等 无体请求
        // 被套上 Transfer-Encoding: chunked + Expect: 100-continue，
        // Alist 等服务端对带体的 MKCOL/DELETE 直接 405/500 拒收
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS,
                         body.empty() ? "" : body.c_str());

        curl_slist* header_list = nullptr;
        for (const auto& [key, value] : headers) {
            std::string header = key + ": " + value;
            header_list = curl_slist_append(header_list, header.c_str());
        }
        // 无条件重设（含 nullptr 清除）：handle 复用下，上一请求的
        // header list 已 free，空头请求不清设会留下悬垂指针
        //（MKCOL/DELETE 无自定义头，curl 构建请求时读旧链表即段错误）
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_list);

        std::string response;
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl_);

        if (header_list) {
            curl_slist_free_all(header_list);
        }

        long status = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
        if (status_out) {
            *status_out = status;
        }
        const bool request_ok = (res == CURLE_OK) && status >= 200 && status < 400;
        if (ok) {
            *ok = request_ok;
        }
        if (!request_ok) {
            if (res != CURLE_OK) {
                FALCON_LOG_ERROR("WebDAV request failed: {}",
                                 curl_easy_strerror(res));
            } else {
                FALCON_LOG_ERROR("WebDAV request failed with HTTP status {}",
                                 status);
            }
            return false;
        }
        response_ = std::move(response);
        return true;
    }

    /// 上一次 perform_request 的响应体
    const std::string& last_response() const { return response_; }

    bool test_connection() {
        long status = 0;
        std::map<std::string, std::string> headers;
        headers["Depth"] = "0";
        headers["Content-Type"] = "application/xml";
        return perform_request("PROPFIND", build_url("/"), headers,
                               kPropfindBody, &status);
    }

    /// 隐藏文件与通配符过滤（与 S3Browser 同语义）
    bool apply_filter(const RemoteResource& res, const ListOptions& options) const {
        if (!options.show_hidden && !res.name.empty() && res.name[0] == '.') {
            return false;
        }
        if (!options.filter.empty() && !match_wildcard(res.name, options.filter)) {
            return false;
        }
        return true;
    }

    void sort_resources(std::vector<RemoteResource>& resources,
                        const ListOptions& options) const {
        if (options.sort_by == "name") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    return options.sort_desc ? a.name > b.name : a.name < b.name;
                });
        } else if (options.sort_by == "size") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    return options.sort_desc ? a.size > b.size : a.size < b.size;
                });
        } else if (options.sort_by == "modified_time") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    return options.sort_desc ? a.modified_time > b.modified_time
                                             : a.modified_time < b.modified_time;
                });
        }
    }

    bool match_wildcard(const std::string& str, const std::string& pattern) const {
        if (pattern == "*") return true;

        const size_t pos = pattern.find('*');
        if (pos == std::string::npos) {
            return str == pattern;
        }

        const std::string prefix = pattern.substr(0, pos);
        const std::string suffix = pattern.substr(pos + 1);

        return str.length() >= prefix.length() + suffix.length() &&
               str.substr(0, prefix.length()) == prefix &&
               str.substr(str.length() - suffix.length()) == suffix;
    }

    CURL* curl_;
    WebDavConfig config_;
    WebDavUrl url_;
    std::string current_path_;
    std::string response_;

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb,
                                std::string* userp) {
        userp->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }
};

// WebDavBrowser 公共接口实现
WebDavBrowser::WebDavBrowser() : p_impl_(std::make_unique<Impl>()) {}

WebDavBrowser::~WebDavBrowser() = default;

std::string WebDavBrowser::get_name() const {
    return "WebDAV";
}

std::vector<std::string> WebDavBrowser::get_supported_protocols() const {
    return {"webdav", "dav", "davs", "webdavs"};
}

bool WebDavBrowser::can_handle(const std::string& url) const {
    return url.find("webdav://") == 0 || url.find("dav://") == 0 ||
           url.find("davs://") == 0 || url.find("webdavs://") == 0;
}

bool WebDavBrowser::connect(const std::string& url,
                            const std::map<std::string, std::string>& options) {
    p_impl_->url_ = WebDavUrlParser::parse(url);
    if (p_impl_->url_.scheme.empty() || p_impl_->url_.host.empty()) {
        FALCON_LOG_ERROR("Invalid WebDAV URL: {}", url);
        return false;
    }

    auto it = options.find("username");
    if (it != options.end()) {
        p_impl_->config_.username = it->second;
    }
    it = options.find("password");
    if (it != options.end()) {
        p_impl_->config_.password = it->second;
    }
    // URL 内嵌凭据兜底（options 显式传入优先）
    if (p_impl_->config_.username.empty()) {
        p_impl_->config_.username = p_impl_->url_.username;
        p_impl_->config_.password = p_impl_->url_.password;
    }

    if (!p_impl_->config_.username.empty()) {
        // 鉴权交由 curl 按 401 挑战协商：Basic（坚果云/ALIST）与
        // Digest（部分 NAS）均覆盖，无需预先知道服务器方案
        curl_easy_setopt(p_impl_->curl_, CURLOPT_HTTPAUTH,
                         static_cast<long>(CURLAUTH_ANY));
        const std::string userpwd =
            p_impl_->config_.username + ":" + p_impl_->config_.password;
        curl_easy_setopt(p_impl_->curl_, CURLOPT_USERPWD, userpwd.c_str());
    }

    // PROPFIND Depth:0 探活：路径不通/凭据错误在此即失败
    return p_impl_->test_connection();
}

void WebDavBrowser::disconnect() {
    // HTTP 无状态，句柄随 Impl 生命周期回收
}

std::vector<RemoteResource> WebDavBrowser::list_directory(
    const std::string& path,
    const ListOptions& options) {

    std::vector<RemoteResource> resources;

    std::map<std::string, std::string> headers;
    headers["Depth"] = "1";
    headers["Content-Type"] = "application/xml";
    if (!p_impl_->perform_request("PROPFIND", p_impl_->build_url(path),
                                  headers, kPropfindBody)) {
        FALCON_LOG_ERROR("Failed to list WebDAV directory");
        return resources;
    }

    std::vector<detail::DavEntry> entries;
    if (!detail::parse_dav_propfind_response(p_impl_->last_response(),
                                             &entries)) {
        FALCON_LOG_ERROR("WebDAV list response is not a multistatus");
        return resources;
    }

    const std::string self_path = p_impl_->request_path(path);
    std::vector<RemoteResource> sub_dirs;
    for (auto& entry : entries) {
        // Depth:1 应答含自条目（href == 请求路径）——剔除
        std::string entry_path = entry.path;
        while (!entry_path.empty() && entry_path.back() == '/') {
            entry_path.pop_back();
        }
        if (entry_path == self_path) {
            continue;
        }

        RemoteResource res;
        res.path = p_impl_->strip_base(entry_path);
        res.name = entry.name.empty()
                       ? res.path.substr(res.path.find_last_of('/') + 1)
                       : entry.name;
        if (entry.is_dir) {
            res.type = ResourceType::Directory;
        } else {
            res.type = ResourceType::File;
            res.size = entry.size;
            res.mime_type = entry.content_type;
        }
        res.modified_time = entry.last_modified;
        res.etag = entry.etag;

        if (res.type == ResourceType::Directory && options.recursive) {
            // 递归模式下目录只用于下钻（与 S3Browser 前缀语义一致）
            sub_dirs.push_back(std::move(res));
            continue;
        }
        if (p_impl_->apply_filter(res, options)) {
            resources.push_back(std::move(res));
        }
    }

    if (options.recursive) {
        for (const auto& dir : sub_dirs) {
            std::vector<RemoteResource> sub_resources =
                list_directory(dir.path, options);
            resources.insert(resources.end(),
                             std::make_move_iterator(sub_resources.begin()),
                             std::make_move_iterator(sub_resources.end()));
        }
    }

    p_impl_->sort_resources(resources, options);
    return resources;
}

RemoteResource WebDavBrowser::get_resource_info(const std::string& path) {
    RemoteResource info;

    std::map<std::string, std::string> headers;
    headers["Depth"] = "0";
    headers["Content-Type"] = "application/xml";
    if (!p_impl_->perform_request("PROPFIND", p_impl_->build_url(path),
                                  headers, kPropfindBody)) {
        return info;
    }

    std::vector<detail::DavEntry> entries;
    if (!detail::parse_dav_propfind_response(p_impl_->last_response(),
                                             &entries) ||
        entries.empty()) {
        return info;
    }

    const detail::DavEntry& entry = entries.front();
    info.path = p_impl_->strip_base(path);
    info.name = info.path.substr(info.path.find_last_of('/') + 1);
    info.type = entry.is_dir ? ResourceType::Directory : ResourceType::File;
    if (!entry.is_dir) {
        info.size = entry.size;
        info.mime_type = entry.content_type;
    }
    info.modified_time = entry.last_modified;
    info.etag = entry.etag;
    return info;
}

bool WebDavBrowser::create_directory(const std::string& path, bool recursive) {
    const std::string normalized = p_impl_->normalize_virtual(path);
    if (normalized == "/") {
        return false;  // 根目录无需创建也不可创建
    }

    if (recursive) {
        // 逐级 MKCOL：已存在的中间级回 405（Method Not Allowed），放行
        size_t pos = 1;  // 跳过前导 '/'
        while (true) {
            const size_t slash = normalized.find('/', pos);
            const std::string prefix = (slash == std::string::npos)
                                           ? normalized
                                           : normalized.substr(0, slash);
            long status = 0;
            p_impl_->perform_request("MKCOL", p_impl_->build_url(prefix), {},
                                     "", &status);
            if (slash == std::string::npos) {
                return status >= 200 && status < 400;
            }
            pos = slash + 1;
        }
    }

    bool ok = false;
    p_impl_->perform_request("MKCOL", p_impl_->build_url(normalized), {}, "",
                             nullptr, &ok);
    return ok;
}

bool WebDavBrowser::remove(const std::string& path, bool recursive) {
    const std::string normalized = p_impl_->normalize_virtual(path);

    std::map<std::string, std::string> headers;
    if (recursive) {
        // RFC 4918：DELETE 对集合默认已是 Depth infinity，显式声明以
        // 对齐语义；个别服务器会拒绝（Depth 头 403），走逐项删除兜底
        headers["Depth"] = "infinity";
    }
    bool ok = false;
    p_impl_->perform_request("DELETE", p_impl_->build_url(normalized),
                             headers, "", nullptr, &ok);
    if (ok || !recursive) {
        return ok;
    }

    // 兜底：逐项删除（先深后浅），再删目录本身
    ListOptions options;
    options.recursive = true;
    auto resources = list_directory(normalized, options);
    std::sort(resources.begin(), resources.end(),
              [](const RemoteResource& a, const RemoteResource& b) {
                  return a.path.length() > b.path.length();
              });
    bool all_ok = true;
    for (const auto& res : resources) {
        bool child_ok = false;
        p_impl_->perform_request("DELETE", p_impl_->build_url(res.path), {},
                                 "", nullptr, &child_ok);
        all_ok = all_ok && child_ok;
    }
    bool dir_ok = false;
    p_impl_->perform_request("DELETE", p_impl_->build_url(normalized), {},
                             "", nullptr, &dir_ok);
    return all_ok && dir_ok;
}

bool WebDavBrowser::rename(const std::string& old_path,
                           const std::string& new_path) {
    std::map<std::string, std::string> headers;
    headers["Destination"] = p_impl_->build_url(new_path);
    headers["Overwrite"] = "T";
    bool ok = false;
    p_impl_->perform_request("MOVE", p_impl_->build_url(old_path), headers,
                             "", nullptr, &ok);
    return ok;
}

bool WebDavBrowser::copy(const std::string& source_path,
                         const std::string& dest_path) {
    std::map<std::string, std::string> headers;
    headers["Destination"] = p_impl_->build_url(dest_path);
    headers["Overwrite"] = "T";
    headers["Depth"] = "infinity";  // 集合复制需要递归语义
    bool ok = false;
    p_impl_->perform_request("COPY", p_impl_->build_url(source_path),
                             headers, "", nullptr, &ok);
    return ok;
}

bool WebDavBrowser::exists(const std::string& path) {
    RemoteResource info = get_resource_info(path);
    return !info.name.empty();
}

std::string WebDavBrowser::get_current_directory() {
    return p_impl_->current_path_;
}

bool WebDavBrowser::change_directory(const std::string& path) {
    p_impl_->current_path_ = p_impl_->normalize_virtual(path);
    return true;
}

std::string WebDavBrowser::get_root_path() {
    return "/";
}

std::map<std::string, uint64_t> WebDavBrowser::get_quota_info() {
    std::map<std::string, uint64_t> quota;

    // RFC 4331（quota-used/available-bytes）：仅部分服务器实现（ALIST
    // 支持）；不支持的应答缺字段 → 空 map，与接口约定一致
    std::map<std::string, std::string> headers;
    headers["Depth"] = "0";
    headers["Content-Type"] = "application/xml";
    if (!p_impl_->perform_request("PROPFIND", p_impl_->build_url("/"),
                                  headers, kPropfindAllpropBody)) {
        return quota;
    }

    uint64_t used = 0;
    uint64_t available = 0;
    if (detail::parse_dav_quota(p_impl_->last_response(), &used, &available)) {
        quota["used"] = used;
        quota["available"] = available;
        quota["total"] = used + available;
    }
    return quota;
}

} // namespace falcon
