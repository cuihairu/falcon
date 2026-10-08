// ============================================================================
// Swarm RPC handlers 实现（见 swarm_rpc_handlers.hpp 头注释）
// ============================================================================

#include "swarm_rpc_handlers.hpp"

#include <cctype>
#include <cstdint>
#include <set>
#include <utility>

#include "../common/swarm_protocol.hpp"

namespace falcon::swarm {

namespace {

using Clock = SwarmRendezvousState::Clock;

// 非负整数形态（size/ttl_s）：unsigned 恒非负直接放行；signed 校验 ≥0。
bool nonneg_int(const nlohmann::json& v) {
    if (v.is_number_unsigned()) {
        return true;
    }
    return v.is_number_integer() && v.get<std::int64_t>() >= 0;
}

// 镜像 URL scheme 门（§16.2 语义门，大小写不敏感）：http/https 之外
// 计 rejected（query/fragment 隐私过滤在公告方，服务端不过滤——§16.1）。
bool has_http_scheme(const std::string& url) {
    std::string head;
    head.reserve(8);
    for (std::size_t i = 0; i < url.size() && i < 8; ++i) {
        head.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(url[i]))));
    }
    return head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0;
}

SwarmReply invalid_params(std::string message) {
    SwarmReply reply;
    reply.error_code = -32602;
    reply.error_message = std::move(message);
    return reply;
}

SwarmReply method_not_found(const std::string& method) {
    SwarmReply reply;
    reply.error_code = -32601;
    reply.error_message = "Method not found: " + method;
    return reply;
}

