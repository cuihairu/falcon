#pragma once

// ============================================================================
// Swarm RPC handlers：method → handler 纯逻辑编排层（无 socket，可单测）
//
// 职责边界：本层做 JSON 形状校验（params 为 object、字段存在性/类型/
// 定长 hex 形态，全部 -32602），编排 SwarmServerState 方法并原样透传
// SwarmReply（语义错误与通知由 state 层产出）。状态语义绝不在此重复
// 实现——handlers 只回答「请求形状合不合法」，state 回答「语义过不过」。
//
// 定长形态（§8 编码定案）：node_id=32 hex / pubkey=88 hex（DER hex）/
// nonce=16 hex / challenge_sig=128 hex / sha256=64 hex，is_hex_string
// 严格小写。session 只做 string 类型检查（形态由 state 的会话表查找
// 天然收口，未知形态 → -32003 而非 -32602——会话不存在与会话畸形
// 对调用方是同一处置：重新注册）。
//
// announce/retract：方法常量已定义但阶段 0 不实现，分派回 -32601。
// ============================================================================

#include <string>

#include <nlohmann/json.hpp>

#include "swarm_server_state.hpp"

namespace falcon::swarm {

using SwarmReply = SwarmServerState::SwarmReply;

// 分派一个 swarm 方法调用。params 非 object → -32602；未知方法 →
// -32601。now 透传给 state（虚拟时钟单测同款入参纪律）。
SwarmReply dispatch_swarm_method(SwarmServerState& state,
                                 const std::string& method,
                                 const nlohmann::json& params,
                                 SwarmServerState::Clock::time_point now);

}  // namespace falcon::swarm
