#include "rpc/json_rpc_client.hpp"

#include <falcon/detail/injection.hpp>

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>

namespace falcon::daemon::rpc {

namespace {

/// libcurl 全局初始化（进程级一次；curl_global_init 非线程安全）
void ensure_curl_initialized() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

/// 每个请求一个独立 id（服务端按请求回显，不依赖其唯一性）
std::string make_request_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "falcon-%llx",
                  static_cast<unsigned long long>(rng()));
    return buf;
}

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

/// 封装 nlohmann::json 解析错误为统一错误码
JsonRpcError transport_error(const std::string& message) {
    return JsonRpcError{-32000, message};
}

} // namespace

JsonRpcClient::JsonRpcClient(JsonRpcClientConfig config) : config_(std::move(config)) {
    ensure_curl_initialized();
}

JsonRpcClient::~JsonRpcClient() = default;

std::optional<nlohmann::json> JsonRpcClient::call(const std::string& method,
                                                  nlohmann::json params,
                                                  JsonRpcError* err) {
    const auto fail = [&err](JsonRpcError e) {
        if (err) *err = std::move(e);
        return std::nullopt;
    };

    if (!params.is_array()) {
        params = nlohmann::json::array();
    }
    if (!config_.secret.empty()) {
        params.insert(params.begin(), "token:" + config_.secret);
    }

    nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id", make_request_id()},
        {"method", method},
        {"params", std::move(params)},
    };
    const std::string body = request.dump();

    // 注入命中时短路真实创建，避免已创建句柄在失败路径泄漏
    CURL* curl =
        ::falcon::detail::inject_failure(::falcon::detail::InjectPoint::CurlEasyInit)
            ? nullptr
            : curl_easy_init();
    if (!curl) {
        return fail(transport_error("curl_easy_init failed"));
    }

    std::string response;
    long http_code = 0;
    char curl_error[CURL_ERROR_SIZE] = {};

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, config_.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // 同进程测试场景会指向自签/内网地址，客户端不做 TLS 校验扩展
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::string message = curl_error[0] ? curl_error : curl_easy_strerror(rc);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return fail(transport_error("HTTP request failed: " + message));
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (http_code != 200) {
        return fail(transport_error("HTTP " + std::to_string(http_code)));
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response);
    } catch (const nlohmann::json::exception& e) {
        return fail(JsonRpcError{-32700, std::string("Invalid JSON response: ") + e.what()});
    }
    if (!parsed.is_object()) {
        return fail(JsonRpcError{-32600, "Response is not a JSON object"});
    }

    if (parsed.contains("error")) {
        const auto& e = parsed["error"];
        JsonRpcError rpc_err;
        rpc_err.code = e.value("code", -32000);
        rpc_err.message = e.value("message", "Unknown error");
        return fail(std::move(rpc_err));
    }
    if (!parsed.contains("result")) {
        return fail(JsonRpcError{-32600, "Response has neither result nor error"});
    }

    if (err) *err = JsonRpcError{};
    return std::move(parsed["result"]);
}

// ---------------------------------------------------------------------------
// 便捷封装
// ---------------------------------------------------------------------------

namespace {

/// 从 result（期望字符串 gid）提取
std::optional<std::string> as_gid(std::optional<nlohmann::json> result,
                                  JsonRpcError* err) {
    if (!result) return std::nullopt;
    if (!result->is_string()) {
        if (err) *err = JsonRpcError{-32600, "Expected gid string in response"};
        return std::nullopt;
    }
    return result->get<std::string>();
}

bool expect_ok(std::optional<nlohmann::json> result, JsonRpcError* err) {
    if (!result) return false;
    if (!result->is_string() || result->get<std::string>() != "OK") {
        if (err) *err = JsonRpcError{-32600, "Expected \"OK\" in response"};
        return false;
    }
    return true;
}

} // namespace

std::optional<std::string> JsonRpcClient::add_uri(const std::vector<std::string>& uris,
                                                  const nlohmann::json& options,
                                                  JsonRpcError* err) {
    nlohmann::json params = nlohmann::json::array({uris});
    if (!options.is_null()) params.push_back(options);
    return as_gid(call("aria2.addUri", std::move(params), err), err);
}

std::optional<std::string> JsonRpcClient::pause(const std::string& gid, JsonRpcError* err) {
    return as_gid(call("aria2.pause", nlohmann::json::array({gid}), err), err);
}

std::optional<std::string> JsonRpcClient::unpause(const std::string& gid, JsonRpcError* err) {
    return as_gid(call("aria2.unpause", nlohmann::json::array({gid}), err), err);
}

std::optional<std::string> JsonRpcClient::remove(const std::string& gid, JsonRpcError* err) {
    return as_gid(call("aria2.remove", nlohmann::json::array({gid}), err), err);
}

std::optional<std::string> JsonRpcClient::change_priority(const std::string& gid,
                                                          int priority,
                                                          JsonRpcError* err) {
    return as_gid(call("aria2.changePriority", nlohmann::json::array({gid, priority}), err),
                  err);
}

std::optional<nlohmann::json> JsonRpcClient::tell_status(const std::string& gid,
                                                         JsonRpcError* err) {
    return call("aria2.tellStatus", nlohmann::json::array({gid}), err);
}

std::optional<nlohmann::json> JsonRpcClient::tell_active(JsonRpcError* err) {
    return call("aria2.tellActive", nlohmann::json::array(), err);
}

std::optional<nlohmann::json> JsonRpcClient::tell_waiting(JsonRpcError* err) {
    return call("aria2.tellWaiting", nlohmann::json::array({0, 10000}), err);
}

std::optional<nlohmann::json> JsonRpcClient::tell_stopped(JsonRpcError* err) {
    return call("aria2.tellStopped", nlohmann::json::array({0, 10000}), err);
}

std::optional<nlohmann::json> JsonRpcClient::get_global_stat(JsonRpcError* err) {
    return call("aria2.getGlobalStat", nlohmann::json::array(), err);
}

std::optional<nlohmann::json> JsonRpcClient::get_global_option(JsonRpcError* err) {
    return call("aria2.getGlobalOption", nlohmann::json::array(), err);
}

bool JsonRpcClient::change_global_option(const nlohmann::json& options, JsonRpcError* err) {
    return expect_ok(call("aria2.changeGlobalOption", nlohmann::json::array({options}), err),
                     err);
}

bool JsonRpcClient::save_session(JsonRpcError* err) {
    return expect_ok(call("aria2.saveSession", nlohmann::json::array(), err), err);
}

bool JsonRpcClient::purge_download_result(JsonRpcError* err) {
    return expect_ok(call("aria2.purgeDownloadResult", nlohmann::json::array(), err), err);
}

bool JsonRpcClient::remove_download_result(const std::string& gid, JsonRpcError* err) {
    return expect_ok(call("aria2.removeDownloadResult", nlohmann::json::array({gid}), err),
                     err);
}

bool JsonRpcClient::shutdown(JsonRpcError* err) {
    return expect_ok(call("aria2.forceShutdown", nlohmann::json::array(), err), err);
}

} // namespace falcon::daemon::rpc
