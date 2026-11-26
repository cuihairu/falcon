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

TEST(SwarmLoopback, HealthEndpointNoAuth) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    auto reply = http_request(harness.port(),
                              "GET /v1/health HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Connection: close\r\n\r\n");
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->status, 200);
    nlohmann::json body = nlohmann::json::parse(reply->body, nullptr, false);
    ASSERT_FALSE(body.is_discarded());
    EXPECT_EQ(body.value("status", ""), "ok");
    EXPECT_EQ(body.value("peers", -1), 0);

    // 注册一节点后 peers 计数联动（health 直读 state 的只读观测）
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    auto reply2 = http_request(harness.port(),
                               "GET /v1/health HTTP/1.1\r\n"
                               "Host: 127.0.0.1\r\n"
                               "Connection: close\r\n\r\n");
    ASSERT_TRUE(reply2.has_value());
    nlohmann::json body2 = nlohmann::json::parse(reply2->body, nullptr, false);
    ASSERT_FALSE(body2.is_discarded());
    EXPECT_EQ(body2.value("peers", -1), 1);
}

TEST(SwarmLoopback, BearerMissingRejected) {
    HarnessConfig cfg;
    cfg.server_token = "tok";
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    const auto out = http_rpc_call(harness.port(), kMethodHeartbeat,
                                   nlohmann::json{{kFieldSession, "s-x"}});
    EXPECT_EQ(out.http_status, 401);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error_code, kErrBearer);
    EXPECT_EQ(out.error_message, "Missing or invalid bearer token");
}

TEST(SwarmLoopback, BearerWrongRejected) {
    HarnessConfig cfg;
    cfg.server_token = "tok";
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    const auto out =
        http_rpc_call(harness.port(), kMethodHeartbeat,
                      nlohmann::json{{kFieldSession, "s-x"}}, 1, "wrong");
    EXPECT_EQ(out.http_status, 401);
    EXPECT_EQ(out.error_code, kErrBearer);
    EXPECT_EQ(out.error_message, "Missing or invalid bearer token");
}

TEST(SwarmLoopback, BearerOkPassesThroughToDispatch) {
    HarnessConfig cfg;
    cfg.server_token = "tok";
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    // 正确 Bearer 过门后进分发（未知方法 → -32601 而非 401，证明分层）
    const auto out = http_rpc_call(harness.port(), "no.such.method",
                                   nlohmann::json::object(), 1, "tok");
    EXPECT_EQ(out.http_status, 200);
    EXPECT_EQ(out.error_code, -32601);
}

TEST(SwarmLoopback, WsUpgradeRequiresBearer) {
    HarnessConfig cfg;
    cfg.server_token = "tok";
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    SwarmWsClient ws;
    bool upgrade_ok = true;
    // 非 101 应答 → connect 返回 false 且 fd 保留（handshake_raw 可查）
    ASSERT_FALSE(ws.connect(harness.port(), {}, &upgrade_ok));
    EXPECT_FALSE(upgrade_ok);
    EXPECT_NE(ws.handshake_raw().find(" 401 "), std::string::npos);
}

// ===========================================================================
// JSON-RPC 层错误分形（HTTP 恒 200；码与消息逐字钉死）
// ===========================================================================

TEST(SwarmLoopback, ParseErrorYields32700) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 原始非 JSON body（不经 http_post_json 的序列化）
    const std::string body = "definitely not json";
    std::string req = "POST /jsonrpc HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n" + body;
    auto reply = http_request(harness.port(), req);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->status, 200);
    const auto out = parse_rpc_envelope(reply->body);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error_code, -32700);
    EXPECT_EQ(out.error_message, "Parse error");
}

TEST(SwarmLoopback, InvalidRequestWhenMethodMissing) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    const nlohmann::json body = {{"jsonrpc", "2.0"}, {"id", 7},
                                 {"params", nlohmann::json::object()}};
    auto reply = http_post_json(harness.port(), body);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->status, 200);
    const auto out = parse_rpc_envelope(reply->body);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error_code, -32600);
    EXPECT_EQ(out.error_message, "Invalid Request");
}

