#pragma once

// ============================================================================
// falcon-swarmd 线协议单一事实源（docs/p2sp_network_design.md §7.3/§8）
//
// 方法/通知/错误码/线字段名常量 + canonical JSON 序列化 + 签名 payload
// 组装 + 时间格式助手。client 与 server 共用本头文件，两端对拍以这里
// 为唯一出处（防同一语义两端各写一份漂移）。
//
// 编码定案（§8.1）：
//   params_json     = nlohmann::json 默认键序排序 + 紧凑 dump（canonical_json）
//   signing payload = method + "\n" + nonce + "\n" + sha256_hex(params_json)
//   时间戳          = UTC RFC 3339 秒粒度（"...Z" 收尾）
//   哈希/签名/随机量 = 全部小写 hex（is_hex_string 严格校验小写）
// ============================================================================

#include <chrono>
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "swarm_crypto.hpp"

namespace falcon::swarm {

// ---- 方法名（§7.3 方法表）------------------------------------------------
// challenge 在 §8.2 的线形态是 register 的 result{"status":"challenge"}，
// 不作为独立入站方法分派；此处保留协议表静态事实供文档对照。
inline constexpr char kMethodRegister[] = "falcon.swarm.register";
inline constexpr char kMethodChallenge[] = "falcon.swarm.challenge";
inline constexpr char kMethodHeartbeat[] = "falcon.swarm.heartbeat";
inline constexpr char kMethodAnnounce[] = "falcon.swarm.announce";
inline constexpr char kMethodRetract[] = "falcon.swarm.retract";
inline constexpr char kMethodQuery[] = "falcon.swarm.query";
inline constexpr char kMethodUnsubscribe[] = "falcon.swarm.unsubscribe";

// ---- WS 通知名（§7.3 通知表；params 为 object，与 daemon 数组式不同）----
inline constexpr char kNotifyResourceAdded[] = "falcon.swarm.onResourceAdded";
inline constexpr char kNotifyResourceExpired[] =
    "falcon.swarm.onResourceExpired";
inline constexpr char kNotifyPeerJoined[] = "falcon.swarm.onPeerJoined";
inline constexpr char kNotifyPeerLeft[] = "falcon.swarm.onPeerLeft";

// ---- 应用层错误码（-32001..-32005；与 JSON-RPC 标准码 -32700/-32600/
// ---- -32601/-32602/-32603 分层，server 对外与 client 解包共用）----------
inline constexpr int kErrBearer = -32001;          // Bearer 准入失败（HTTP 401）
inline constexpr int kErrRateLimited = -32002;     // 限频（HTTP 侧同时 429）
inline constexpr int kErrUnknownSession = -32003;  // 未知/过期 session
inline constexpr int kErrGroupToken = -32004;      // 群组令牌错误
inline constexpr int kErrSignature = -32005;       // 签名/指纹/黑名单拒绝

// ---- 线字段名（§8 各报文；阶段 0 链路实际消费的字段）--------------------
inline constexpr char kFieldNodeId[] = "node_id";
inline constexpr char kFieldPubkey[] = "pubkey";
inline constexpr char kFieldNonce[] = "nonce";
inline constexpr char kFieldChallenge[] = "challenge";
inline constexpr char kFieldChallengeSig[] = "challenge_sig";
inline constexpr char kFieldGroupToken[] = "group_token";
inline constexpr char kFieldAdvertise[] = "advertise";
inline constexpr char kFieldAddr[] = "addr";
inline constexpr char kFieldDirect[] = "direct";
inline constexpr char kFieldAgent[] = "agent";
inline constexpr char kFieldStatus[] = "status";
inline constexpr char kFieldSession[] = "session";
inline constexpr char kFieldHeartbeatInterval[] = "heartbeat_interval_s";
inline constexpr char kFieldServerTime[] = "server_time";
inline constexpr char kFieldSha256[] = "sha256";
inline constexpr char kFieldSources[] = "sources";
inline constexpr char kFieldSourceNodeType[] = "type";
inline constexpr char kFieldLastSeen[] = "last_seen";
inline constexpr char kFieldBy[] = "by";
inline constexpr char kFieldName[] = "name";
inline constexpr char kFieldSize[] = "size";
inline constexpr char kFieldUrl[] = "url";
inline constexpr char kFieldEtag[] = "etag";
inline constexpr char kFieldLastModified[] = "last_modified";

// 挑战值 / 会话凭据生成（server 侧；RAND_bytes 失败返回空串，调用方
// 判空走内部错误收口）。挑战 = hex(32)；session = "s-" + hex(32)（§9.3
// 随机 128b+，前缀用于线上形态肉眼区分）。
std::string make_challenge_value();
std::string make_session_id();

// 严格小写 hex 校验：长度精确等于 exact_len 且字符全部 [0-9a-f]。
// node_id=32 / pubkey hex=88 / 签名 hex=128 / nonce 等随机量按各自定长传入。
bool is_hex_string(const std::string& s, std::size_t exact_len);

// canonical JSON：nlohmann::json 默认 map 即按键排序 + 紧凑 dump
// （无空白分隔）。所有进签名与对拍的 params_json 必须经此序列化，
// 显式包装防调用方绕过（dump(-1) 与 dump() 在上游改默认值时漂移）。
inline std::string canonical_json(const nlohmann::json& j) {
    return j.dump(-1, ' ', false);
}

// 签名 payload（§8.1）：method + "\n" + nonce + "\n" + sha256_hex(params_json)
// params_json 必须已是 canonical 形态（本函数不二次序列化）。
inline std::string signing_payload(const std::string& method,
                                   const std::string& nonce,
                                   const std::string& canonical_params) {
    return method + "\n" + nonce + "\n" +
           SwarmCrypto::sha256_hex(canonical_params);
}

// 由线上 pubkey（DER(SPKI) 小写 hex 88 字符）推导节点指纹
// （sha256_hex(DER) 前 32 字符）；pubkey 非 hex/长度不符返回空串。
std::string fingerprint_from_pubkey_hex(const std::string& pubkey_hex);

// UTC RFC 3339 秒粒度（"2026-09-22T12:00:00Z"）；gmtime 失败返回空串。
// Windows 走 gmtime_s，POSIX 走 gmtime_r。
std::string rfc3339_utc(std::chrono::system_clock::time_point tp);

}  // namespace falcon::swarm
