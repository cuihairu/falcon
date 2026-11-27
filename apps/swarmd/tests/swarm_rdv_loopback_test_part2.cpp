// ============================================================================
// falcon-swarmd 服务器回环测试：真 socket HTTP/WS 全链（传输层验收）
//
// 覆盖面（plan 验收 ①②）：
//   - 健康端点（无鉴权 GET /v1/health）
//   - Bearer 门（缺/错 → 401 + -32001；HTTP 与 WS 升级两处）
//   - JSON-RPC 层错误分形（-32700/-32600/-32601/-32602 按层归属——
//     params 非 object 是传输层 -32600 而非 handler -32602，钉死）
//   - 非 /jsonrpc 404；非 POST 405
//   - 注册两步往返（HTTP 全链）+ 群令牌/黑名单/指纹抢注/坏签名/快照重放
//     五条拒绝路径（-32004/-32005）
//   - 心跳/查询/注销（-32003 与未命中查询：合法 session → sha256 回显 +
//     sources 空数组）
//   - announce/retract 全链（§16.2）：file/mirror 公告 → 查询命中多路
//     来源、retract 摘源/清空删除、WS onResourceAdded/onResourceExpired
//     通知、per-session 限频与源配额（429 + -32002）
//   - WS：ping→pong、onPeerJoined 广播帧形制（object params、无 id）、
//     心跳超时 sweep → onPeerLeft 广播
//   - 限频（register/query/announce 独立阈值 → 429 + -32002）
//
// 通知帧形制钉子：{"jsonrpc","2.0"},{"method",m},{"params",object} 无 id
// （swarm_rpc_server.cpp 通知构造实锤，与 daemon 数组式 params 分叉）。
// ============================================================================

#include "swarm_rdv_harness.hpp"

#include <chrono>

namespace falcon::swarm::test {

namespace {

// 16 字节随机量的 hex 形态（nonce 用）
std::string make_nonce() {
    const auto bytes = SwarmCrypto::random_bytes(8);
    return SwarmCrypto::bytes_to_hex(bytes.data(), bytes.size());
}

// 64 字符小写 hex（sha256 参数合法形态；值随意但形态合法）
std::string make_sha256() {
    const auto bytes = SwarmCrypto::random_bytes(32);
    return SwarmCrypto::bytes_to_hex(bytes.data(), bytes.size());
}

// 循环读帧直到目标 method 到达或预算耗尽（sweep 摘除有秒级延迟，
// 单次 read_frame 的 500ms 窗口不够——budget 循环消除竞速）
std::optional<nlohmann::json> read_until_method(SwarmWsClient& client,
                                                const std::string& method,
                                                int budget_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto frame = client.read_frame(500);
        if (!frame || frame->opcode != 0x1) continue;
        nlohmann::json j = nlohmann::json::parse(frame->payload, nullptr, false);
        if (!j.is_discarded() &&
            j.value("method", std::string()) == method) {
            return std::optional<nlohmann::json>(j);
        }
    }
    return std::nullopt;
}

// ---- announce/retract 助手（§16.2；签名 payload 由 SwarmTestNode 独立
// 重导——与 server 侧验签同一构造但独立实现，防同一 bug 自我印证）--------

// 首字符可变的 64 hex 合法形态（batch 多哈希用）
std::string sha64(char variant) {
    std::string s(64, '0');
    s[0] = variant;
    return s;
}

// 组 signed announce params：sig 覆盖 {session, resources} 的 canonical
// （赋值右侧先求值——params 此时尚无 sig，与 handler 侧去 sig 验签对齐）
nlohmann::json signed_announce(const SwarmTestNode& node,
                               const std::string& session,
                               const nlohmann::json& resources) {
    nlohmann::json params;
    params[kFieldSession] = session;
    params[kFieldResources] = resources;
    params[kFieldSig] = node.sign_announce(session, canonical_json(params));
    return params;
}

nlohmann::json signed_retract(const SwarmTestNode& node,
                              const std::string& session,
                              const nlohmann::json& sha256s) {
    nlohmann::json params;
    params[kFieldSession] = session;
    params[kFieldSha256s] = sha256s;
    params[kFieldSig] = node.sign_retract(session, canonical_json(params));
    return params;
}

// file 条目：sha 必填，name/size/ttl 缺省不携带（-1 = 不带）
nlohmann::json file_entry(const std::string& sha, const std::string& name = {},
                          std::int64_t size = -1, std::int64_t ttl_s = -1) {
    nlohmann::json e;
    e[kFieldKind] = "file";
    e[kFieldSha256] = sha;
    if (!name.empty()) e[kFieldName] = name;
    if (size >= 0) e[kFieldSize] = size;
    if (ttl_s >= 0) e[kFieldTtlS] = ttl_s;
    return e;
}

// mirror 条目：url 必填，etag/ttl 缺省不携带；accept_ranges 恒带
nlohmann::json mirror_entry(const std::string& sha, const std::string& url,
                            const std::string& etag = {},
                            bool accept_ranges = false,
                            std::int64_t ttl_s = -1) {
    nlohmann::json e;
    e[kFieldKind] = "mirror";
    e[kFieldSha256] = sha;
    e[kFieldUrl] = url;
    if (!etag.empty()) e[kFieldEtag] = etag;
    if (ttl_s >= 0) e[kFieldTtlS] = ttl_s;
    e[kFieldAcceptRanges] = accept_ranges;
    return e;
}

}  // namespace