TEST(SwarmLoopback, ParamsNotObjectYields32600Not32602) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // params 为数组：传输层 Invalid Request 拒绝（-32600），先于方法
    // 分发——与 handler 层参数门（-32602）的分层钉子
    const auto out =
        http_rpc_call(harness.port(), kMethodQuery, nlohmann::json::array());
    EXPECT_EQ(out.http_status, 200);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.error_code, -32600);
    EXPECT_EQ(out.error_message, "params must be an object");
}

TEST(SwarmLoopback, UnknownMethodNotFound) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    const auto out = http_rpc_call(harness.port(), "falcon.swarm.nope",
                                   nlohmann::json::object());
    EXPECT_EQ(out.http_status, 200);
    EXPECT_EQ(out.error_code, -32601);
    EXPECT_EQ(out.error_message, "Method not found: falcon.swarm.nope");
}

TEST(SwarmLoopback, WrongPathNotFound) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    auto reply = http_request(harness.port(),
                              "POST /other HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Content-Length: 2\r\n"
                              "Connection: close\r\n\r\n{}");
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->status, 404);
    const auto out = parse_rpc_envelope(reply->body);
    EXPECT_EQ(out.error_code, -32600);
    EXPECT_EQ(out.error_message, "Not found");
}

TEST(SwarmLoopback, WrongMethodNotAllowed) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    auto reply = http_request(harness.port(),
                              "GET /jsonrpc HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Connection: close\r\n\r\n");
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->status, 405);
    const auto out = parse_rpc_envelope(reply->body);
    EXPECT_EQ(out.error_code, -32600);
    EXPECT_EQ(out.error_message, "Method not allowed");
}

// ===========================================================================
// 注册：参数门与五条拒绝路径
// ===========================================================================

TEST(SwarmLoopback, RegisterMissingFieldsInvalidParams) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    const auto out =
        http_rpc_call(harness.port(), kMethodRegister,
                      nlohmann::json::object());
    EXPECT_EQ(out.http_status, 200);
    EXPECT_EQ(out.error_code, -32602);
    EXPECT_EQ(out.error_message,
              "register requires string node_id, pubkey, nonce");
}

TEST(SwarmLoopback, RegisterFieldShapeGates) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());

    // node_id 非 32 hex
    const auto bad_id =
        http_rpc_call(harness.port(), kMethodRegister,
                      nlohmann::json{{kFieldNodeId, "ZZ"},
                                     {kFieldPubkey, node.pubkey_hex()},
                                     {kFieldNonce, make_nonce()}});
    EXPECT_EQ(bad_id.error_code, -32602);
    EXPECT_EQ(bad_id.error_message, "node_id must be 32 lowercase hex chars");

    // pubkey 非 88 hex
    const auto bad_pubkey =
        http_rpc_call(harness.port(), kMethodRegister,
                      nlohmann::json{{kFieldNodeId, node.node_id()},
                                     {kFieldPubkey, "abcd"},
                                     {kFieldNonce, make_nonce()}});
    EXPECT_EQ(bad_pubkey.error_code, -32602);
    EXPECT_EQ(bad_pubkey.error_message,
              "pubkey must be 88 lowercase hex chars");

    // nonce 非 16 hex
    const auto bad_nonce =
        http_rpc_call(harness.port(), kMethodRegister,
                      nlohmann::json{{kFieldNodeId, node.node_id()},
                                     {kFieldPubkey, node.pubkey_hex()},
                                     {kFieldNonce, "nothex"}});
    EXPECT_EQ(bad_nonce.error_code, -32602);
    EXPECT_EQ(bad_nonce.error_message, "nonce must be 16 lowercase hex chars");
}

TEST(SwarmLoopback, RegisterChallengeRoundtripLoopback) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());

    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    // session 形态："s-" + 32 hex（§9.3 编码定案）
    EXPECT_EQ(flow.session.size(), 34u);
    EXPECT_EQ(flow.session.substr(0, 2), "s-");
    EXPECT_TRUE(is_hex_string(flow.session.substr(2), 32));

    // state 侧观测：会话在册
    EXPECT_TRUE(harness.state().has_session(flow.session));
    EXPECT_EQ(harness.state().peer_count(), 1u);
}

