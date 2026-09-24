// ============================================================================
// SwarmServerState 实现（见 swarm_server_state.hpp 头注释）
// ============================================================================

#include "swarm_server_state.hpp"

#include "../common/swarm_crypto.hpp"
#include "../common/swarm_protocol.hpp"

namespace falcon::swarm {

namespace {

SwarmServerState::SwarmReply make_error(int code, std::string message) {
    SwarmServerState::SwarmReply reply;
    reply.error_code = code;
    reply.error_message = std::move(message);
    return reply;
}

}  // namespace

SwarmServerState::SwarmServerState(Config cfg) : cfg_(std::move(cfg)) {
    blacklist_.insert(cfg_.blacklist.begin(), cfg_.blacklist.end());
}

SwarmServerState::SwarmReply SwarmServerState::start_register(
    const std::string& node_id, const std::string& pubkey_hex,
    const std::string& nonce, const std::string& group_token,
    const std::string& canonical_params, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 门 1：黑名单（吊销节点绝不发挑战，§9.1 人工吊销防线）
    if (blacklist_.count(node_id) != 0) {
        return make_error(kErrSignature, "Node is blacklisted");
    }

    // 门 2：指纹自洽（node_id 必须等于 fingerprint(pubkey)——ID 抢注
    // 于此处被拒，§9.2）
    const std::string derived = fingerprint_from_pubkey_hex(pubkey_hex);
    if (derived.empty() || derived != node_id) {
        return make_error(kErrSignature,
                          "node_id does not match pubkey fingerprint");
    }

    // 门 3：群组令牌（圈子准入，一次性人工发放）
    if (!cfg_.group_token.empty() && group_token != cfg_.group_token) {
        return make_error(kErrGroupToken, "Invalid group token");
    }

    // 记录挑战（同 node 重复 start = 幂等刷新）；随机源失败按内部错误收口
    const std::string challenge = make_challenge_value();
    if (challenge.empty()) {
        return make_error(-32603, "Internal error: challenge generation");
    }

    SwarmChallenge rec;
    rec.node_id = node_id;
    rec.pubkey_hex = pubkey_hex;
    rec.challenge = challenge;
    rec.nonce = nonce;
    rec.params_snapshot = canonical_params;
    rec.expires_at = now + cfg_.challenge_ttl;
    challenges_[node_id] = std::move(rec);

    SwarmReply reply;
    reply.result = nlohmann::json{
        {kFieldStatus, "challenge"},
        {kFieldChallenge, challenge},
    };
    return reply;
}

SwarmServerState::SwarmReply SwarmServerState::complete_register(
    const std::string& node_id, const std::string& canonical_params_no_sig,
    const std::string& challenge_sig, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 挑战单次有效：无论成败先摘（防暴力枚举签名）
    const auto it = challenges_.find(node_id);
    if (it == challenges_.end()) {
        return make_error(kErrSignature, "No pending challenge for node");
    }
    SwarmChallenge rec = std::move(it->second);
    challenges_.erase(it);

    if (rec.expires_at < now) {
        return make_error(kErrSignature, "Challenge expired");
    }

    // 参数绑定：step2（去 challenge_sig 后）必须与 step1 快照逐字节一致
    // ——nonce/node_id/advertise 全部锁定，防两步之间参数偷换（§9.2）
    if (canonical_params_no_sig != rec.params_snapshot) {
        return make_error(kErrSignature, "Register params changed between steps");
    }

    // 验签：payload = method + "\n" + nonce + "\n" + sha256_hex(params)
    const std::vector<uint8_t> der = SwarmCrypto::hex_to_bytes(rec.pubkey_hex);
    const std::string payload =
        signing_payload(kMethodRegister, rec.nonce, rec.params_snapshot);
    if (der.empty() ||
        !SwarmCrypto::verify(der, payload, challenge_sig)) {
        return make_error(kErrSignature, "Challenge signature verification failed");
    }

    // 会话签发：同 node 重注册覆盖旧 session
    const std::string session = make_session_id();
    if (session.empty()) {
        return make_error(-32603, "Internal error: session generation");
    }

    const bool peer_new = peers_.find(node_id) == peers_.end();
    if (!peer_new) {
        // 旧会话失效（重注册轮换）
        const std::string old_session = peers_[node_id].session;
        sessions_.erase(old_session);
        session_expiry_.erase(old_session);
    }

    // peer upsert：保留已知字段，刷新签名/时间
    SwarmPeer& peer = peers_[node_id];
    peer.node_id = node_id;
    peer.pubkey_hex = rec.pubkey_hex;
    peer.session = session;
    peer.last_seen = now;
    peer.last_seen_wall = std::chrono::system_clock::now();
    // agent/advertise 从 step1 快照解析（快照与 step2 一致已证）
    {
        nlohmann::json snap;
        try {
            snap = nlohmann::json::parse(rec.params_snapshot);
        } catch (...) {
            snap = nlohmann::json::object();
        }
        if (snap.is_object()) {
            if (auto ag = snap.find(kFieldAgent);
                ag != snap.end() && ag->is_string()) {
                peer.agent = ag->get<std::string>();
            }
            const auto adv = snap.find(kFieldAdvertise);
            if (adv != snap.end() && adv->is_object()) {
                const auto addr = adv->find(kFieldAddr);
                const auto direct = adv->find(kFieldDirect);
                if (addr != adv->end() && addr->is_string()) {
                    peer.has_advertise = true;
                    peer.advertise_addr = addr->get<std::string>();
                    if (direct != adv->end() && direct->is_boolean()) {
                        peer.advertise_direct = direct->get<bool>();
                    }
                }
            }
        }
    }
    sessions_[session] = node_id;
    session_expiry_[session] = now + cfg_.heartbeat_timeout;

    SwarmReply reply;
    reply.result = nlohmann::json{
        {kFieldStatus, "ok"},
        {kFieldSession, session},
        {kFieldHeartbeatInterval, cfg_.heartbeat_interval.count()},
        {kFieldServerTime, rfc3339_utc(std::chrono::system_clock::now())},
    };
    if (peer_new) {
        // onPeerJoined：params 为 object；advertise 不可达则字段缺省
        nlohmann::json params{
            {kFieldNodeId, node_id},
            {kFieldAgent, peer.agent},
        };
        if (peer.has_advertise) {
            params[kFieldAdvertise] = nlohmann::json{
                {kFieldAddr, peer.advertise_addr},
                {kFieldDirect, peer.advertise_direct},
            };
        }
        Notification n;
        n.method = kNotifyPeerJoined;
        n.params = std::move(params);
        reply.notifications.push_back(std::move(n));
    }
    return reply;
}

SwarmServerState::SwarmReply SwarmServerState::heartbeat(
    const std::string& session, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto sit = sessions_.find(session);
    if (sit == sessions_.end()) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const auto eit = session_expiry_.find(session);
    if (eit == session_expiry_.end() || eit->second < now) {
        // 记录已过期但尚未被 sweep 清扫：按无效会话收口
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }

    // 续租
    eit->second = now + cfg_.heartbeat_timeout;
    const std::string node_id = sit->second;
    const auto pit = peers_.find(node_id);
    if (pit != peers_.end()) {
        pit->second.last_seen = now;
        pit->second.last_seen_wall = std::chrono::system_clock::now();
    }

    SwarmReply reply;
    reply.result = nlohmann::json{
        {kFieldServerTime, rfc3339_utc(std::chrono::system_clock::now())},
    };
    return reply;
}

SwarmServerState::SwarmReply SwarmServerState::query(
    const std::string& session, const std::string& sha256,
    Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto sit = sessions_.find(session);
    if (sit == sessions_.end()) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const auto eit = session_expiry_.find(session);
    if (eit == session_expiry_.end() || eit->second < now) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }

