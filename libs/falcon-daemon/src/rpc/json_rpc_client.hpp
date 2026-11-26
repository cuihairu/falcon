#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace falcon::daemon::rpc {

/// aria2 兼容 JSON-RPC 客户端配置
struct JsonRpcClientConfig {
    std::string url = "http://127.0.0.1:6800/jsonrpc";
    /// aria2 风格 token；为空则调用不带鉴权参数
    std::string secret;
    long timeout_seconds = 30;
};

/// JSON-RPC 调用错误。code 语义与 daemon 服务端一致：
/// 0 = 成功；负数 = JSON-RPC 协议/传输错误（-32000 为传输层失败，
/// 其余透传服务端）；正数 = aria2 业务错误（1/2 等）。
struct JsonRpcError {
    int code = 0;
    std::string message;

    bool is_error() const noexcept { return code != 0; }
};

/// aria2 兼容 JSON-RPC 客户端（同步请求，libcurl 实现）。
/// 每次调用独立建立连接，可在任意线程调用；桌面端用线程池/QtConcurrent
/// 将其异步化。
class JsonRpcClient {
public:
    explicit JsonRpcClient(JsonRpcClientConfig config);
    ~JsonRpcClient();

    JsonRpcClient(const JsonRpcClient&) = delete;
    JsonRpcClient& operator=(const JsonRpcClient&) = delete;

    /// 通用调用。params 无需带 token：配置了 secret 时自动前置
    /// "token:<secret>"。成功返回服务端 result；失败返回 nullopt 并填充 err。
    std::optional<nlohmann::json> call(const std::string& method,
                                       nlohmann::json params = nlohmann::json::array(),
                                       JsonRpcError* err = nullptr);

    // ---- 任务控制 ----
    /// 添加并启动下载，返回 gid
    std::optional<std::string> add_uri(const std::vector<std::string>& uris,
                                       const nlohmann::json& options = {},
                                       JsonRpcError* err = nullptr);
    std::optional<std::string> pause(const std::string& gid, JsonRpcError* err = nullptr);
    std::optional<std::string> unpause(const std::string& gid, JsonRpcError* err = nullptr);
    std::optional<std::string> remove(const std::string& gid, JsonRpcError* err = nullptr);
    /// priority 取 falcon::TaskPriority 数值（0=Low 1=Normal 2=High 3=Critical）
    std::optional<std::string> change_priority(const std::string& gid, int priority,
                                               JsonRpcError* err = nullptr);

    // ---- 查询 ----
    std::optional<nlohmann::json> tell_status(const std::string& gid,
                                              JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> tell_active(JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> tell_waiting(JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> tell_stopped(JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> get_global_stat(JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> get_global_option(JsonRpcError* err = nullptr);

    // ---- 全局与会话 ----
    /// options 为 {"aria2 键": "字符串值"} 对象
    bool change_global_option(const nlohmann::json& options, JsonRpcError* err = nullptr);
    bool save_session(JsonRpcError* err = nullptr);
    bool purge_download_result(JsonRpcError* err = nullptr);
    bool remove_download_result(const std::string& gid, JsonRpcError* err = nullptr);
    /// 请求 daemon 正常停机（排水 + 落库），等价 aria2.forceShutdown
    bool shutdown(JsonRpcError* err = nullptr);

private:
    JsonRpcClientConfig config_;
};

} // namespace falcon::daemon::rpc