TEST(SwarmLoopback, GroupTokenMismatchAndAccept) {
    HarnessConfig cfg;
    cfg.group_token = "g-secret";
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());

    // 缺 group_token → -32004（默认空串 != 配置的 g-secret）
    const auto rej =
        register_node_two_steps(harness.port(), node, make_nonce());
    EXPECT_FALSE(rej.ok);
    EXPECT_EQ(rej.error_code, kErrGroupToken);
    EXPECT_EQ(rej.error_message, "Invalid group token");

    // 正确令牌放行（新节点——被拒的 step1 挑战会被下一步 step1 幂等
    // 刷新，但直接换新节点语义更干净）
    SwarmTestNode good;
    const auto ok = register_node_two_steps(harness.port(), good,
                                            make_nonce(), {}, "g-secret");
    EXPECT_TRUE(ok.ok) << ok.error_message;
    EXPECT_EQ(harness.state().peer_count(), 1u);
}

TEST(SwarmLoopback, BlacklistedNodeRejectedAtStep1) {
    SwarmTestNode node;
    HarnessConfig cfg;
    cfg.blacklist = {node.node_id()};
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    EXPECT_FALSE(flow.ok);
    EXPECT_EQ(flow.error_code, kErrSignature);
    EXPECT_EQ(flow.error_message, "Node is blacklisted");
    EXPECT_EQ(harness.state().peer_count(), 0u);
}

TEST(SwarmLoopback, IdHijackRejectedAtStep1) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode victim;
    SwarmTestNode attacker;

    // attacker 的 pubkey + victim 的 node_id：指纹自洽门在挑战之前拒绝
    nlohmann::json params{
        {kFieldNodeId, victim.node_id()},
        {kFieldPubkey, attacker.pubkey_hex()},
        {kFieldNonce, make_nonce()},
        {kFieldAgent, "falcon-test"},
    };
    const auto out =
        http_rpc_call(harness.port(), kMethodRegister, params);
    EXPECT_EQ(out.error_code, kErrSignature);
    EXPECT_EQ(out.error_message, "node_id does not match pubkey fingerprint");
}

TEST(SwarmLoopback, BadSignatureRejected) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;

    // step1 拿挑战
    const nlohmann::json step1 = node.step1_params(make_nonce());
    const auto r1 = http_rpc_call(harness.port(), kMethodRegister, step1);
    ASSERT_TRUE(r1.ok);
    ASSERT_TRUE(r1.result.contains(kFieldChallenge));
    EXPECT_EQ(r1.result.value(kFieldStatus, ""), "challenge");

    // step2 带 128 hex 伪签名 → 验签失败（挑战已单次消耗）
    const std::string fake_sig(128, 'a');
    const auto r2 = http_rpc_call(
        harness.port(), kMethodRegister,
        SwarmTestNode::step2_params(step1, fake_sig));
    EXPECT_EQ(r2.error_code, kErrSignature);
    EXPECT_EQ(r2.error_message,
              "Challenge signature verification failed");
    EXPECT_EQ(harness.state().peer_count(), 0u);
}

TEST(SwarmLoopback, ParamsMismatchReplayRejected) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;

    const std::string nonce = make_nonce();
    const nlohmann::json step1 = node.step1_params(nonce);
    const auto r1 = http_rpc_call(harness.port(), kMethodRegister, step1);
    ASSERT_TRUE(r1.ok);

    // step2 篡改 agent（去 challenge_sig 后 canonical 与 step1 快照不一致
    // → 参数绑定门先于验签拒绝，防两步之间参数偷换 §9.2）
    const std::string sig =
        node.sign_register(nonce, canonical_json(step1));
    nlohmann::json tampered = step1;
    tampered[kFieldAgent] = "tampered";
    tampered[kFieldChallengeSig] = sig;
    const auto r2 =
        http_rpc_call(harness.port(), kMethodRegister, tampered);
    EXPECT_EQ(r2.error_code, kErrSignature);
    EXPECT_EQ(r2.error_message, "Register params changed between steps");
    EXPECT_EQ(harness.state().peer_count(), 0u);
}