    SwarmReply reply;
    // 阶段 0：资源表恒空（无 announce 写入路径），恒走未命中分支——
    // {sha256 回显 + sources 空数组}。命中读取逻辑在位，阶段 1 即通。
    const auto rit = resources_.find(sha256);
    if (rit == resources_.end() || rit->second.expires_at < now) {
        reply.result = nlohmann::json{
            {kFieldSha256, sha256},
            {kFieldSources, nlohmann::json::array()},
        };
        return reply;
    }

    const SwarmResource& res = rit->second;
    nlohmann::json sources = nlohmann::json::array();
    for (const SwarmResourceSource& src : res.sources) {
        if (src.is_node) {
            nlohmann::json node_src{
                {kFieldSourceNodeType, "node"},
                {kFieldNodeId, src.node_id},
            };
            const auto pit = peers_.find(src.node_id);
            if (pit != peers_.end()) {
                node_src[kFieldAgent] = pit->second.agent;
                node_src[kFieldLastSeen] =
                    rfc3339_utc(pit->second.last_seen_wall);
            }
            sources.push_back(std::move(node_src));
        } else {
            sources.push_back(nlohmann::json{
                {kFieldSourceNodeType, "url"},
                {kFieldUrl, src.url},
                {kFieldEtag, src.etag},
                {kFieldLastModified, src.last_modified},
            });
        }
    }
    reply.result = nlohmann::json{
        {kFieldSha256, res.sha256},
        {kFieldName, res.name},
        {kFieldSize, res.size},
        {kFieldSources, std::move(sources)},
    };
    return reply;
}

