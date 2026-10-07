#include "mcp_curl_post.hpp"

#include <curl/curl.h>

#include <cctype>
#include <mutex>
#include <string>

namespace falcon::daemon::mcpstdio {

namespace {

void ensure_curl_global() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

size_t write_body_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    body->append(ptr, bytes);
    return bytes;
}

size_t capture_session_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* session = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    const std::string line(ptr, bytes);
    const auto colon = line.find(':');
    if (colon != std::string::npos &&
        to_lower(trim(line.substr(0, colon))) == "mcp-session-id") {
        *session = trim(line.substr(colon + 1));
    }
    return bytes;
}

std::string normalize_base(std::string url) {
    while (url.size() > 1 && url.back() == '/') url.pop_back();
    return url;
}

} // namespace

HttpPostFn make_curl_http_post(const McpStdioConfig& config) {
    ensure_curl_global();
    const std::string base = normalize_base(config.rpc_url);
    const int timeout_seconds = config.timeout_seconds;

    return [base, timeout_seconds](const std::string& path,
                                   const std::vector<HttpHeader>& headers,
                                   const std::string& body,
                                   const std::string& http_method) -> HttpResult {
        CURL* curl = curl_easy_init();
        if (!curl) return {};  // status 0 = 传输失败，桥层合成错误信封

        HttpResult out;
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // 多线程进程安全
        curl_easy_setopt(curl, CURLOPT_URL, (base + path).c_str());
        if (timeout_seconds > 0)
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));

        // DELETE 走 CUSTOMREQUEST 且不带 POSTFIELDS（逐请求句柄，无
        // 复用残留）；POST 默认方法 + 请求体。
        const bool is_delete = http_method == "DELETE";
        if (is_delete) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

        curl_slist* head_list = nullptr;
        for (const auto& h : headers) {
            const std::string line = h.first + ": " + h.second;
            head_list = curl_slist_append(head_list, line.c_str());
        }
        if (head_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, head_list);

        if (!is_delete) {
            // CURLOPT_POSTFIELDS 不持有数据——body 生存期覆盖 perform
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(body.size()));
        }

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_session_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.session_id);

        const CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) {
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            out.status = static_cast<int>(status);
        }

        if (head_list) curl_slist_free_all(head_list);
        curl_easy_cleanup(curl);
        return out;
    };
}

} // namespace falcon::daemon::mcpstdio