// ===========================================================================
// 心跳 / 查询 / 注销（未命中查询：sha256 回显 + sources 空数组）
// ===========================================================================

TEST(SwarmLoopback, HeartbeatRoundtripAndUnknownSession) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    const auto hb = http_rpc_call(harness.port(), kMethodHeartbeat,
                                  nlohmann::json{{kFieldSession, flow.session}});
    EXPECT_TRUE(hb.ok) << hb.error_message;
    EXPECT_TRUE(hb.result.contains(kFieldServerTime));

    const auto unknown = http_rpc_call(
        harness.port(), kMethodHeartbeat,
        nlohmann::json{{kFieldSession, "s-0000000000000000000000000000ffff"}});
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error_code, kErrUnknownSession);
    EXPECT_EQ(unknown.error_message, "Unknown or expired session");
}

TEST(SwarmLoopback, HeartbeatMissingSessionInvalidParams) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    const auto out =
        http_rpc_call(harness.port(), kMethodHeartbeat,
                      nlohmann::json::object());
    EXPECT_EQ(out.error_code, -32602);
    EXPECT_EQ(out.error_message, "session must be a string");
}

TEST(SwarmLoopback, QueryEmptyIndexWellFormedLoopback) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    // 空表往返：合法 session → sha256 回显 + sources 空数组
    const std::string sha = make_sha256();
    const auto hit = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, flow.session}, {kFieldSha256, sha}});
    EXPECT_TRUE(hit.ok) << hit.error_message;
    EXPECT_EQ(hit.result.value(kFieldSha256, ""), sha);
    ASSERT_TRUE(hit.result.contains(kFieldSources));
    EXPECT_TRUE(hit.result.at(kFieldSources).is_array());
    EXPECT_TRUE(hit.result.at(kFieldSources).empty());

    // sha256 形态门（handler 层 -32602）
    const auto bad_sha = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, flow.session}, {kFieldSha256, "xy"}});
    EXPECT_EQ(bad_sha.error_code, -32602);
    EXPECT_EQ(bad_sha.error_message,
              "sha256 must be 64 lowercase hex chars");

    // 无效 session → -32003
    const auto bad_session = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, "s-ffffffffffffffffffffffffffffffff"},
                       {kFieldSha256, sha}});
    EXPECT_EQ(bad_session.error_code, kErrUnknownSession);
}

TEST(SwarmLoopback, UnsubscribeRemovesPeerLoopback) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    const auto un = http_rpc_call(
        harness.port(), kMethodUnsubscribe,
        nlohmann::json{{kFieldSession, flow.session}});
    EXPECT_TRUE(un.ok) << un.error_message;
    EXPECT_EQ(un.result.value(kFieldStatus, ""), "ok");
    EXPECT_EQ(harness.state().peer_count(), 0u);

    // 注销后原 session 立即失效
    const auto hb = http_rpc_call(harness.port(), kMethodHeartbeat,
                                  nlohmann::json{{kFieldSession, flow.session}});
    EXPECT_EQ(hb.error_code, kErrUnknownSession);
}

// ===========================================================================
// announce / retract（§16.2 全链：公告归并 → 查询命中多路来源 → 摘源；
// WS onResourceAdded/onResourceExpired；per-session 限频与源配额）
// ===========================================================================

