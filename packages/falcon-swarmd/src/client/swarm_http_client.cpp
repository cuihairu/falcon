// ============================================================================
// SwarmHttpClient 实现（见 swarm_http_client.hpp 头注释）
// ============================================================================

#include "swarm_http_client.hpp"

#include <curl/curl.h>

#include <atomic>
#include <mutex>

namespace falcon::swarm {

namespace {

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb,
                     void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

/// 进程级 Winsock 初始化（Windows 上 curl 的 socket 层依赖 WSAStartup；
/// 与 DownloadEngineV2 的 call_once 同姿态——进程生命周期不清理）。
std::once_flag& curl_global_flag() {
    static std::once_flag flag;
    return flag;
}

}  // namespace

SwarmHttpClient::SwarmHttpClient(Options opt) : opt_(std::move(opt)) {}

SwarmHttpReply SwarmHttpClient::post_json(const std::string& rpc_path,
                                          const std::string& json_body) {
    SwarmHttpReply reply;
    std::call_once(curl_global_flag(), [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        reply.transport_error = "curl_easy_init failed";
        return reply;
    }

    const std::string url = "http://" + opt_.host + ":" +
                            std::to_string(opt_.port) + rpc_path;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(json_body.size()));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, opt_.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply.body);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!opt_.bearer_token.empty()) {
        headers = curl_slist_append(
            headers, ("Authorization: Bearer " + opt_.bearer_token).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);

    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.http_status);
        reply.transport_ok = true;
    } else {
        reply.transport_error = std::string("curl: ") +
                                curl_easy_strerror(rc);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return reply;
}

SwarmHttpReply SwarmHttpClient::get_health() {
    SwarmHttpReply reply;
    std::call_once(curl_global_flag(), [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        reply.transport_error = "curl_easy_init failed";
        return reply;
    }

    const std::string url = "http://" + opt_.host + ":" +
                            std::to_string(opt_.port) + "/v1/health";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, opt_.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply.body);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.http_status);
        reply.transport_ok = true;
    } else {
        reply.transport_error = std::string("curl: ") + curl_easy_strerror(rc);
    }

    curl_easy_cleanup(curl);
    return reply;
}

}  // namespace falcon::swarm
