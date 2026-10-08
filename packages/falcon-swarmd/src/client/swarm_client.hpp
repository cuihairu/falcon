#pragma once

// ============================================================================
// SwarmClient：节点侧 swarm 会话客户端（阶段 0：注册/心跳/查询/退订）
//
// 生命周期 = start() 起 WS 收订 + 两步注册 + 心跳线程；stop() 退订收口。
// 心跳线程对 -32003（未知/过期 session）自动重注册自愈——Rendezvous 侧心跳
// 超时摘除（sweep）后节点侧无感恢复。
//
// 关键语义：
//   - 会话数据（session/node_id/计数）由 mutex 保护（心跳线程与查询
//     调用方并发）；观测访问器线程安全。
//   - detach() = 停心跳但保持注册（模拟进程崩溃：Rendezvous 心跳超时后
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
#include <vector>

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

/// announce 公告条目（§16.3）。kind = "file"（本机源：advertise 注册态
/// 承载，name/size 资源级元数据）或 "mirror"（url 源：etag/last_modified/
/// accept_ranges 归并后写胜出）。可选字段空/缺省不上线。
struct AnnounceResource {
    std::string kind;              // "file" | "mirror"
    std::string sha256;            // 64 位小写 hex（非此形态服务器计 rejected）
    std::string name;              // file：资源名（空 = 不上送）
    bool has_size = false;
    uint64_t size = 0;             // file：资源尺寸（has_size 才上送）
    std::string url;               // mirror：必填（非 http(s) 计 rejected）
    std::string etag;              // mirror：空 = 不上送
    std::string last_modified;     // mirror：空 = 不上送
    bool accept_ranges = false;    // mirror：false = 不上送（与服务器缺省同义）
    std::chrono::seconds ttl_s{0}; // <=0 = 不上送（服务器缺省 86400，钳
                                   // [3600, 604800]）
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

    /// 停心跳线程但保持注册态（模拟崩溃；Rendezvous 心跳超时后摘除）。
    void detach();

    /// 查询资源来源（阶段 0 空表：合法 session → sha256 回显 + 空数组）。
    SwarmError query(const std::string& sha256_hex, nlohmann::json* result);

    /// 公告资源（§16.2/§16.3）：批量 file/mirror 条目，签名 nonce 槽位 =
    /// 当前 session。成功（err.ok）时 result = {accepted, rejected,
    /// expires_at}——非 0 的 rejected 交调用方解读，不是错误；网络/协议
    /// 失败走 SwarmError。会话读取与签名/发送之间心跳线程可能重注册换
    /// session（→ -32003），调用方按可重试错误处理。
    SwarmError announce(const std::vector<AnnounceResource>& resources,
                        nlohmann::json* result);

    /// 摘除名下公告（按哈希整批）。成功时 result = {removed, unknown}——
    /// 表中无该哈希计 unknown；表中有但名下无源两边都不计（§16.2）。
    SwarmError retract(const std::vector<std::string>& sha256s,
                       nlohmann::json* result);

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