TEST(SwarmLoopback, AnnounceQueryRoundTripOverHttp) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    // file 条目公告：accepted=1 / rejected=0 / expires_at 落在钳制区间
    const std::string sha = make_sha256();
    const auto ann = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node, flow.session,
                        nlohmann::json::array(
                            {file_entry(sha, "hello.bin", 456)})));
    ASSERT_TRUE(ann.ok) << ann.error_message;
    EXPECT_EQ(ann.result.value(kFieldAccepted, std::size_t{0}), 1u);
    EXPECT_EQ(ann.result.value(kFieldRejected, std::size_t{1}), 0u);
    const auto expires_at = ann.result.value(kFieldExpiresAt, std::uint64_t{0});
    EXPECT_GE(expires_at, std::uint64_t{3600});   // clamp 下界
    EXPECT_LE(expires_at, std::uint64_t{604800}); // clamp 上界

    // 查询命中：资源级 name/size + node 源实时渲染（type/node_id/agent/
    // last_seen；无 advertise 字段——注册未公告可达地址）
    const auto hit = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, flow.session}, {kFieldSha256, sha}});
    ASSERT_TRUE(hit.ok) << hit.error_message;
    EXPECT_EQ(hit.result.value(kFieldSha256, ""), sha);
    EXPECT_EQ(hit.result.value(kFieldName, ""), "hello.bin");
    EXPECT_EQ(hit.result.value(kFieldSize, std::int64_t{-1}), 456);
    const auto& sources = hit.result.at(kFieldSources);
    ASSERT_TRUE(sources.is_array());
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources[0].value(kFieldSourceNodeType, ""), "node");
    EXPECT_EQ(sources[0].value(kFieldNodeId, ""), node.node_id());
    EXPECT_EQ(sources[0].value(kFieldAgent, ""), "falcon-test");
    EXPECT_TRUE(sources[0].contains(kFieldLastSeen));
    EXPECT_FALSE(sources[0].contains(kFieldAdvertise));

    // 幂等重公告：node 源 upsert 不重复（accepted 照常计）
    const auto again = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node, flow.session,
                        nlohmann::json::array(
                            {file_entry(sha, "hello.bin", 456)})));
    ASSERT_TRUE(again.ok) << again.error_message;
    EXPECT_EQ(again.result.value(kFieldAccepted, std::size_t{0}), 1u);

    const auto hit2 = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, flow.session}, {kFieldSha256, sha}});
    ASSERT_TRUE(hit2.ok) << hit2.error_message;
    EXPECT_EQ(hit2.result.at(kFieldSources).size(), 1u);
}

TEST(SwarmLoopback, AnnounceMirrorMergesIntoSameResourceOverHttp) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    // file + mirror 同 sha 同资源（§8.3 归并）：node 源 + url 源并列
    const std::string sha = make_sha256();
    const auto ann = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node, flow.session,
                        nlohmann::json::array({
                            file_entry(sha, "pkg.tar", 4096),
                            mirror_entry(sha, "https://mir.example/pkg.tar",
                                         "\"v1\"", true),
                        })));
    ASSERT_TRUE(ann.ok) << ann.error_message;
    EXPECT_EQ(ann.result.value(kFieldAccepted, std::size_t{0}), 2u);

    const auto hit = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, flow.session}, {kFieldSha256, sha}});
    ASSERT_TRUE(hit.ok) << hit.error_message;
    const auto& sources = hit.result.at(kFieldSources);
    ASSERT_TRUE(sources.is_array());
    ASSERT_EQ(sources.size(), 2u);

    // url 源字段全在（etag/last_modified/accept_ranges 恒渲染）
    bool saw_url = false;
    for (const auto& src : sources) {
        if (src.value(kFieldSourceNodeType, "") == "url") {
            saw_url = true;
            EXPECT_EQ(src.value(kFieldUrl, ""), "https://mir.example/pkg.tar");
            EXPECT_EQ(src.value(kFieldEtag, ""), "\"v1\"");
            EXPECT_EQ(src.value(kFieldAcceptRanges, false), true);
            EXPECT_TRUE(src.contains(kFieldLastModified));
        }
    }
    EXPECT_TRUE(saw_url) << "url source missing after file+mirror announce";
}

