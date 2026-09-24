// ============================================================================
// falcon-swarmd 服务器回环测试：真 socket HTTP/WS 全链（阶段 0 传输层验收）
//
// 覆盖面（plan 验收 ①②）：
//   - 健康端点（无鉴权 GET /v1/health）
//   - Bearer 门（缺/错 → 401 + -32001；HTTP 与 WS 升级两处）
//   - JSON-RPC 层错误分形（-32700/-32600/-32601/-32602 按层归属——
//     params 非 object 是传输层 -32600 而非 handler -32602，钉死）
//   - 阶段 0 边界（announce/retract 未实现 -32601；非 /jsonrpc 404；
//     非 POST 405）
//   - 注册两步往返（HTTP 全链）+ 群令牌/黑名单/指纹抢注/坏签名/快照重放
//     五条拒绝路径（-32004/-32005）
//   - 心跳/查询/注销（-32003 与空表往返——用户裁决「空表往返」：
//     合法 session → sha256 回显 + sources 空数组）
//   - WS：ping→pong、onPeerJoined 广播帧形制（object params、无 id）、
//     心跳超时 sweep → onPeerLeft 广播
//   - 限频（register/query 独立阈值 → 429 + -32002）
//
// 通知帧形制钉子：{"jsonrpc","2.0"},{"method",m},{"params",object} 无 id
// （swarm_rpc_server.cpp 通知构造实锤，与 daemon 数组式 params 分叉）。
// ============================================================================

#include "swarm_server_harness.hpp"

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

}  // namespace

// ===========================================================================
// 鉴权与健康端点
// ===========================================================================

TEST(SwarmLoopback, HealthEndpointNoAuth) {
    SwarmServerHarness harness;
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
    SwarmServerHarness harness(cfg);
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
    SwarmServerHarness harness(cfg);
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
    SwarmServerHarness harness(cfg);
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
    SwarmServerHarness harness(cfg);
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
    ASSERT_TRUE(harness.ok());

    const auto out = http_rpc_call(harness.port(), "falcon.swarm.nope",
                                   nlohmann::json::object());
    EXPECT_EQ(out.http_status, 200);
    EXPECT_EQ(out.error_code, -32601);
    EXPECT_EQ(out.error_message, "Method not found: falcon.swarm.nope");
}

TEST(SwarmLoopback, AnnounceRetractNotImplementedInPhaseZero) {
    SwarmServerHarness harness;
    ASSERT_TRUE(harness.ok());

    const auto a = http_rpc_call(harness.port(), kMethodAnnounce,
                                 nlohmann::json::object());
    EXPECT_EQ(a.error_code, -32601);
    EXPECT_EQ(a.error_message,
              std::string("Method not implemented in this phase: ") +
                  kMethodAnnounce);

    const auto r = http_rpc_call(harness.port(), kMethodRetract,
                                 nlohmann::json::object());
    EXPECT_EQ(r.error_code, -32601);
    EXPECT_EQ(r.error_message,
              std::string("Method not implemented in this phase: ") +
                  kMethodRetract);
}

TEST(SwarmLoopback, WrongPathNotFound) {
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness(cfg);
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
    SwarmServerHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    const auto flow =
        register_node_two_steps(harness.port(), node, make_nonce());
    EXPECT_FALSE(flow.ok);
    EXPECT_EQ(flow.error_code, kErrSignature);
    EXPECT_EQ(flow.error_message, "Node is blacklisted");
    EXPECT_EQ(harness.state().peer_count(), 0u);
}

TEST(SwarmLoopback, IdHijackRejectedAtStep1) {
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
// 心跳 / 查询 / 注销（空表往返——用户裁决：阶段 0 资源表恒空）
// ===========================================================================

TEST(SwarmLoopback, HeartbeatRoundtripAndUnknownSession) {
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
    ASSERT_TRUE(harness.ok());

    const auto out =
        http_rpc_call(harness.port(), kMethodHeartbeat,
                      nlohmann::json::object());
    EXPECT_EQ(out.error_code, -32602);
    EXPECT_EQ(out.error_message, "session must be a string");
}

TEST(SwarmLoopback, QueryEmptyIndexWellFormedLoopback) {
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
// WS 面：ping→pong / onPeerJoined 广播 / 心跳超时 sweep → onPeerLeft
// ===========================================================================

TEST(SwarmLoopback, WsPingPong) {
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;
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
    SwarmServerHarness harness;  // 默认 timeout=2s / sweep=50ms（测试值）
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
    SwarmServerHarness harness(cfg);
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
    SwarmServerHarness harness(cfg);
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
