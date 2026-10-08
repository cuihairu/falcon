// ============================================================================
// SwarmRendezvousState 实现（见 swarm_rdv_state.hpp 头注释）
// ============================================================================

#include "swarm_rdv_state.hpp"

#include <algorithm>
#include <utility>

#include "../common/swarm_crypto.hpp"
#include "../common/swarm_protocol.hpp"

namespace falcon::swarm {

namespace {

SwarmRendezvousState::SwarmReply make_error(int code, std::string message) {
    SwarmRendezvousState::SwarmReply reply;
    reply.error_code = code;
    reply.error_message = std::move(message);
    return reply;
}

// announce ttl 钳制（§16.2：缺省 86400，钳 [3600, 604800]）。
std::chrono::seconds clamp_announce_ttl(std::chrono::seconds ttl) {
    constexpr std::chrono::seconds kMin{3600};
    constexpr std::chrono::seconds kMax{604800};
    if (ttl < kMin) return kMin;
    if (ttl > kMax) return kMax;
    return ttl;
}

// 源归属判定：node 源与名下 url 源都以 node_id 字段记录 owner
// （SwarmResourceSource 复用同一字段承载两种 owner 语义）。
bool source_owned_by(const SwarmResourceSource& src,
                     const std::string& node_id) {
    return src.node_id == node_id;
}

}  // namespace

SwarmRendezvousState::SwarmRendezvousState(Config cfg) : cfg_(std::move(cfg)) {
    blacklist_.insert(cfg_.blacklist.begin(), cfg_.blacklist.end());
}

SwarmRendezvousState::SwarmReply SwarmRendezvousState::start_register(
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

SwarmRendezvousState::SwarmReply SwarmRendezvousState::complete_register(
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

SwarmRendezvousState::SwarmReply SwarmRendezvousState::heartbeat(
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

SwarmRendezvousState::SwarmReply SwarmRendezvousState::query(
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
    // 未命中分支：{sha256 回显 + sources 空数组}；命中走归并后的多路
    // 来源渲染（§8.4）。表为空（零公告）时两分支天然等价——阶段 0 行为。
    const auto rit = resources_.find(sha256);
    if (rit == resources_.end() || rit->second.expires_at < now) {
        reply.result = nlohmann::json{
            {kFieldSha256, sha256},
            {kFieldSources, nlohmann::json::array()},
        };
        return reply;
    }

    const SwarmResource& res = rit->second;
    reply.result = nlohmann::json{
        {kFieldSha256, res.sha256},
        {kFieldName, res.name},
        {kFieldSize, res.size},
        {kFieldSources, render_sources_locked(res)},
    };
    return reply;
}

nlohmann::json SwarmRendezvousState::render_sources_locked(
    const SwarmResource& res) const {
    nlohmann::json sources = nlohmann::json::array();
    for (const SwarmResourceSource& src : res.sources) {
        if (src.is_node) {
            nlohmann::json node_src{
                {kFieldSourceNodeType, "node"},
                {kFieldNodeId, src.node_id},
            };
            // 注册态实时渲染：advertise 不可达/未公告时字段缺省——
            // 节点重注册换 advertise 后查询即刻反映，无需重公告。
            const auto pit = peers_.find(src.node_id);
            if (pit != peers_.end()) {
                node_src[kFieldAgent] = pit->second.agent;
                if (pit->second.has_advertise) {
                    node_src[kFieldAdvertise] = nlohmann::json{
                        {kFieldAddr, pit->second.advertise_addr},
                        {kFieldDirect, pit->second.advertise_direct},
                    };
                }
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
                {kFieldAcceptRanges, src.accept_ranges},
            });
        }
    }
    return sources;
}

SwarmRendezvousState::SwarmReply SwarmRendezvousState::unsubscribe(
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

void SwarmRendezvousState::erase_peer_locked(const std::string& node_id,
                                         std::vector<Notification>& out) {
    const auto pit = peers_.find(node_id);
    if (pit == peers_.end()) {
        return;
    }
    sessions_.erase(pit->second.session);
    session_expiry_.erase(pit->second.session);
    peers_.erase(pit);

    // 名下源随节点摘除（node 源 + 名下 url 源，§16.2 sweep 联动）；
    // 源清空的资源删除并广播 onResourceExpired（先收集后删除防迭代失效）。
    std::vector<std::string> emptied;
    for (auto& entry : resources_) {
        auto& sources = entry.second.sources;
        sources.erase(
            std::remove_if(sources.begin(), sources.end(),
                           [&node_id](const SwarmResourceSource& s) {
                               return source_owned_by(s, node_id);
                           }),
            sources.end());
        if (sources.empty()) {
            emptied.push_back(entry.first);
        }
    }
    for (const std::string& sha : emptied) {
        resources_.erase(sha);
        Notification n;
        n.method = kNotifyResourceExpired;
        n.params = nlohmann::json{{kFieldSha256, sha}};
        out.push_back(std::move(n));
    }

    Notification n;
    n.method = kNotifyPeerLeft;
    n.params = nlohmann::json{{kFieldNodeId, node_id}};
    out.push_back(std::move(n));
}

SwarmRendezvousState::SwarmReply SwarmRendezvousState::announce(
    const std::string& session, const std::string& canonical_params_no_sig,
    const std::string& sig, const std::vector<SwarmAnnounceEntry>& entries,
    std::size_t rejected_precount, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 会话门（与 heartbeat/query 同语义：过期未清扫亦按无效收口）
    const auto sit = sessions_.find(session);
    if (sit == sessions_.end()) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const auto eit = session_expiry_.find(session);
    if (eit == session_expiry_.end() || eit->second < now) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const auto pit = peers_.find(sit->second);
    if (pit == peers_.end()) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const std::string node_id = pit->first;
    const std::vector<uint8_t> der =
        SwarmCrypto::hex_to_bytes(pit->second.pubkey_hex);

    // 验签：nonce 位填 session（高频幂等 upsert 不逐次挑战，§16.2）。
    // 防同一 bug 自我印证由测试侧独立重导 payload（SwarmTestNode）。
    const std::string payload =
        signing_payload(kMethodAnnounce, session, canonical_params_no_sig);
    if (der.empty() || !SwarmCrypto::verify(der, payload, sig)) {
        return make_error(kErrSignature,
                          "Announce signature verification failed");
    }

    // 配额：名下源数 + 本批条目 > 上限 → 整批 -32002（保守多计：幂等
    // 重公告暂存条目在计数内，宁可错拒不放过超配额节点）
    std::size_t owned = 0;
    for (const auto& entry : resources_) {
        for (const SwarmResourceSource& src : entry.second.sources) {
            if (source_owned_by(src, node_id)) ++owned;
        }
    }
    if (owned + entries.size() > cfg_.max_sources_per_node) {
        return make_error(kErrRateLimited, "Source quota exceeded for node");
    }

    // 逐条归并（§8.3：同 sha256 同资源，多路来源并列）
    std::vector<std::string> first_seen;  // 首次出现的哈希（通知在锁内组装）
    std::size_t accepted = 0;
    uint64_t max_remaining = 0;
    for (const SwarmAnnounceEntry& e : entries) {
        if (!is_hex_string(e.sha256, 64)) {
            // handlers 已过形状门——纵深复核；畸形哈希计入 rejected
            ++rejected_precount;
            continue;
        }
        const Clock::time_point exp = now + clamp_announce_ttl(e.ttl);
        auto rit = resources_.find(e.sha256);
        const bool created = rit == resources_.end();
        if (created) {
            SwarmResource res;
            res.sha256 = e.sha256;
            res.expires_at = exp;
            rit = resources_.emplace(e.sha256, std::move(res)).first;
            first_seen.push_back(e.sha256);
        } else if (rit->second.expires_at < exp) {
            // expires_at = max(现值, now+ttl)：重复公告不缩短窗口；
            // 过期未清扫资源被重公告即按 max 公式自然处理（无特例）
            rit->second.expires_at = exp;
        }
        SwarmResource& res = rit->second;

        // name/size 资源级单值：非空后写胜出（hash_only 不覆写既有值）
        if (!e.name.empty()) {
            res.name = e.name;
        }
        if (e.has_size) {
            res.size = e.size;
        }

        if (e.is_file) {
            // node 源：owner = 会话节点，幂等 upsert（addr/direct/agent
            // 查询时按注册态实时渲染，此处不冻结快照）
            bool found = false;
            for (SwarmResourceSource& src : res.sources) {
                if (src.is_node && src.node_id == node_id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                SwarmResourceSource src;
                src.is_node = true;
                src.node_id = node_id;
                res.sources.push_back(std::move(src));
            }
        } else {
            // url 源：去重键 = (owner, url)——同 URL 两 owner 是两个
            // 独立源；已有条目刷新元数据（后写胜出）
            bool found = false;
            for (SwarmResourceSource& src : res.sources) {
                if (!src.is_node && src.node_id == node_id && src.url == e.url) {
                    found = true;
                    src.etag = e.etag;
                    src.last_modified = e.last_modified;
                    src.accept_ranges = e.accept_ranges;
                    break;
                }
            }
            if (!found) {
                SwarmResourceSource src;
                src.is_node = false;
                src.node_id = node_id;
                src.url = e.url;
                src.etag = e.etag;
                src.last_modified = e.last_modified;
                src.accept_ranges = e.accept_ranges;
                res.sources.push_back(std::move(src));
            }
        }
        ++accepted;
        // result.expires_at = 资源级（归并后现值）：max(各命中资源剩余整秒)
        // ——短 ttl 重公告不缩短他人续租，回报真实剩余（§16.2「资源级
        // expires_at」），非本批 ttl
        if (res.expires_at > now) {
            max_remaining = std::max(
                max_remaining,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        res.expires_at - now)
                        .count()));
        }
    }

    SwarmReply reply;
    reply.result = nlohmann::json{
        {kFieldAccepted, accepted},
        {kFieldRejected, rejected_precount},
        {kFieldExpiresAt, max_remaining},
    };

    // onResourceAdded：资源首次出现才广播（params 形态 §8.5）；渲染在
    // 归并完成后执行——同批 file+mirror 命中同一新资源时通知携带并后终态
    for (const std::string& sha : first_seen) {
        const SwarmResource& res = resources_.at(sha);
        Notification n;
        n.method = kNotifyResourceAdded;
        n.params = nlohmann::json{
            {kFieldSha256, sha},
            {kFieldName, res.name},
            {kFieldSize, res.size},
            {kFieldBy, node_id},
            {kFieldSources, render_sources_locked(res)},
        };
        reply.notifications.push_back(std::move(n));
    }
    return reply;
}

SwarmRendezvousState::SwarmReply SwarmRendezvousState::retract(
    const std::string& session, const std::string& canonical_params_no_sig,
    const std::string& sig, const std::vector<std::string>& sha256s,
    Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 会话门 + 验签（与 announce 同门序；nonce 位 = session）
    const auto sit = sessions_.find(session);
    if (sit == sessions_.end()) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const auto eit = session_expiry_.find(session);
    if (eit == session_expiry_.end() || eit->second < now) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const auto pit = peers_.find(sit->second);
    if (pit == peers_.end()) {
        return make_error(kErrUnknownSession, "Unknown or expired session");
    }
    const std::string node_id = pit->first;
    const std::vector<uint8_t> der =
        SwarmCrypto::hex_to_bytes(pit->second.pubkey_hex);
    const std::string payload =
        signing_payload(kMethodRetract, session, canonical_params_no_sig);
    if (der.empty() || !SwarmCrypto::verify(der, payload, sig)) {
        return make_error(kErrSignature, "Retract signature verification failed");
    }

    // 逐哈希摘名下源；removed = ≥1 源被摘；unknown = 表中无该哈希；
    // 表中有但名下无源 → 两边都不计（§16.2 计数口径）
    std::size_t removed = 0;
    std::size_t unknown = 0;
    std::vector<std::string> emptied;
    for (const std::string& sha : sha256s) {
        const auto rit = resources_.find(sha);
        if (rit == resources_.end()) {
            ++unknown;
            continue;
        }
        auto& sources = rit->second.sources;
        const auto before = sources.size();
        sources.erase(
            std::remove_if(sources.begin(), sources.end(),
                           [&node_id](const SwarmResourceSource& s) {
                               return source_owned_by(s, node_id);
                           }),
            sources.end());
        if (sources.size() != before) {
            ++removed;
        }
        if (sources.empty()) {
            emptied.push_back(sha);
        }
    }
    // 源清空的资源删除 + onResourceExpired（先摘表再组装通知）
    SwarmReply reply;
    reply.result = nlohmann::json{
        {kFieldRemoved, removed},
        {kFieldUnknown, unknown},
    };
    for (const std::string& sha : emptied) {
        resources_.erase(sha);
        Notification n;
        n.method = kNotifyResourceExpired;
        n.params = nlohmann::json{{kFieldSha256, sha}};
        reply.notifications.push_back(std::move(n));
    }
    return reply;
}

std::vector<SwarmRendezvousState::Notification> SwarmRendezvousState::sweep(
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

    // 过期资源摘除（§16.2 sweep 联动：→ onResourceExpired 通知）
    std::vector<std::string> expired_resources;
    for (auto it = resources_.begin(); it != resources_.end();) {
        if (it->second.expires_at < now) {
            expired_resources.push_back(it->first);  // 按值拷贝防 erase 悬垂
            it = resources_.erase(it);
        } else {
            ++it;
        }
    }
    for (const std::string& sha : expired_resources) {
        Notification n;
        n.method = kNotifyResourceExpired;
        n.params = nlohmann::json{{kFieldSha256, sha}};
        out.push_back(std::move(n));
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

std::size_t SwarmRendezvousState::peer_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

bool SwarmRendezvousState::has_session(const std::string& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.count(session) != 0;
}

}  // namespace falcon::swarm
