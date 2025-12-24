/**
 * @file http_request.hpp
 * @brief HTTP 请求类占位实现
 * @author Falcon Team
 * @date 2025-12-24
 */

#pragma once

#include <falcon/types.hpp>
#include <string>
#include <map>

namespace falcon {

/**
 * @brief HTTP 请求类（占位实现）
 *
 * TODO: 完整实现 HTTP 请求构建
 */
class HttpRequest {
public:
    HttpRequest() = default;
    ~HttpRequest() = default;

    const std::string& url() const { return url_; }
    const std::string& method() const { return method_; }
    const std::map<std::string, std::string>& headers() const { return headers_; }

    void set_url(const std::string& url) { url_ = url; }
    void set_method(const std::string& method) { method_ = method; }
    void set_header(const std::string& key, const std::string& value) {
        headers_[key] = value;
    }

    std::string to_string() const {
        // TODO: 构建完整的 HTTP 请求字符串
        return method_ + " " + url_ + " HTTP/1.1\r\n";
    }

private:
    std::string url_;
    std::string method_ = "GET";
    std::map<std::string, std::string> headers_;
};

/**
 * @brief HTTP 响应类（占位实现）
 *
 * TODO: 完整实现 HTTP 响应解析
 */
class HttpResponse {
public:
    HttpResponse() = default;
    ~HttpResponse() = default;

    int status_code() const { return status_code_; }
    const std::string& status_text() const { return status_text_; }
    const std::map<std::string, std::string>& headers() const { return headers_; }

    void set_status_code(int code) { status_code_ = code; }
    void set_status_text(const std::string& text) { status_text_ = text; }
    void add_header(const std::string& key, const std::string& value) {
        headers_[key] = value;
    }

    const std::string& get_header(const std::string& key) const {
        static const std::string empty;
        auto it = headers_.find(key);
        return it != headers_.end() ? it->second : empty;
    }

private:
    int status_code_ = 0;
    std::string status_text_;
    std::map<std::string, std::string> headers_;
};

} // namespace falcon
