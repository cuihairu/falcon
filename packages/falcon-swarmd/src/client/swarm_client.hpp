#pragma once

// ============================================================================
// SwarmClient：节点侧 swarm 会话客户端（阶段 0：注册/心跳/查询/退订）
//
// 生命周期 = start() 起 WS 收订 + 两步注册 + 心跳线程；stop() 退订收口。
// 心跳线程对 -32003（未知/过期 session）自动重注册自愈——server 侧心跳
// 超时摘除（sweep）后节点侧无感恢复。
//
// 关键语义：
//   - 会话数据（session/node_id/计数）由 mutex 保护（心跳线程与查询
//     调用方并发）；观测访问器线程安全。
//   - detach() = 停心跳但保持注册（模拟进程崩溃：server 心跳超时后
//     摘除并广播 onPeerLeft——e2e 心跳超时用例的客户端侧入口）。
//   - 析构 = detach + unsubscribe 尽力（一次性 HTTP，失败静默）+ WS 收订
//     停机，绝不阻塞退出路径（daemon RAII 先例）。
// ============================================================================

#include "swarm_http_client.hpp"
#include "swarm_key_store.hpp"
#include "swarm_ws_subscriber.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace falcon::swarm {

struct SwarmClientConfig {
    std::string host = "127.0.0.1";
    int port = 0;
    std::string server_token;      // 非空才带 Bearer
    std::string group_token;       // 非空才进注册 params（服务器侧校验）
    std::string agent = "falcon-swarm";
    std::string advertise_addr;    // 非空才带 advertise{addr, direct}
    bool advertise_direct = true;
    std::string key_file;          // 非空 = load-or-create PEM；空 = 内存临时身份
    std::chrono::milliseconds rpc_timeout_ms{5000};
    std::chrono::milliseconds reconnect_delay{2000};
};

struct SwarmError {
    int code = 0;                  // 0 = 成功；其余为 JSON-RPC error.code
    std::string message;
    long http_status = 0;

    bool ok() const { return code == 0; }
};

class SwarmClient {
public:
    SwarmClient(SwarmClientConfig cfg, SwarmKeyMaterial key);
    ~SwarmClient();

    SwarmClient(const SwarmClient&) = delete;
    SwarmClient& operator=(const SwarmClient&) = delete;

    /// 启动 WS 收订并完成两步注册（注册失败返回 false + error）。
    bool start(std::string* error = nullptr);

    /// 退订 + 停机（幂等）。
    void stop();

    /// 停心跳线程但保持注册态（模拟崩溃；server 心跳超时后摘除）。
    void detach();

    /// 查询资源来源（阶段 0 空表：合法 session → sha256 回显 + 空数组）。
    SwarmError query(const std::string& sha256_hex, nlohmann::json* result);

    // ---- 观测（线程安全）---------------------------------------------
    std::string node_id() const;
    std::string session() const;
    uint64_t heartbeat_count() const { return heartbeat_count_.load(); }
    uint64_t reregister_count() const { return reregister_count_.load(); }
    uint64_t ws_connected_count() const {
        return subscriber_ ? subscriber_->connected_count() : 0;
    }

private:
    /// 单次 JSON-RPC 调用（envelope 组装 + 解包；网络失败 → code=-1）。
    SwarmError rpc_call(const std::string& method, const nlohmann::json& params,
                        nlohmann::json* result);
    /// 两步注册握手；成功更新 session_ 与心跳周期。返回 SwarmError。
    SwarmError do_register();
    /// 心跳线程体：wait_for 周期 → 心跳；-32003 → 重注册自愈。
    void heartbeat_loop();

    SwarmClientConfig cfg_;
    SwarmKeyMaterial key_;
    SwarmHttpClient http_;
    std::unique_ptr<SwarmWsSubscriber> subscriber_;

    mutable std::mutex session_mutex_;  // session() const 读锁
    std::string session_;
    long heartbeat_interval_ms_ = 30000;  // 服务器下发值（ms），clamp 后生效

    std::thread heartbeat_thread_;
    std::mutex hb_mutex_;
    std::condition_variable hb_cv_;
    bool hb_stop_ = false;
    bool heartbeat_active_ = false;  // detach 后置 false：线程退出但会话保留

    std::atomic<uint64_t> heartbeat_count_{0};
    std::atomic<uint64_t> reregister_count_{0};
};

}  // namespace falcon::swarm