SwarmServerState::SwarmReply SwarmServerState::unsubscribe(
    const std::string& session, Clock::time_point now) {
    (void)now;
    std::lock_guard<std::mutex> lock(mutex_);

    const auto sit = sessions_.find(session);
    if (sit == sessions_.end()) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const std::string node_id = sit->second;

    SwarmReply reply;
    erase_peer_locked(node_id, reply.notifications);
    reply.result = nlohmann::json{{kFieldStatus, "ok"}};
    return reply;
}

void SwarmServerState::erase_peer_locked(const std::string& node_id,
                                         std::vector<Notification>& out) {
    const auto pit = peers_.find(node_id);
    if (pit == peers_.end()) {
        return;
    }
    sessions_.erase(pit->second.session);
    session_expiry_.erase(pit->second.session);
    peers_.erase(pit);

    Notification n;
    n.method = kNotifyPeerLeft;
    n.params = nlohmann::json{{kFieldNodeId, node_id}};
    out.push_back(std::move(n));
}

std::vector<SwarmServerState::Notification> SwarmServerState::sweep(
    Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Notification> out;

    // 过期挑战摘除（无通知）
    for (auto it = challenges_.begin(); it != challenges_.end();) {
        if (it->second.expires_at < now) {
            it = challenges_.erase(it);
        } else {
            ++it;
        }
    }

    // 过期资源摘除（阶段 0 表恒空；无通知——onResourceExpired 触发路径
    // 随阶段 1 announce 落地）
    for (auto it = resources_.begin(); it != resources_.end();) {
        if (it->second.expires_at < now) {
            it = resources_.erase(it);
        } else {
            ++it;
        }
    }

    // 过期会话 → 节点下线 → onPeerLeft
    // 顺序敏感：先摘循环条目再调 erase_peer_locked——后者内部也会按
    // session 摘 session_expiry_（ unsubscribe/register 换代路径复用），
    // 若先调用则本循环迭代器指向的节点被删，再 erase(it) 即 UAF。
    for (auto it = session_expiry_.begin(); it != session_expiry_.end();) {
        if (it->second < now) {
            const std::string session = it->first;
            it = session_expiry_.erase(it);
            const auto sit = sessions_.find(session);
            if (sit != sessions_.end()) {
                // 必须按值拷贝：erase_peer_locked 内部 sessions_.erase 会
                // 析构 sit->second 所在条目，按引用传参即悬垂
                const std::string node_id = sit->second;
                erase_peer_locked(node_id, out);
            }
        } else {
            ++it;
        }
    }

    return out;
}

std::size_t SwarmServerState::peer_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

bool SwarmServerState::has_session(const std::string& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.count(session) != 0;
}

}  // namespace falcon::swarm