TEST(SwarmLoopback, RetractRemovesAndQueryMissesOverHttp) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    const std::string sha = make_sha256();
    ASSERT_TRUE(http_rpc_call(
                    harness.port(), kMethodAnnounce,
                    signed_announce(node, flow.session,
                                    nlohmann::json::array({file_entry(sha)})))
                    .ok);

    // 摘除：removed=1 / unknown=0；唯一源被摘 → 资源删除 → 查询落空
    const auto ret = http_rpc_call(
        harness.port(), kMethodRetract,
        signed_retract(node, flow.session, nlohmann::json::array({sha})));
    ASSERT_TRUE(ret.ok) << ret.error_message;
    EXPECT_EQ(ret.result.value(kFieldRemoved, std::size_t{0}), 1u);
    EXPECT_EQ(ret.result.value(kFieldUnknown, std::size_t{1}), 0u);

    const auto miss = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, flow.session}, {kFieldSha256, sha}});
    ASSERT_TRUE(miss.ok) << miss.error_message;
    EXPECT_EQ(miss.result.value(kFieldSha256, ""), sha);
    EXPECT_TRUE(miss.result.at(kFieldSources).empty());

    // 重复 retract：表中已无该哈希 → unknown=1（计数口径 §16.2）
    const auto ret2 = http_rpc_call(
        harness.port(), kMethodRetract,
        signed_retract(node, flow.session, nlohmann::json::array({sha})));
    ASSERT_TRUE(ret2.ok) << ret2.error_message;
    EXPECT_EQ(ret2.result.value(kFieldRemoved, std::size_t{1}), 0u);
    EXPECT_EQ(ret2.result.value(kFieldUnknown, std::size_t{0}), 1u);
}

TEST(SwarmLoopback, AnnounceNotificationsReachWebSocket) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 订阅者先连（广播时已在订阅者表）；peerJoined 帧被 read 过滤跳过
    SwarmWsClient sub;
    ASSERT_TRUE(sub.connect(harness.port()));

    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    const std::string sha = make_sha256();
    const auto ann = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node, flow.session,
                        nlohmann::json::array(
                            {file_entry(sha, "notify.bin", 128)})));
    ASSERT_TRUE(ann.ok) << ann.error_message;

    const auto n = read_until_method(sub, kNotifyResourceAdded, 5000);
    ASSERT_TRUE(n.has_value()) << "no onResourceAdded within budget";
    EXPECT_EQ(n->value("jsonrpc", ""), "2.0");
    EXPECT_EQ(n->value("method", ""), std::string(kNotifyResourceAdded));
    EXPECT_FALSE(n->contains("id"));  // 通知帧无 id（请求信封才带）
    ASSERT_TRUE(n->contains("params"));
    const auto& params = n->at("params");
    ASSERT_TRUE(params.is_object());
    EXPECT_EQ(params.value(kFieldSha256, ""), sha);
    EXPECT_EQ(params.value(kFieldName, ""), "notify.bin");
    EXPECT_EQ(params.value(kFieldSize, std::int64_t{-1}), 128);
    EXPECT_EQ(params.value(kFieldBy, ""), node.node_id());
    ASSERT_TRUE(params.contains(kFieldSources));
    ASSERT_EQ(params.at(kFieldSources).size(), 1u);
}