// ===========================================================================
// 鉴权与健康端点
// ===========================================================================


TEST(SwarmLoopback, WsPeerLeftOnHeartbeatTimeoutSweep) {
    SwarmRendezvousHarness harness;  // 默认 timeout=2s / sweep=50ms（测试值）
    ASSERT_TRUE(harness.ok());

    SwarmWsClient sub;
    ASSERT_TRUE(sub.connect(harness.port()));

    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    // onPeerJoined 先到
    const auto joined = read_until_method(sub, kNotifyPeerJoined, 5000);
    ASSERT_TRUE(joined.has_value());

    // 不再心跳 → timeout(2s) + sweep(50ms) 后摘除并广播 onPeerLeft
    const auto left = read_until_method(sub, kNotifyPeerLeft, 10000);
    ASSERT_TRUE(left.has_value()) << "no onPeerLeft within budget";
    ASSERT_TRUE(left->contains("params"));
    ASSERT_TRUE(left->at("params").is_object());
    EXPECT_EQ(left->at("params").value(kFieldNodeId, ""), node.node_id());
    EXPECT_EQ(harness.state().peer_count(), 0u);
}

// ===========================================================================
// 限频（独立 harness 实例防互染；HTTP 429 + -32002）
// ===========================================================================

TEST(SwarmLoopback, RateLimitRegister429) {
    HarnessConfig cfg;
    cfg.rate_register_per_min = 2;
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;

    // 前两次 step1 放行（各拿挑战——nonce 不同互不覆盖语义）
    for (int i = 0; i < 2; ++i) {
        const auto r = http_rpc_call(harness.port(), kMethodRegister,
                                     node.step1_params(make_nonce()));
        SCOPED_TRACE(std::string("step1 #") + std::to_string(i));
        EXPECT_TRUE(r.ok) << r.error_message;
    }
    // 第三次 → 429 + -32002（同 IP 同 key 计满窗口）
    const auto third = http_rpc_call(harness.port(), kMethodRegister,
                                     node.step1_params(make_nonce()));
    EXPECT_EQ(third.http_status, 429);
    EXPECT_FALSE(third.ok);
    EXPECT_EQ(third.error_code, kErrRateLimited);
    EXPECT_EQ(third.error_message, "Rate limit exceeded");
}

TEST(SwarmLoopback, RateLimitQuery429) {
    HarnessConfig cfg;
    cfg.rate_query_per_min = 1;
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    const nlohmann::json params{{kFieldSession, flow.session},
                                {kFieldSha256, make_sha256()}};
    const auto first = http_rpc_call(harness.port(), kMethodQuery, params);
    EXPECT_TRUE(first.ok) << first.error_message;

    const auto second = http_rpc_call(harness.port(), kMethodQuery, params);
    EXPECT_EQ(second.http_status, 429);
    EXPECT_EQ(second.error_code, kErrRateLimited);
    EXPECT_EQ(second.error_message, "Rate limit exceeded");
}

}  // namespace falcon::swarm::test
