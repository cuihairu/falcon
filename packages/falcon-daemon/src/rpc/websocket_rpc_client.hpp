/**
 * @file websocket_rpc_client.hpp
 * @brief WebSocket JSON-RPC 2.0 客户端：单连接承载请求/响应与服务器通知
 * @author Falcon Team
 * @date 2026-09-12
 *
 * 与 JsonRpcClient（HTTP，每请求一个连接）不同：本客户端与 daemon 维持
 * 一条 WebSocket 长连接（ws://host:port/jsonrpc，与 HTTP JSON-RPC 同端点），
 * 请求/响应在同一连接上按 id 匹配，服务器通知（aria2.onDownloadStart、
 * falcon.onProgress 等）经 notification handler 推送给调用方——桌面端据此
 * 把快照轮询升级为事件驱动。
 */

#pragma once

#include "rpc/json_rpc_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace falcon::daemon::rpc {

class WebSocketRpcClient {
public:
    /// 服务器通知回调。method/params_json 为通知方法名与已序列化的
    /// params JSON 数组文本。
    ///
    /// 线程约定：在内部读线程调用，与 set_notification_handler 互斥
    /// （setter 返回后保证不再有在途调用），handler 内可安全调用 call()。
    using NotificationHandler = std::function<void(
        const std::string& method, const std::string& params_json)>;

    explicit WebSocketRpcClient(JsonRpcClientConfig config);
    ~WebSocketRpcClient();

    WebSocketRpcClient(const WebSocketRpcClient&) = delete;
    WebSocketRpcClient& operator=(const WebSocketRpcClient&) = delete;

    /// 重定向端点（断开当前连接；下次 call 自动按新端点重连）
    void set_url(const std::string& url);

    /// 建立 TCP 连接并完成 RFC 6455 握手；已连接时直接返回 true
    bool connect();

    /// 关闭连接（shutdown 唤醒读线程并 join；幂等）
    void disconnect();

    bool is_connected() const { return connected_.load(); }

    void set_notification_handler(NotificationHandler handler);

    /// 同步调用。语义与 JsonRpcClient::call 一致：自动前置 token、
    /// 错误码约定相同（-32000 为传输层失败）。未连接时先尝试重连。
    std::optional<nlohmann::json> call(const std::string& method,
                                       nlohmann::json params = nlohmann::json::array(),
                                       JsonRpcError* err = nullptr);

    // ---- 便捷封装（与 JsonRpcClient 平行） ----
    std::optional<std::string> add_uri(const std::vector<std::string>& uris,
                                       const nlohmann::json& options = {},
                                       JsonRpcError* err = nullptr);
    std::optional<std::string> pause(const std::string& gid, JsonRpcError* err = nullptr);
    std::optional<std::string> unpause(const std::string& gid, JsonRpcError* err = nullptr);
    std::optional<std::string> remove(const std::string& gid, JsonRpcError* err = nullptr);
    /// priority 取 falcon::TaskPriority 数值（0=Low 1=Normal 2=High 3=Critical）
    std::optional<std::string> change_priority(const std::string& gid, int priority,
                                               JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> tell_status(const std::string& gid,
                                              JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> tell_active(JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> tell_waiting(JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> tell_stopped(JsonRpcError* err = nullptr);
    std::optional<nlohmann::json> get_global_stat(JsonRpcError* err = nullptr);
    /// options 为 {"aria2 键": "字符串值"} 对象
    bool change_global_option(const nlohmann::json& options, JsonRpcError* err = nullptr);
    bool save_session(JsonRpcError* err = nullptr);
    bool purge_download_result(JsonRpcError* err = nullptr);
    bool remove_download_result(const std::string& gid, JsonRpcError* err = nullptr);
    /// 请求 daemon 正常停机（排水 + 落库），等价 aria2.forceShutdown
    bool shutdown(JsonRpcError* err = nullptr);

private:
    struct Endpoint {
        std::string host;
        std::string port;
        std::string path;
    };

    /// 解析 ws://host:port/path（http/https 同义映射；wss 明文不支持）
    static Endpoint parse_url(const std::string& url);

    /// 请求/响应等待槽（call 与读线程共享，各持一把互斥）
    struct PendingCall {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        nlohmann::json response;  // 完整响应对象（含 result 或 error）
    };

    void join_reader();
    void fail_pending(int code, const std::string& message);
    /// 会话读线程主体：recv → 帧解析 → 响应/通知分发，退出时清理
    void session_loop(int fd, std::string leftover);
    void dispatch_message(const std::string& payload);
    /// 发送一帧（串行化于 write_mutex_）；失败时不做清理（由读线程统一收尾）
    bool send_frame(std::uint8_t opcode, const std::string& payload);

    JsonRpcClientConfig config_;

    // endpoint_ 与连接生命周期由 connect_mutex_ 串行化（connect/disconnect/
    // set_url/reader 退出）；fd_ 用原子量供 send/shutdown 路径无锁读取
    std::mutex connect_mutex_;
    Endpoint endpoint_;
    std::atomic<int> fd_{-1};
    std::thread reader_;

    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> next_id_{1};

    std::mutex write_mutex_;  // 同一连接上帧发送串行化（call 并发 + reader 回 pong）

    std::mutex pending_mutex_;
    std::map<std::uint64_t, std::shared_ptr<PendingCall>> pending_;

    std::mutex handler_mutex_;
    NotificationHandler handler_;
};

} // namespace falcon::daemon::rpc