TEST(SwarmLoopback, OnResourceExpiredBothTriggersOverWire) {
    SwarmRendezvousHarness harness;  // 默认 timeout=2s / sweep=50ms（测试值）
    ASSERT_TRUE(harness.ok());

    SwarmWsClient sub;
    ASSERT_TRUE(sub.connect(harness.port()));

    // 触发面 (a)：retract 摘空唯一源 → 资源删除 + onResourceExpired
    SwarmTestNode node_a;
    const auto flow_a =
        register_node_two_steps(harness.port(), node_a, make_nonce());
    ASSERT_TRUE(flow_a.ok) << flow_a.error_message;
    const std::string sha_x = make_sha256();
    ASSERT_TRUE(http_rpc_call(
                    harness.port(), kMethodAnnounce,
                    signed_announce(node_a, flow_a.session,
                                    nlohmann::json::array({file_entry(sha_x)})))
                    .ok);
    // onResourceAdded 先到（同帧流上的次序锚——过期帧不早于资源存在）
    const auto added = read_until_method(sub, kNotifyResourceAdded, 5000);
    ASSERT_TRUE(added.has_value());

    ASSERT_TRUE(http_rpc_call(
                    harness.port(), kMethodRetract,
                    signed_retract(node_a, flow_a.session,
                                   nlohmann::json::array({sha_x})))
                    .ok);
    const auto expired_a =
        read_until_method(sub, kNotifyResourceExpired, 5000);
    ASSERT_TRUE(expired_a.has_value()) << "no onResourceExpired (retract)";
    ASSERT_TRUE(expired_a->contains("params"));
    EXPECT_EQ(expired_a->at("params").value(kFieldSha256, ""), sha_x);

    // 触发面 (b)：节点心跳超时 sweep 摘除 → 名下源随节点删除 → 空资源
    // onResourceExpired。（TTL 到期面在过线上不可行——钳制下界 3600s，
    // 留给单元层虚拟时钟覆盖。）顺序注：erase_peer_locked 先推空资源的
    // onResourceExpired 再推 onPeerLeft。
    SwarmTestNode node_b;
    const auto flow_b =
        register_node_two_steps(harness.port(), node_b, make_nonce());
    ASSERT_TRUE(flow_b.ok) << flow_b.error_message;
    const std::string sha_y = make_sha256();
    ASSERT_TRUE(http_rpc_call(
                    harness.port(), kMethodAnnounce,
                    signed_announce(node_b, flow_b.session,
                                    nlohmann::json::array({file_entry(sha_y)})))
                    .ok);
    const auto added_b = read_until_method(sub, kNotifyResourceAdded, 5000);
    ASSERT_TRUE(added_b.has_value());

    // node_b 不再心跳 → timeout(2s) + sweep(50ms) 后摘除；等待期间 node_a
    // 每 800ms 心跳维持 session（末尾查询需要存活 session；超时 2s >
    // 800ms 不会摘除）。onPeerLeft 帧被 read 过滤跳过，无干扰
    const auto deadline_b = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(15000);
    auto last_hb = std::chrono::steady_clock::now();
    std::optional<nlohmann::json> expired_b;
    while (std::chrono::steady_clock::now() < deadline_b) {
        auto frame = sub.read_frame(100);
        if (frame && frame->opcode == 0x1) {
            nlohmann::json j =
                nlohmann::json::parse(frame->payload, nullptr, false);
            if (!j.is_discarded() && j.value("method", std::string()) ==
                                         std::string(kNotifyResourceExpired)) {
                expired_b = j;
                break;
            }
        }
        if (std::chrono::steady_clock::now() - last_hb >
            std::chrono::milliseconds(800)) {
            ASSERT_TRUE(http_rpc_call(harness.port(), kMethodHeartbeat,
                                      nlohmann::json{
                                          {kFieldSession, flow_a.session}})
                            .ok);
            last_hb = std::chrono::steady_clock::now();
        }
    }
    ASSERT_TRUE(expired_b.has_value()) << "no onResourceExpired (sweep)";
    EXPECT_EQ(expired_b->at("params").value(kFieldSha256, ""), sha_y);

    // 节点摘除后资源确已删除：query 落空（node_a session 存活）
    const auto miss = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, flow_a.session},
                       {kFieldSha256, sha_y}});
    ASSERT_TRUE(miss.ok) << miss.error_message;
    EXPECT_TRUE(miss.result.at(kFieldSources).empty());
}