// 可选 string 字段：缺省合法，存在但非 string 拒绝
bool optional_string(const nlohmann::json& params, const char* key,
                     std::string* out) {
    const auto it = params.find(key);
    if (it == params.end()) {
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    if (out != nullptr) {
        *out = it->get<std::string>();
    }
    return true;
}

SwarmReply handle_register(SwarmRendezvousState& state,
                           const nlohmann::json& params,
                           Clock::time_point now) {
    const auto node_it = params.find(kFieldNodeId);
    const auto pubkey_it = params.find(kFieldPubkey);
    const auto nonce_it = params.find(kFieldNonce);
    if (node_it == params.end() || !node_it->is_string() ||
        pubkey_it == params.end() || !pubkey_it->is_string() ||
        nonce_it == params.end() || !nonce_it->is_string()) {
        return invalid_params(
            "register requires string node_id, pubkey, nonce");
    }

    const std::string node_id = node_it->get<std::string>();
    const std::string pubkey = pubkey_it->get<std::string>();
    const std::string nonce = nonce_it->get<std::string>();
    if (!is_hex_string(node_id, 32)) {
        return invalid_params("node_id must be 32 lowercase hex chars");
    }
    if (!is_hex_string(pubkey, 88)) {
        return invalid_params("pubkey must be 88 lowercase hex chars");
    }
    if (!is_hex_string(nonce, 16)) {
        return invalid_params("nonce must be 16 lowercase hex chars");
    }

    // 可选字段类型门（形状在 handlers 层收口，state 不再重复解析校验）
    std::string group_token;
    if (!optional_string(params, kFieldGroupToken, &group_token)) {
        return invalid_params("group_token must be a string");
    }
    std::string agent;
    if (!optional_string(params, kFieldAgent, &agent)) {
        return invalid_params("agent must be a string");
    }
    const auto adv = params.find(kFieldAdvertise);
    if (adv != params.end()) {
        if (!adv->is_object()) {
            return invalid_params("advertise must be an object");
        }
        const auto addr = adv->find(kFieldAddr);
        if (addr == adv->end() || !addr->is_string()) {
            return invalid_params("advertise.addr must be a string");
        }
        const auto direct = adv->find(kFieldDirect);
        if (direct != adv->end() && !direct->is_boolean()) {
            return invalid_params("advertise.direct must be a boolean");
        }
    }

    const auto sig_it = params.find(kFieldChallengeSig);
    if (sig_it == params.end()) {
        // step1：原样 params 进快照（canonical 序列化由调用方约定——
        // 此处 params 即完整 step1 形态）
        return state.start_register(node_id, pubkey, nonce, group_token,
                                    canonical_json(params), now);
    }

    // step2：challenge_sig 形态门 + 摘签后 canonical 与 step1 快照比对
    if (!sig_it->is_string() ||
        !is_hex_string(sig_it->get<std::string>(), 128)) {
        return invalid_params(
            "challenge_sig must be 128 lowercase hex chars");
    }
    nlohmann::json no_sig = params;
    no_sig.erase(kFieldChallengeSig);
    return state.complete_register(node_id, canonical_json(no_sig),
                                   sig_it->get<std::string>(), now);
}

// session 参数抽取（string 类型门）；缺省/类型错返回 false
bool take_session(const nlohmann::json& params, std::string* session,
                  SwarmReply* err) {
    const auto it = params.find(kFieldSession);
    if (it == params.end() || !it->is_string()) {
        *err = invalid_params("session must be a string");
        return false;
    }
    *session = it->get<std::string>();
    return true;
}

SwarmReply handle_heartbeat(SwarmRendezvousState& state,
                            const nlohmann::json& params,
                            Clock::time_point now) {
    std::string session;
    SwarmReply err;
    if (!take_session(params, &session, &err)) {
        return err;
    }
    return state.heartbeat(session, now);
}

SwarmReply handle_query(SwarmRendezvousState& state, const nlohmann::json& params,
                        Clock::time_point now) {
    std::string session;
    SwarmReply err;
    if (!take_session(params, &session, &err)) {
        return err;
    }
    const auto sha_it = params.find(kFieldSha256);
    if (sha_it == params.end() || !sha_it->is_string() ||
        !is_hex_string(sha_it->get<std::string>(), 64)) {
        return invalid_params("sha256 must be 64 lowercase hex chars");
    }
    return state.query(session, sha_it->get<std::string>(), now);
}

SwarmReply handle_unsubscribe(SwarmRendezvousState& state,
                              const nlohmann::json& params,
                              Clock::time_point now) {
    std::string session;
    SwarmReply err;
    if (!take_session(params, &session, &err)) {
        return err;
    }
    return state.unsubscribe(session, now);
}

// announce（§16.2）：形状门（resources 数组 ≤256 / 条目对象 / kind 枚举 /
// 必填与类型 / 未知字段）→ -32602 整请求失败；语义门（sha256 非 64 小写
// hex / url scheme 非 http(s)）→ 逐条 rejected 计数；session/sig/配额归
// state（沿 register 验签先例）。分型依据 = 「params 级错误才 -32602，
// 单条资源非法计入 rejected」。
SwarmReply handle_announce(SwarmRendezvousState& state,
                           const nlohmann::json& params,
                           Clock::time_point now) {
    std::string session;
    SwarmReply err;
    if (!take_session(params, &session, &err)) {
        return err;
    }
    const auto sig_it = params.find(kFieldSig);
    if (sig_it == params.end() || !sig_it->is_string() ||
        !is_hex_string(sig_it->get<std::string>(), 128)) {
        return invalid_params("sig must be 128 lowercase hex chars");
    }
    const auto res_it = params.find(kFieldResources);
    if (res_it == params.end() || !res_it->is_array()) {
        return invalid_params("resources must be an array");
    }
    if (res_it->size() > 256) {
        return invalid_params("resources must hold at most 256 entries");
    }

    std::vector<SwarmAnnounceEntry> entries;
    entries.reserve(res_it->size());
    std::size_t rejected = 0;
    for (const auto& raw : *res_it) {
        if (!raw.is_object()) {
            return invalid_params("resources entries must be objects");
        }
        const auto kind_it = raw.find(kFieldKind);
        if (kind_it == raw.end() || !kind_it->is_string()) {
            return invalid_params("entry.kind must be a string");
        }
        const std::string kind = kind_it->get<std::string>();
        const bool is_file = (kind == "file");
        if (!is_file && kind != "mirror") {
            return invalid_params("entry.kind must be \"file\" or \"mirror\"");
        }
        const auto sha_it = raw.find(kFieldSha256);
        if (sha_it == raw.end() || !sha_it->is_string()) {
            return invalid_params("entry.sha256 must be a string");
        }
        // 严格 schema（未知字段拒绝，按 kind 分型）
        std::set<std::string> allowed{std::string(kFieldKind),
                                      std::string(kFieldSha256),
                                      std::string(kFieldTtlS)};
        if (is_file) {
            allowed.insert(kFieldName);
            allowed.insert(kFieldSize);
        } else {
            allowed.insert(kFieldUrl);
            allowed.insert(kFieldEtag);
            allowed.insert(kFieldLastModified);
            allowed.insert(kFieldAcceptRanges);
        }
        for (const auto& kv : raw.items()) {
            if (allowed.find(kv.key()) == allowed.end()) {
                return invalid_params("unknown entry field: " + kv.key());
            }
        }

        // 语义门（整请求已过形状门）：形态错 → rejected 跳过
        const std::string sha256 = sha_it->get<std::string>();
        if (!is_hex_string(sha256, 64)) {
            ++rejected;
            continue;
        }
        SwarmAnnounceEntry entry;
        entry.is_file = is_file;
        entry.sha256 = sha256;
        const auto ttl_it = raw.find(kFieldTtlS);
        if (ttl_it != raw.end()) {
            if (!nonneg_int(*ttl_it)) {
                return invalid_params("entry.ttl_s must be a non-negative integer");
            }
            entry.ttl = std::chrono::seconds{ttl_it->get<std::int64_t>()};
        }
        if (is_file) {
            const auto name_it = raw.find(kFieldName);
            if (name_it != raw.end()) {
                if (!name_it->is_string()) {
                    return invalid_params("entry.name must be a string");
                }
                entry.name = name_it->get<std::string>();
            }
            const auto size_it = raw.find(kFieldSize);
            if (size_it != raw.end()) {
                if (!nonneg_int(*size_it)) {
                    return invalid_params("entry.size must be a non-negative integer");
                }
                entry.has_size = true;
                entry.size = size_it->get<std::uint64_t>();
            }
        } else {
            const auto url_it = raw.find(kFieldUrl);
            if (url_it == raw.end() || !url_it->is_string()) {
                return invalid_params("entry.url must be a string");
            }
            entry.url = url_it->get<std::string>();
            if (!has_http_scheme(entry.url)) {
                ++rejected;
                continue;
            }
            const auto etag_it = raw.find(kFieldEtag);
            if (etag_it != raw.end()) {
                if (!etag_it->is_string()) {
                    return invalid_params("entry.etag must be a string");
                }
                entry.etag = etag_it->get<std::string>();
            }
            const auto lm_it = raw.find(kFieldLastModified);
            if (lm_it != raw.end()) {
                if (!lm_it->is_string()) {
                    return invalid_params("entry.last_modified must be a string");
                }
                entry.last_modified = lm_it->get<std::string>();
            }
            const auto ar_it = raw.find(kFieldAcceptRanges);
            if (ar_it != raw.end()) {
                if (!ar_it->is_boolean()) {
                    return invalid_params("entry.accept_ranges must be a boolean");
                }
                entry.accept_ranges = ar_it->get<bool>();
            }
        }
        entries.push_back(std::move(entry));
    }

    // 验签 payload 的 params = 去 sig 后 canonical（与 register step2 同法）
    nlohmann::json no_sig = params;
    no_sig.erase(kFieldSig);
    return state.announce(session, canonical_json(no_sig),
                          sig_it->get<std::string>(), entries, rejected, now);
}

// retract（§16.2）：sha256s 数组元素形态错（非 string/非 64 hex）按 params
// 级错误 -32602（查询面同参数形态惯例，哈希是标识符非内容）；未知哈希在
// state 侧计入 unknown 不报错。
SwarmReply handle_retract(SwarmRendezvousState& state,
                          const nlohmann::json& params,
                          Clock::time_point now) {
    std::string session;
    SwarmReply err;
    if (!take_session(params, &session, &err)) {
        return err;
    }
    const auto sig_it = params.find(kFieldSig);
    if (sig_it == params.end() || !sig_it->is_string() ||
        !is_hex_string(sig_it->get<std::string>(), 128)) {
        return invalid_params("sig must be 128 lowercase hex chars");
    }
    const auto arr_it = params.find(kFieldSha256s);
    if (arr_it == params.end() || !arr_it->is_array()) {
        return invalid_params("sha256s must be an array");
    }
    std::vector<std::string> sha256s;
    sha256s.reserve(arr_it->size());
    for (const auto& v : *arr_it) {
        if (!v.is_string() ||
            !is_hex_string(v.get<std::string>(), 64)) {
            return invalid_params(
                "sha256s entries must be 64 lowercase hex chars");
        }
        sha256s.push_back(v.get<std::string>());
    }

    nlohmann::json no_sig = params;
    no_sig.erase(kFieldSig);
    return state.retract(session, canonical_json(no_sig),
                         sig_it->get<std::string>(), sha256s, now);
}

}  // namespace

SwarmReply dispatch_swarm_method(SwarmRendezvousState& state,
                                 const std::string& method,
                                 const nlohmann::json& params,
                                 SwarmRendezvousState::Clock::time_point now) {
    if (!params.is_object()) {
        return invalid_params("params must be an object");
    }

    if (method == kMethodRegister) {
        return handle_register(state, params, now);
    }
    if (method == kMethodHeartbeat) {
        return handle_heartbeat(state, params, now);
    }
    if (method == kMethodQuery) {
        return handle_query(state, params, now);
    }
    if (method == kMethodUnsubscribe) {
        return handle_unsubscribe(state, params, now);
    }
    if (method == kMethodAnnounce) {
        return handle_announce(state, params, now);
    }
    if (method == kMethodRetract) {
        return handle_retract(state, params, now);
    }
    return method_not_found(method);
}

}  // namespace falcon::swarm
