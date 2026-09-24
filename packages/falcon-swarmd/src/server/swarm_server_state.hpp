#pragma once

// ============================================================================
// SwarmServerState：swarmd 服务器内存状态（设计文档 §7.4 存储模型）
//
// 节点表 + 会话表 + 挑战表 + 资源目录表，单把 mutex 保护（圈规模小，
// 进程内 map 足够）。全部公开方法以 steady_clock::time_point 显式入参
// 接收时刻——状态类零时钟依赖，单测可注入虚拟时间推进 TTL/超时。
//
// 通知纪律（锁外派发）：方法在锁内完成状态变更并组装通知，随返回值
// 带出（SwarmReply::notifications），由传输层锁外广播——状态锁内绝不
// 调用网络回调。阶段 0 触发面仅 onPeerJoined/onPeerLeft（onResource*
// 常量已定，触发路径随阶段 1 announce 落地）。
//
// 身份规则（§9.2）：指纹 = sha256_hex(DER(SPKI)) 前 32 字符；注册第
// 一步即校验 node_id == fingerprint(pubkey)——ID 抢注（自报他人指纹
// 配自己公钥）在挑战之前就被指纹自洽性拒绝。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace falcon::swarm {

// 单资源来源：node 源（R1，持有节点）或 url 源（R2，镜像 URL）。
// 阶段 0 无 announce 写入路径，表恒空；结构 + 读取随 query 在位，
// 阶段 1 加写入即通（§8.3 归并语义：同 sha256 多路来源）。
struct SwarmResourceSource {
    bool is_node = true;      // true=node 源，false=url 源
    std::string node_id;      // node 源：持有者指纹
    std::string url;          // url 源：镜像地址
    std::string etag;         // url 源可选
    std::string last_modified;
    bool accept_ranges = false;
};

struct SwarmResource {
    std::string sha256;
    std::string name;         // 可空（hash_only 模式）
    uint64_t size = 0;
    std::vector<SwarmResourceSource> sources;
    std::chrono::steady_clock::time_point expires_at{};
};

struct SwarmPeer {
    std::string node_id;
    std::string pubkey_hex;   // DER(SPKI) 小写 hex，88 字符
    std::string agent;
    bool has_advertise = false;
    std::string advertise_addr;
    bool advertise_direct = false;
    std::string session;      // 该节点当前活跃会话（重注册覆盖）
    std::chrono::steady_clock::time_point last_seen{};
    // last_seen 的墙钟副本：query 结果的 last_seen 字段需要 RFC 3339，
    // 而 steady_clock 与 system_clock 无可移植换算——写路径两处同记。
    std::chrono::system_clock::time_point last_seen_wall{};
};

struct SwarmChallenge {
    std::string node_id;
    std::string pubkey_hex;   // step1 自报公钥（step2 验签用）
    std::string challenge;    // hex(32)，单次有效
    std::string nonce;        // step1 params.nonce（payload 组装）
    std::string params_snapshot;  // step1 canonical_json(params)，step2 绑定校验
    std::chrono::steady_clock::time_point expires_at{};
};

class SwarmServerState {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        std::string group_token;   // 空 = 不校验（测试/裸跑形态）
        std::vector<std::string> blacklist;
        std::chrono::seconds heartbeat_timeout{180};
        std::chrono::seconds challenge_ttl{60};
        std::chrono::seconds heartbeat_interval{60};  // 注册 result 回显
    };

    // 通知：method = falcon.swarm.onXxx，params = object（与 daemon
    // 数组式不同，线协议钉死）。
    struct Notification {
        std::string method;
        nlohmann::json params;
    };

    // 方法执行结果：ok 时 result 有效；失败时 error_code/-message 有效；
    // 通知由调用方锁外派发。
    struct SwarmReply {
        nlohmann::json result;
        int error_code = 0;
        std::string error_message;
        std::vector<Notification> notifications;

        bool ok() const { return error_code == 0; }
    };

    explicit SwarmServerState(Config cfg);

    SwarmServerState(const SwarmServerState&) = delete;
    SwarmServerState& operator=(const SwarmServerState&) = delete;

    // ---- 注册两步（§8.2）--------------------------------------------------
    // step1：黑名单/指纹自洽/group_token 三道门 → 记录挑战 → 挑战值。
    // 拒绝时 error_code ∈ {-32004, -32005}，绝不发挑战。
    SwarmReply start_register(const std::string& node_id,
                              const std::string& pubkey_hex,
                              const std::string& nonce,
                              const std::string& group_token,
                              const std::string& canonical_params,
                              Clock::time_point now);

    // step2：challenge_sig 验签。canonical_params_no_sig = step2 params
    // 去 challenge_sig 后的 canonical 序列化——与 step1 快照逐字节比对
    // （绑定 nonce/node_id，防参数偷换）。成功签发 session 并 upsert
    // 节点；挑战无论成败单次消耗。
    SwarmReply complete_register(const std::string& node_id,
                                 const std::string& canonical_params_no_sig,
                                 const std::string& challenge_sig,
                                 Clock::time_point now);

    // ---- 心跳续租（§7.3）--------------------------------------------------
    // session 无效/过期 → -32003；成功刷新 TTL 与 last_seen。
    SwarmReply heartbeat(const std::string& session, Clock::time_point now);

    // ---- 查询（§8.4）------------------------------------------------------
    // session 无效 → -32003；命中返回 {sha256,name,size,sources:[...]}，
    // 未命中返回 {sha256, sources:[]}（阶段 0 表恒空 = 恒未命中分支）。
    SwarmReply query(const std::string& session, const std::string& sha256,
                     Clock::time_point now);

    // ---- 优雅注销（§7.3）--------------------------------------------------
    // 摘节点 + 会话 → onPeerLeft；session 无效 → -32003。
    SwarmReply unsubscribe(const std::string& session, Clock::time_point now);

    // announce/retract 阶段 0 不实现：方法未注册，分发层回 -32601。

    // ---- 周期清扫（心跳超时摘除）------------------------------------------
    // 摘过期挑战（无通知）、过期资源（无通知，阶段 0 裁决）、过期会话
    // （摘节点 → onPeerLeft 通知）。
    std::vector<Notification> sweep(Clock::time_point now);

    // ---- 观测（健康面/测试）-----------------------------------------------
    std::size_t peer_count();
    bool has_session(const std::string& session);

private:
    // 锁内助手（调用方持锁）
    void erase_peer_locked(const std::string& node_id,
                           std::vector<Notification>& out);

    Config cfg_;
    std::unordered_set<std::string> blacklist_;

    mutable std::mutex mutex_;
    std::map<std::string, SwarmPeer> peers_;          // node_id → peer
    std::map<std::string, std::string> sessions_;     // session → node_id
    // 会话到期时刻（session → expires_at）；与 peers_ 的 last_seen 同步续租
    std::map<std::string, Clock::time_point> session_expiry_;
    std::map<std::string, SwarmChallenge> challenges_;  // node_id → challenge
    std::map<std::string, SwarmResource> resources_;    // sha256 → resource
};

}  // namespace falcon::swarm