TEST(SwarmLoopback, AnnounceRateLimitPerSessionSharesHttp429) {
    HarnessConfig cfg;
    cfg.rate_announce_per_min = 1;
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    SwarmTestNode node_a;
    const auto flow_a =
        register_node_two_steps(harness.port(), node_a, make_nonce());
    ASSERT_TRUE(flow_a.ok) << flow_a.error_message;
    SwarmTestNode node_b;
    const auto flow_b =
        register_node_two_steps(harness.port(), node_b, make_nonce());
    ASSERT_TRUE(flow_b.ok) << flow_b.error_message;

    // node_a 第 1 次放行
    const auto first = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node_a, flow_a.session,
                        nlohmann::json::array(
                            {file_entry(make_sha256())})));
    EXPECT_TRUE(first.ok) << first.error_message;

    // 同 session 第 2 次 → 429 + -32002（per-session key 计满）
    const auto second = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node_a, flow_a.session,
                        nlohmann::json::array(
                            {file_entry(make_sha256())})));
    EXPECT_EQ(second.http_status, 429);
    EXPECT_FALSE(second.ok);
    EXPECT_EQ(second.error_code, kErrRateLimited);
    EXPECT_EQ(second.error_message, "Rate limit exceeded");

    // node_b 独立 session 不受 node_a 配额影响 → per-session 钉子
    const auto other = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node_b, flow_b.session,
                        nlohmann::json::array(
                            {file_entry(make_sha256())})));
    EXPECT_TRUE(other.ok) << other.error_message;
}

TEST(SwarmLoopback, QuotaRejectionCoSendsHttp429) {
    HarnessConfig cfg;
    cfg.max_sources_per_node = 1;
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());
    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    // 名下 0 源 + 本批 2 条 > 上限 1 → 整批拒绝：-32002 + HTTP 429 同发
    const std::string sha = make_sha256();
    const auto batch = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node, flow.session,
                        nlohmann::json::array({file_entry(sha),
                                               file_entry(sha64('9'))})));
    EXPECT_EQ(batch.http_status, 429);
    EXPECT_FALSE(batch.ok);
    EXPECT_EQ(batch.error_code, kErrRateLimited);
    EXPECT_EQ(batch.error_message, "Source quota exceeded for node");

    // 拒绝后零残留：两条都未入库
    const auto q1 = http_rpc_call(
        harness.port(), kMethodQuery,
        nlohmann::json{{kFieldSession, flow.session}, {kFieldSha256, sha}});
    EXPECT_TRUE(q1.ok);
    EXPECT_TRUE(q1.result.at(kFieldSources).empty());

    // 单条 = 恰好在上限内 → 放行
    const auto single = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node, flow.session,
                        nlohmann::json::array({file_entry(sha)})));
    ASSERT_TRUE(single.ok) << single.error_message;
    EXPECT_EQ(single.result.value(kFieldAccepted, std::size_t{0}), 1u);

    // 已有 1 源 + 再 1 条 > 上限 → 再次拒绝
    const auto over = http_rpc_call(
        harness.port(), kMethodAnnounce,
        signed_announce(node, flow.session,
                        nlohmann::json::array({file_entry(sha64('8'))})));
    EXPECT_EQ(over.http_status, 429);
    EXPECT_EQ(over.error_code, kErrRateLimited);
}


TEST(SwarmLoopback, WsPingPong) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    SwarmWsClient ws;
    ASSERT_TRUE(ws.connect(harness.port()));
    ws.send_ping("hb-payload");
    auto frame = ws.read_frame(5000);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, 0xA);  // pong
    EXPECT_EQ(frame->payload, "hb-payload");
}

TEST(SwarmLoopback, WsPeerJoinedNotificationFanout) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 订阅者先连（广播时已在订阅者表）
    SwarmWsClient sub;
    ASSERT_TRUE(sub.connect(harness.port()));

    SwarmTestNode node;
    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    ASSERT_TRUE(flow.ok) << flow.error_message;

    const auto n = read_until_method(sub, kNotifyPeerJoined, 5000);
    ASSERT_TRUE(n.has_value()) << "no onPeerJoined within budget";
    EXPECT_EQ(n->value("jsonrpc", ""), "2.0");
    EXPECT_EQ(n->value("method", ""), std::string(kNotifyPeerJoined));
    ASSERT_TRUE(n->contains("params"));
    ASSERT_TRUE(n->at("params").is_object());
    EXPECT_EQ(n->at("params").value(kFieldNodeId, ""), node.node_id());
    // 通知帧无 id（请求信封才带）
    EXPECT_FALSE(n->contains("id"));
}

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
