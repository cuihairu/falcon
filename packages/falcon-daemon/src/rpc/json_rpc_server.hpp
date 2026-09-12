#pragma once

#include <falcon/download_engine.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace falcon::daemon {
    class TaskStorage;
}

namespace falcon::daemon::rpc {

class RpcEventBridge;
class WsClientState;

struct JsonRpcServerConfig {
    uint16_t listen_port = 6800;
    std::string secret;
    bool allow_origin_all = false;
    std::string bind_address = "127.0.0.1";
    /// Falcon 扩展通知 falcon.onProgress 的每任务推送节流间隔
    std::chrono::milliseconds progress_push_interval{1000};
};

class JsonRpcServer {
public:
    struct HttpRequest;
    struct HttpResponse;

    explicit JsonRpcServer(falcon::DownloadEngine* engine,
                          JsonRpcServerConfig config,
                          TaskStorage* storage = nullptr);
    ~JsonRpcServer();

    JsonRpcServer(const JsonRpcServer&) = delete;
    JsonRpcServer& operator=(const JsonRpcServer&) = delete;

    bool start();
    void stop();

    uint16_t port() const noexcept { return config_.listen_port; }

    /// 注册进程级停机回调（aria2.forceShutdown / aria2.shutdown 触发）。
    /// 回调在 RPC 工作线程上执行，实现方需能从任意线程安全地请求停机。
    void set_shutdown_handler(std::function<void()> handler);

    /// 向所有 WebSocket 订阅者广播一条 JSON-RPC 通知。
    /// params_json 必须是已序列化的 JSON 数组文本（如 `[{"gid":"..."}]`），
    /// 与 HTTP 端口的 WebSocket 升级订阅者共享（aria2.onDownloadStart 等）。
    void broadcast_notification(const std::string& method,
                                const std::string& params_json);

    /// 当前 WebSocket 订阅者数量（测试与监控用）
    std::size_t websocket_client_count();

    /// 运行时热更新认证配置（SIGHUP 配置重载用），线程安全；
    /// 对后续到达的请求立即生效，已建立的 WebSocket 会话不受影响。
    void update_auth(std::string secret, bool allow_origin_all);

private:
    /// auth 配置的带锁读取（会被配置重载并发更新，禁止直读 config_）
    std::string auth_secret() const;
    bool auth_allow_origin_all() const;

    void accept_loop();
    void handle_connection(int client_fd);

    HttpResponse handle_http_request(const HttpRequest& req);
    HttpResponse handle_jsonrpc(const std::string& body);

    /// GET 请求是否为 WebSocket 升级（Upgrade: websocket + Sec-WebSocket-Key）
    bool is_websocket_upgrade(const HttpRequest& req) const;
    /// 完成 RFC 6455 握手并进入会话循环，直到对端关闭或服务器停止。
    /// 连接 fd 的所有权由本函数接管（返回后由 ScopedFd 关闭）。
    void handle_websocket(int client_fd, const HttpRequest& req);
    /// 向单个已注册的 WebSocket 连接发送一帧（串行化于该连接的写互斥）
    bool ws_send_frame(int client_fd, std::uint8_t opcode,
                       const std::string& payload);

    falcon::DownloadEngine* engine_ = nullptr;
    TaskStorage* storage_ = nullptr;
    JsonRpcServerConfig config_;
    // 认证配置的运行时视图（update_auth 热更目标；启动值来自 config_）
    mutable std::mutex auth_mutex_;
    std::string auth_secret_;
    bool auth_allow_origin_all_ = false;
    std::function<void()> shutdown_handler_;
    std::string session_id_;

    std::atomic<bool> stop_requested_{false};
    std::thread accept_thread_;
    std::mutex worker_threads_mutex_;
    std::vector<std::thread> worker_threads_;

    // WebSocket 订阅者注册表：fd → 每连接写互斥。广播线程与连接线程共享；
    // 持 shared_ptr 使广播快照在连接注销后仍可安全完成发送。
    std::mutex ws_clients_mutex_;
    std::map<int, std::shared_ptr<WsClientState>> ws_clients_;

    // 引擎事件 → 通知广播桥（生命周期与 server 一致；start/stop 时挂接）
    std::unique_ptr<RpcEventBridge> event_bridge_;

    int listen_fd_ = -1;
};

} // namespace falcon::daemon::rpc
