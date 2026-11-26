/**
 * @file host_api.cpp
 * @brief falcon_host_api 的协议层实现
 *
 * 边界内侧可以用任意 C++ / curl；对外只暴露 POD 与 C 函数指针。
 * 跨 so 返回的内存全部由 host->allocate 分配，第三方插件用
 * host->deallocate 释放，避免 CRT / 堆不匹配。
 */
#include <falcon/protocols/host_api.hpp>

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace falcon {
namespace protocols {
namespace {

void* host_allocate(void*, size_t bytes) {
    return std::malloc(bytes ? bytes : 1);
}

void host_deallocate(void*, void* ptr) {
    std::free(ptr);
}

/** 把 falcon_str 复制成宿主自己分配的字符串，返回给插件。 */
falcon_str dup_for_plugin(void* host_data, const falcon_host_api* api,
                          const std::string& value) {
    falcon_str out{};
    if (value.empty()) return out;
    char* buffer = static_cast<char*>(api->allocate(host_data, value.size()));
    if (!buffer) return out;
    std::memcpy(buffer, value.data(), value.size());
    out.data = buffer;
    out.size = static_cast<uint32_t>(value.size());
    return out;
}

struct ResponseBuffer {
    std::string data;
};

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    static_cast<ResponseBuffer*>(userdata)->data.append(ptr, bytes);
    return bytes;
}

std::string to_string(falcon_str value) {
    return (value.data && value.size)
               ? std::string(value.data, value.size)
               : std::string();
}

curl_slist* build_headers(const falcon_http_header* headers, uint32_t count) {
    curl_slist* list = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const std::string header =
            to_string(headers[i].name) + ": " + to_string(headers[i].value);
        list = curl_slist_append(list, header.c_str());
    }
    return list;
}

void do_request(void* host_data, const falcon_host_api* api,
                falcon_str url, falcon_str body, bool is_post,
                const falcon_http_header* headers, uint32_t header_count,
                int32_t timeout_seconds, falcon_http_response* out) {
    if (!out) return;
    *out = falcon_http_response{};

    CURL* curl = curl_easy_init();
    if (!curl) {
        out->error = dup_for_plugin(host_data, api, "curl_easy_init failed");
        return;
    }

    const std::string url_text = to_string(url);
    const std::string body_text = to_string(body);
    ResponseBuffer buffer;
    curl_slist* header_list = build_headers(headers, header_count);

    curl_easy_setopt(curl, CURLOPT_URL, url_text.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     timeout_seconds > 0 ? timeout_seconds : 30);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }
    if (is_post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_text.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body_text.size()));
    }

    const CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out->status_code = static_cast<int32_t>(status);
        out->body = dup_for_plugin(host_data, api, buffer.data);
    } else {
        const char* message = curl_easy_strerror(result);
        out->error = dup_for_plugin(
            host_data, api, message ? message : "HTTP request failed");
    }

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
}

void host_http_get(void* host_data, falcon_str url,
                   const falcon_http_header* headers, uint32_t header_count,
                   int32_t timeout_seconds, falcon_http_response* out) {
    const auto* api = host_api();
    do_request(host_data, api, url, falcon_str{}, false, headers, header_count,
               timeout_seconds, out);
}

void host_http_post(void* host_data, falcon_str url, falcon_str body,
                    const falcon_http_header* headers, uint32_t header_count,
                    int32_t timeout_seconds, falcon_http_response* out) {
    const auto* api = host_api();
    do_request(host_data, api, url, body, true, headers, header_count,
               timeout_seconds, out);
}

void host_log(void*, falcon_log_level level, falcon_str message) {
    const char* tag = "info";
    switch (level) {
        case FALCON_LOG_DEBUG: tag = "debug"; break;
        case FALCON_LOG_INFO:  tag = "info";  break;
        case FALCON_LOG_WARN:  tag = "warn";  break;
        case FALCON_LOG_ERROR: tag = "error"; break;
    }
    const std::string text = to_string(message);
    std::fprintf(stderr, "[falcon:%s] %.*s\n", tag,
                 static_cast<int>(text.size()), text.data());
}

const void* host_extension(void*, falcon_str) {
    return nullptr; // 当前无宿主扩展；新增能力走 extension id 查询
}

} // namespace

const falcon_host_api* host_api() {
    static const falcon_host_api api = [] {
        falcon_host_api value{};
        value.abi_version = FALCON_PLUGIN_ABI_VERSION;
        value.host_data = nullptr;
        value.allocate = &host_allocate;
        value.deallocate = &host_deallocate;
        value.http_get = &host_http_get;
        value.http_post = &host_http_post;
        value.log = &host_log;
        value.extension = &host_extension;
        return value;
    }();
    return &api;
}

} // namespace protocols
} // namespace falcon

extern "C" const falcon_host_api* falcon_protocols_host_api(void) {
    return falcon::protocols::host_api();
}
