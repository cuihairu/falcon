#pragma once

// ============================================================================
// SwarmRpcServer：swarmd 传输层（docs/p2sp_network_design.md §7.2/§7.3）
//
// 独立瘦传输层（与 daemon JsonRpcServer 形制同源、代码零共享）：TCP accept
// 线程 + 每连接 worker + 同端口 WebSocket 升级（ws://host:port/jsonrpc）。
// 准入与 daemon 分叉（设计文档裁决）：HTTP/WS 统一 `Authorization: Bearer
// <server_token>`（缺/错 → HTTP 401 + {"error":{"code":-32001,...}}），
// GET /v1/health 无鉴权；JSON-RPC 层错误全部 HTTP 200（aria2 同形制），
// 限频命中额外 HTTP 429（WS 上无 HTTP 语义，仅 error 信封）。
//
// 分层纪律：本层只做传输与准入——JSON 形状校验与语义在 handlers/state；
// SwarmReply 的通知（随返回值锁内组装）由本层锁外广播到全部 WS 订阅者，
// sweep 线程周期调 state_.sweep() 并广播其通知。本层不解析协议字段。
//
// 停机（daemon 先例）：stop() 先置标志 + 唤醒 sweep，再 shutdown 全部
// WS 会话 fd 唤醒阻塞在 recv 的会话线程，然后关 listen → join accept →
// join workers → join sweep——顺序防跨线程 close 的 fd 复用竞争（会话
// 线程统一注销自己的 fd）。start() 内 POSIX 进程级 SIGPIPE 免疫 +
// Windows Winsock call_once 初始化（CI 红面 1f44064/02671e7 教训直落
// 新基建——服务器被客户端 abort 是常态，SIGPIPE 必须进程级免疫）。
// ============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "swarm_rate_limiter.hpp"
#include "swarm_server_state.hpp"

namespace falcon::swarm {

struct WsClientState;

// 传输层配置（swarm.json "swarm" 节 → main 装配；测试直接构造）。
struct SwarmServerOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;            // 0 = 内核分配临时端口（测试/默认）
    std::string server_token;          // 空 = 不鉴权（测试/裸跑形态）
    std::string server_name = "falcon-swarmd";
    std::chrono::milliseconds sweep_interval{1000};
    std::size_t rate_register_per_min = 5;   // 0 = 不限
    std::size_t rate_query_per_min = 120;    // 0 = 不限
};

class SwarmRpcServer {
public:
    // 请求/响应中间形态（.cpp 解析/组装；回环测试直接发裸 HTTP/WS 帧，
    // 不经这些结构）。headers 键已小写化、值已 trim。
    struct HttpRequest {
        std::string method;
        std::string path;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    struct HttpResponse {
        int status_code = 200;
        std::string status_text = "OK";
        std::string body;
    };

    SwarmRpcServer(SwarmServerOptions options, SwarmServerState& state);
    ~SwarmRpcServer();

    SwarmRpcServer(const SwarmRpcServer&) = delete;
    SwarmRpcServer& operator=(const SwarmRpcServer&) = delete;

    // 起监听 + accept/sweep 线程。失败返回 false（last_error() 有因）。
    // 已在运行时幂等返回 true。
    bool start();

    // 停机排水（幂等）：见类注释顺序。
    void stop();

    std::uint16_t port() const { return port_.load(); }
    bool is_running() const {
        return !stop_requested_.load() && accept_thread_.joinable();
    }
    std::string last_error() const;

private:
    void accept_loop();
    void handle_connection(int client_fd);
    void handle_websocket(int client_fd, const HttpRequest& req);
    // HTTP 路径：health/404/405/Bearer 门后分发 JSON-RPC（peer = 限频键）
    HttpResponse handle_http_request(const HttpRequest& req,
                                     const std::string& peer);
    bool is_websocket_upgrade(const HttpRequest& req) const;

    // HTTP/WS 共用分发：返回应答信封 body；http_status 出参（限频 429，
    // 其余恒 200——JSON-RPC 错误全在 body）。peer = 限频键（对端 IP）。
    std::string handle_jsonrpc(const std::string& payload,
                               const std::string& peer, int* http_status);

    bool bearer_ok(const std::unordered_map<std::string, std::string>& headers) const;
    bool ws_send_frame(int client_fd, std::uint8_t opcode,
                       const std::string& payload);
    void broadcast_notification(const std::string& method,
                                const nlohmann::json& params);
    void sweep_loop();
    std::string peer_ip(int fd) const;
    void set_last_error(std::string message);

    SwarmServerOptions opts_;
    SwarmServerState& state_;

    int listen_fd_ = -1;
    std::atomic<std::uint16_t> port_{0};
    std::atomic<bool> stop_requested_{false};

    std::thread accept_thread_;
    std::thread sweep_thread_;

    mutable std::mutex worker_threads_mutex_;
    std::vector<std::thread> worker_threads_;

    std::mutex ws_clients_mutex_;
    std::map<int, std::shared_ptr<WsClientState>> ws_clients_;

    std::mutex sweep_mutex_;
    std::condition_variable sweep_cv_;

    mutable std::mutex last_error_mutex_;
    std::string last_error_;

    // per-IP 滑动窗口限频（键 = 对端 IP；与 state 的 steady_clock 纪律同源）
    SwarmRateLimiter register_limiter_;
    SwarmRateLimiter query_limiter_;
};

}  // namespace falcon::swarm
