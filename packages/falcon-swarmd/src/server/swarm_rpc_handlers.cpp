// ============================================================================
// Swarm RPC handlers 实现（见 swarm_rpc_handlers.hpp 头注释）
// ============================================================================

#include "swarm_rpc_handlers.hpp"

#include "../common/swarm_protocol.hpp"

namespace falcon::swarm {

namespace {

using Clock = SwarmServerState::Clock;

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

SwarmReply handle_register(SwarmServerState& state,
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

SwarmReply handle_heartbeat(SwarmServerState& state,
                            const nlohmann::json& params,
                            Clock::time_point now) {
    std::string session;
    SwarmReply err;
    if (!take_session(params, &session, &err)) {
        return err;
    }
    return state.heartbeat(session, now);
}

SwarmReply handle_query(SwarmServerState& state, const nlohmann::json& params,
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

SwarmReply handle_unsubscribe(SwarmServerState& state,
                              const nlohmann::json& params,
                              Clock::time_point now) {
    std::string session;
    SwarmReply err;
    if (!take_session(params, &session, &err)) {
        return err;
    }
    return state.unsubscribe(session, now);
}

}  // namespace

SwarmReply dispatch_swarm_method(SwarmServerState& state,
                                 const std::string& method,
                                 const nlohmann::json& params,
                                 SwarmServerState::Clock::time_point now) {
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
    if (method == kMethodAnnounce || method == kMethodRetract) {
        SwarmReply reply;
        reply.error_code = -32601;
        reply.error_message =
            "Method not implemented in this phase: " + method;
        return reply;
    }
    return method_not_found(method);
}

}  // namespace falcon::swarm
