// ============================================================================
// falcon-swarmd 线协议对拍 + 限频器 + 服务器状态单测（阶段 0）
//
// 三块：
//   SwarmProtocol  —— canonical JSON / signing payload / RFC 8032 Ed25519
//                     向量（TEST1/TEST2 全链独立实算入库，见各用例注释）/
//                     指纹推导 / hex 与 RFC 3339 助手
//   SwarmRateLimiter —— 虚拟时钟（now 显式入参）零 sleep
//   SwarmServerState —— 注册两步/挑战单次消耗/参数快照绑定/黑名单/群组令牌/
//                     心跳续租/清扫摘除/空表查询（阶段 0「空表往返」裁决）
//
// 时钟纪律：State 与限频器的 now 一律显式入参——单测用
// SwarmServerState::Clock::now() 取基点后自行加减推进，零真实等待。
// ============================================================================

#include "common/swarm_crypto.hpp"
#include "common/swarm_protocol.hpp"
#include "server/swarm_rate_limiter.hpp"
#include "server/swarm_server_state.hpp"
#include "swarm_server_harness.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace falcon::swarm {
namespace {

using test::SwarmTestNode;

// ---------------------------------------------------------------------------
// RFC 8032 §7.1 官方向量（本会话以 Python cryptography 独立实算三平台终验：
// 种子 → 公钥 → 签名逐一吻合；指纹 = sha256_hex(DER(SPKI)) 前 32 字符）。
// 测试向量绝不凭记忆入库。
// ---------------------------------------------------------------------------
constexpr char kRfc8032Test1Seed[] =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr char kRfc8032Test1Pub[] =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
constexpr char kRfc8032Test1Sig[] =
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
constexpr char kRfc8032Test1Fingerprint[] = "06e3fd8fda29bb60ab59557de61edb0a";

constexpr char kRfc8032Test2Seed[] =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr char kRfc8032Test2Pub[] =
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
constexpr char kRfc8032Test2Sig[] =
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";
constexpr char kRfc8032Test2Fingerprint[] = "deb2ded39dc26fce0e6085b6fc34bf6b";

// Ed25519 公钥的 DER(SPKI) 前缀（12 字节）+ 32 字节 raw = 44 字节 DER
constexpr char kEd25519SpkiPrefix[] = "302a300506032b6570032100";

// raw 公钥 hex → DER(SPKI)
std::vector<uint8_t> spki_der_from_raw_hex(const std::string& raw_hex) {
    return SwarmCrypto::hex_to_bytes(std::string(kEd25519SpkiPrefix) + raw_hex);
}

// State 单测用配置（TTL 全部拉长——过期路径由用例显式构造越界时刻）
SwarmServerState::Config state_config(std::string group_token = {},
                                      std::vector<std::string> blacklist = {}) {
    SwarmServerState::Config cfg;
    cfg.group_token = std::move(group_token);
    cfg.blacklist = std::move(blacklist);
    cfg.heartbeat_interval = std::chrono::seconds(10);
    cfg.heartbeat_timeout = std::chrono::seconds(60);
    cfg.challenge_ttl = std::chrono::seconds(60);
    return cfg;
}

// 完整注册两步（State 直调形态）；返回 SwarmReply 与 session。
struct StateRegisterOutcome {
    SwarmServerState::SwarmReply reply;
    std::string session;
    bool ok = false;
};

StateRegisterOutcome register_via_state(SwarmServerState& state,
                                        const SwarmTestNode& node,
                                        const std::string& nonce,
                                        SwarmServerState::Clock::time_point now) {
    StateRegisterOutcome out;
    const nlohmann::json step1 = node.step1_params(nonce);
    const std::string canonical = canonical_json(step1);
    out.reply = state.start_register(node.node_id(), node.pubkey_hex(), nonce,
                                     {}, canonical, now);
    if (!out.reply.ok()) return out;
    const std::string sig = node.sign_register(nonce, canonical);
    out.reply = state.complete_register(node.node_id(), canonical, sig, now);
    if (out.reply.ok()) {
        out.session = out.reply.result.value(kFieldSession, std::string());
        out.ok = true;
    }
    return out;
}

}  // namespace

// ===========================================================================
// SwarmProtocol：canonical JSON / signing payload / 助手
// ===========================================================================

TEST(SwarmProtocol, CanonicalJsonSortedKeysCompact) {
    // 构造顺序故意乱序；nlohmann 默认 map 按键排序，dump 紧凑（零空白）
    const nlohmann::json j = {
        {"zulu", 1},
        {"alpha", nlohmann::json{{"yy", true}, {"xx", nlohmann::json::array({2, 1})}}},
        {"mike", "v"},
    };
    EXPECT_EQ(canonical_json(j),
              R"({"alpha":{"xx":[2,1],"yy":true},"mike":"v","zulu":1})");
}

TEST(SwarmProtocol, SigningPayloadShapeAndEmptySha256Vector) {
    // sha256("") = e3b0c442...（RFC 6234 空消息向量，Python 实算核对）
    EXPECT_EQ(SwarmCrypto::sha256_hex(std::string()),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(SwarmCrypto::sha256_hex(nullptr, 0),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // payload = method + "\n" + nonce + "\n" + sha256_hex(params)
    const std::string params = canonical_json(nlohmann::json{{"a", 1}});
    const std::string expect =
        std::string(kMethodRegister) + "\n0011223344556677\n" +
        SwarmCrypto::sha256_hex(params);
    EXPECT_EQ(signing_payload(kMethodRegister, "0011223344556677", params),
              expect);
    // 换 nonce/params 必然变 payload（绑定语义的负对照）
    EXPECT_NE(signing_payload(kMethodRegister, "0011223344556678", params),
              expect);
    EXPECT_NE(signing_payload(kMethodHeartbeat, "0011223344556677", params),
              expect);
}

TEST(SwarmProtocol, Rfc8032Test1SignVerifyRoundtrip) {
    const std::vector<uint8_t> seed =
        SwarmCrypto::hex_to_bytes(kRfc8032Test1Seed);
    const std::vector<uint8_t> der = spki_der_from_raw_hex(kRfc8032Test1Pub);
    ASSERT_EQ(seed.size(), 32u);
    ASSERT_EQ(der.size(), 44u);

    const std::string sig = SwarmCrypto::sign(seed, "");
    EXPECT_EQ(sig, kRfc8032Test1Sig);
    EXPECT_TRUE(SwarmCrypto::verify(der, "", kRfc8032Test1Sig));
}

TEST(SwarmProtocol, Rfc8032Test2SignVerifyRoundtrip) {
    const std::vector<uint8_t> seed =
        SwarmCrypto::hex_to_bytes(kRfc8032Test2Seed);
    const std::vector<uint8_t> der = spki_der_from_raw_hex(kRfc8032Test2Pub);
    ASSERT_EQ(seed.size(), 32u);

    const std::string msg(1, static_cast<char>(0x72));  // RFC 向量单字节 0x72
    const std::string sig = SwarmCrypto::sign(seed, msg);
    EXPECT_EQ(sig, kRfc8032Test2Sig);
    EXPECT_TRUE(SwarmCrypto::verify(der, msg, kRfc8032Test2Sig));
}

TEST(SwarmProtocol, TamperedMessageOrSignatureFailsVerify) {
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const std::string sig = SwarmCrypto::sign(node.seed(), "payload-1");

    // 消息翻转 1 bit → 验签失败
    EXPECT_FALSE(SwarmCrypto::verify(
        SwarmCrypto::hex_to_bytes(node.pubkey_hex()), "payload-2", sig));
    // 签名末字符翻转 → 验签失败（hex 尾字节部分位翻转）
    std::string tampered = sig;
    tampered.back() = tampered.back() == '0' ? '1' : '0';
    EXPECT_NE(tampered, sig);
    EXPECT_FALSE(SwarmCrypto::verify(
        SwarmCrypto::hex_to_bytes(node.pubkey_hex()), "payload-1", tampered));
}

TEST(SwarmProtocol, FingerprintDerivationMatchesRfc8032Vectors) {
    // 指纹 = sha256_hex(DER(SPKI)) 前 32 字符；两向量分别核对
    const std::vector<uint8_t> der1 = spki_der_from_raw_hex(kRfc8032Test1Pub);
    const std::vector<uint8_t> der2 = spki_der_from_raw_hex(kRfc8032Test2Pub);
    EXPECT_EQ(SwarmCrypto::fingerprint(der1), kRfc8032Test1Fingerprint);
    EXPECT_EQ(SwarmCrypto::fingerprint(der2), kRfc8032Test2Fingerprint);

    // 线上形态（DER hex 88 字符）同样可推导
    const std::string pub_hex1 =
        std::string(kEd25519SpkiPrefix) + kRfc8032Test1Pub;
    EXPECT_EQ(fingerprint_from_pubkey_hex(pub_hex1), kRfc8032Test1Fingerprint);
}

TEST(SwarmProtocol, FingerprintFromPubkeyHexRejectsMalformed) {
    // 非 hex / 长度不符 → 空串（调用方按 -32005 拒绝）
    EXPECT_EQ(fingerprint_from_pubkey_hex("zz"), std::string());
    EXPECT_EQ(fingerprint_from_pubkey_hex(std::string(87, 'a')), std::string());
    EXPECT_EQ(fingerprint_from_pubkey_hex(std::string(89, 'a')), std::string());
}

TEST(SwarmProtocol, HexHelpersRoundTripAndReject) {
    const std::vector<uint8_t> bytes = {0x00, 0x0f, 0xa5, 0xff};
    EXPECT_EQ(SwarmCrypto::bytes_to_hex(bytes.data(), bytes.size()), "000fa5ff");
    const auto back = SwarmCrypto::hex_to_bytes("000fa5ff");
    EXPECT_EQ(back, bytes);

    // 奇数长度 / 非 hex 字符 → 空
    EXPECT_TRUE(SwarmCrypto::hex_to_bytes("0").empty());
    EXPECT_TRUE(SwarmCrypto::hex_to_bytes("0g").empty());
    // 大写 hex 解码放行（容忍面），输出恒小写
    EXPECT_EQ(SwarmCrypto::bytes_to_hex(
                  SwarmCrypto::hex_to_bytes("FF").data(), 1),
              "ff");
}

TEST(SwarmProtocol, IsHexStringStrictLowercaseExactLength) {
    EXPECT_TRUE(is_hex_string("0123456789abcdef", 16));
    EXPECT_FALSE(is_hex_string("0123456789ABCDEF", 16));  // 大写拒绝
    EXPECT_FALSE(is_hex_string("0123456789abcdeg", 16));  // g 非 hex
    EXPECT_FALSE(is_hex_string("0123456789abcde", 16));   // 串短于要求
    EXPECT_FALSE(is_hex_string("0123456789abcdef", 15));  // 串长于要求
    EXPECT_FALSE(is_hex_string("0123456789abcdef0", 16));
    EXPECT_TRUE(is_hex_string("", 0));
}

TEST(SwarmProtocol, Rfc3339UtcFormat) {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 8;  // 9 月（0 基）
    tm.tm_mday = 22;
    tm.tm_hour = 12;
    tm.tm_min = 34;
    tm.tm_sec = 56;
#ifdef _WIN32
    const std::time_t t = ::_mkgmtime(&tm);
#else
    const std::time_t t = ::timegm(&tm);
#endif
    ASSERT_NE(t, static_cast<std::time_t>(-1));
    EXPECT_EQ(rfc3339_utc(std::chrono::system_clock::from_time_t(t)),
              "2026-09-22T12:34:56Z");
}

TEST(SwarmProtocol, ChallengeAndSessionIdShapes) {
    const std::string challenge = make_challenge_value();
    ASSERT_EQ(challenge.size(), 32u);
    EXPECT_TRUE(is_hex_string(challenge, 32));

    const std::string session = make_session_id();
    ASSERT_EQ(session.size(), 34u);  // "s-" 前缀 + hex(32)
    EXPECT_EQ(session.substr(0, 2), "s-");
    EXPECT_TRUE(is_hex_string(session.substr(2), 32));

    // 两次生成互异（随机源活跃性负对照）
    EXPECT_NE(challenge, make_challenge_value());
    EXPECT_NE(session, make_session_id());
}

// ===========================================================================
// SwarmRateLimiter：虚拟时钟（now 显式入参），零 sleep
// ===========================================================================

TEST(SwarmRateLimiter, BurstLimitRejectsAtMaxEvents) {
    SwarmRateLimiter lim(3, std::chrono::milliseconds(1000));
    const auto t0 = SwarmRateLimiter::Clock::now();
    EXPECT_TRUE(lim.allow("ip-a", t0));
    EXPECT_TRUE(lim.allow("ip-a", t0));
    EXPECT_TRUE(lim.allow("ip-a", t0));
    EXPECT_FALSE(lim.allow("ip-a", t0));  // 第 4 次拒绝
    EXPECT_EQ(lim.max_events(), 3u);
}

TEST(SwarmRateLimiter, WindowSlideRestoresBudget) {
    SwarmRateLimiter lim(1, std::chrono::milliseconds(500));
    const auto t0 = SwarmRateLimiter::Clock::now();
    EXPECT_TRUE(lim.allow("k", t0));
    EXPECT_FALSE(lim.allow("k", t0 + std::chrono::milliseconds(400)));
    // 窗口滑过：首个事件出窗，预算恢复
    EXPECT_TRUE(lim.allow("k", t0 + std::chrono::milliseconds(600)));
    EXPECT_FALSE(lim.allow("k", t0 + std::chrono::milliseconds(700)));
}

TEST(SwarmRateLimiter, PerKeyIsolation) {
    SwarmRateLimiter lim(1, std::chrono::milliseconds(1000));
    const auto t0 = SwarmRateLimiter::Clock::now();
    EXPECT_TRUE(lim.allow("ip-1", t0));
    EXPECT_FALSE(lim.allow("ip-1", t0));
    EXPECT_TRUE(lim.allow("ip-2", t0));  // 各 key 独立记账
    EXPECT_FALSE(lim.allow("ip-2", t0));
}

TEST(SwarmRateLimiter, ZeroMaxEventsMeansUnlimited) {
    SwarmRateLimiter lim(0, std::chrono::milliseconds(1000));
    const auto t0 = SwarmRateLimiter::Clock::now();
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(lim.allow("k", t0)) << "i=" << i;
    }
    EXPECT_EQ(lim.window(), std::chrono::milliseconds(1000));
}

// ===========================================================================
// SwarmServerState：注册两步 / 挑战纪律 / 门禁 / 心跳 / 清扫 / 空表查询
// ===========================================================================

TEST(SwarmServerState, RegisterChallengeFlowIssuesSession) {
    SwarmServerState state(state_config());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmServerState::Clock::now();

    const std::string nonce = "0011223344556677";
    const nlohmann::json step1 = node.step1_params(nonce);
    const std::string canonical = canonical_json(step1);

    // step1：挑战形态（hex32），无 session
    auto r1 = state.start_register(node.node_id(), node.pubkey_hex(), nonce,
                                   {}, canonical, t0);
    ASSERT_TRUE(r1.ok()) << r1.error_message;
    ASSERT_EQ(r1.result.at(kFieldStatus).get<std::string>(), "challenge");
    const std::string challenge = r1.result.at(kFieldChallenge).get<std::string>();
    EXPECT_TRUE(is_hex_string(challenge, 32));
    EXPECT_EQ(state.peer_count(), 0u);  // 未完成注册不入 peer 表

    // step2：同参数 + 真实签名 → session（"s-" + hex32）+ 续租间隔回显
    const std::string sig = node.sign_register(nonce, canonical);
    auto r2 = state.complete_register(node.node_id(), canonical, sig, t0);
    ASSERT_TRUE(r2.ok()) << r2.error_message;
    const std::string session = r2.result.at(kFieldSession).get<std::string>();
    EXPECT_EQ(session.size(), 34u);
    EXPECT_EQ(session.substr(0, 2), "s-");
    EXPECT_EQ(r2.result.at(kFieldHeartbeatInterval).get<long long>(), 10);
    EXPECT_TRUE(r2.result.contains(kFieldServerTime));

    EXPECT_TRUE(state.has_session(session));
    EXPECT_EQ(state.peer_count(), 1u);
}

TEST(SwarmServerState, SnapshotMismatchBetweenStepsRejected) {
    SwarmServerState state(state_config());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmServerState::Clock::now();

    const std::string nonce = "0011223344556677";
    const std::string canonical1 =
        canonical_json(node.step1_params(nonce, "falcon-test"));
    auto r1 = state.start_register(node.node_id(), node.pubkey_hex(), nonce, {},
                                   canonical1, t0);
    ASSERT_TRUE(r1.ok());

    // step2 偷换参数（agent 换值）→ canonical 不一致 → -32005
    const std::string canonical2 =
        canonical_json(node.step1_params(nonce, "someone-else"));
    const std::string sig = node.sign_register(nonce, canonical2);
    auto r2 = state.complete_register(node.node_id(), canonical2, sig, t0);
    EXPECT_FALSE(r2.ok());
    EXPECT_EQ(r2.error_code, kErrSignature);
    EXPECT_EQ(state.peer_count(), 0u);
}

TEST(SwarmServerState, ChallengeExpiredRejected) {
    SwarmServerState state(state_config());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmServerState::Clock::now();

    const std::string nonce = "0011223344556677";
    const std::string canonical = canonical_json(node.step1_params(nonce));
    auto r1 = state.start_register(node.node_id(), node.pubkey_hex(), nonce, {},
                                   canonical, t0);
    ASSERT_TRUE(r1.ok());

    // TTL=60s：越界一瞬即拒
    const std::string sig = node.sign_register(nonce, canonical);
    auto r2 = state.complete_register(node.node_id(), canonical, sig,
                                      t0 + std::chrono::seconds(61));
    EXPECT_FALSE(r2.ok());
    EXPECT_EQ(r2.error_code, kErrSignature);
}

TEST(SwarmServerState, ChallengeConsumedOnFirstComplete) {
    SwarmServerState state(state_config());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmServerState::Clock::now();

    const std::string nonce = "0011223344556677";
    const std::string canonical = canonical_json(node.step1_params(nonce));
    ASSERT_TRUE(state
                    .start_register(node.node_id(), node.pubkey_hex(), nonce, {},
                                    canonical, t0)
                    .ok());
    const std::string sig = node.sign_register(nonce, canonical);
    ASSERT_TRUE(state.complete_register(node.node_id(), canonical, sig, t0).ok());

    // 挑战单次消耗：同签名重放 → 无 pending challenge → -32005
    auto replay = state.complete_register(node.node_id(), canonical, sig, t0);
    EXPECT_FALSE(replay.ok());
    EXPECT_EQ(replay.error_code, kErrSignature);
}

TEST(SwarmServerState, BlacklistedNodeNeverGetsChallenge) {
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    SwarmServerState state(state_config({}, {node.node_id()}));
    const auto t0 = SwarmServerState::Clock::now();

    const std::string nonce = "0011223344556677";
    const std::string canonical = canonical_json(node.step1_params(nonce));
    auto r1 = state.start_register(node.node_id(), node.pubkey_hex(), nonce, {},
                                   canonical, t0);
    EXPECT_FALSE(r1.ok());
    EXPECT_EQ(r1.error_code, kErrSignature);  // 黑名单走 -32005（§9.1）

    // 黑名单未发挑战：后续 complete 亦无 pending 可消耗
    const std::string sig = node.sign_register(nonce, canonical);
    auto r2 = state.complete_register(node.node_id(), canonical, sig, t0);
    EXPECT_FALSE(r2.ok());
    EXPECT_EQ(r2.error_code, kErrSignature);
}

TEST(SwarmServerState, GroupTokenMismatchRejected) {
    SwarmServerState state(state_config("circle-token"));
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmServerState::Clock::now();

    const std::string nonce = "0011223344556677";
    const std::string canonical = canonical_json(node.step1_params(nonce));
    auto r1 = state.start_register(node.node_id(), node.pubkey_hex(), nonce,
                                   "wrong-token", canonical, t0);
    EXPECT_FALSE(r1.ok());
    EXPECT_EQ(r1.error_code, kErrGroupToken);

    // 正确令牌放行（至挑战段即可——群组门在指纹门之后）
    auto r2 = state.start_register(node.node_id(), node.pubkey_hex(), nonce,
                                   "circle-token", canonical, t0);
    EXPECT_TRUE(r2.ok()) << r2.error_message;
}

TEST(SwarmServerState, FingerprintPubkeyMismatchRejected) {
    // ID 抢注：B 节点自报 A 的 node_id（配 B 的 pubkey）→ 指纹自洽门拒绝
    SwarmServerState state(state_config());
    SwarmTestNode victim;
    SwarmTestNode attacker;
    ASSERT_TRUE(victim.valid() && attacker.valid());
    const auto t0 = SwarmServerState::Clock::now();

    const std::string nonce = "0011223344556677";
    const std::string canonical =
        canonical_json(attacker.step1_params(nonce));
    auto r1 = state.start_register(victim.node_id(), attacker.pubkey_hex(),
                                   nonce, {}, canonical, t0);
    EXPECT_FALSE(r1.ok());
    EXPECT_EQ(r1.error_code, kErrSignature);
}

TEST(SwarmServerState, HeartbeatUnknownSessionRejected) {
    SwarmServerState state(state_config());
    auto hb = state.heartbeat("s-deadbeefdeadbeefdeadbeefdeadbeef",
                              SwarmServerState::Clock::now());
    EXPECT_FALSE(hb.ok());
    EXPECT_EQ(hb.error_code, kErrUnknownSession);
}

TEST(SwarmServerState, HeartbeatRenewsAndExpiredSessionRejected) {
    SwarmServerState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmServerState::Clock::now();
    auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok) << reg.reply.error_message;

    // 续租：timeout=60s，t+50 心跳把有效期推到 t+110
    auto hb = state.heartbeat(reg.session, t0 + std::chrono::seconds(50));
    ASSERT_TRUE(hb.ok()) << hb.error_message;
    EXPECT_TRUE(hb.result.contains(kFieldServerTime));

    // 续租生效铁证：t+100（原有效期之外）依然可用
    auto hb2 = state.heartbeat(reg.session, t0 + std::chrono::seconds(100));
    EXPECT_TRUE(hb2.ok()) << hb2.error_message;

    // 长期不续租：过期即拒（sweep 未扫也按无效会话收口）
    auto hb3 = state.heartbeat(reg.session, t0 + std::chrono::seconds(300));
    EXPECT_FALSE(hb3.ok());
    EXPECT_EQ(hb3.error_code, kErrUnknownSession);
}

TEST(SwarmServerState, SweepRemovesExpiredSessionWithPeerLeft) {
    SwarmServerState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmServerState::Clock::now();
    auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok) << reg.reply.error_message;
    EXPECT_EQ(state.peer_count(), 1u);

    // 未到期不摘
    auto early = state.sweep(t0 + std::chrono::seconds(59));
    EXPECT_TRUE(early.empty());
    EXPECT_EQ(state.peer_count(), 1u);

    // 到期清扫：onPeerLeft（object params，node_id 指名）+ 表归位
    auto swept = state.sweep(t0 + std::chrono::seconds(61));
    ASSERT_EQ(swept.size(), 1u);
    EXPECT_EQ(swept[0].method, kNotifyPeerLeft);
    ASSERT_TRUE(swept[0].params.is_object());
    EXPECT_EQ(swept[0].params.at(kFieldNodeId).get<std::string>(),
              node.node_id());
    EXPECT_EQ(state.peer_count(), 0u);
    EXPECT_FALSE(state.has_session(reg.session));
}

TEST(SwarmServerState, QueryEmptyIndexReturnsWellFormedMiss) {
    // 阶段 0「空表往返」裁决：资源表恒空（无 announce 写入路径），
    // 合法 session 查询恒返回 {sha256 回显 + sources 空数组}
    SwarmServerState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmServerState::Clock::now();
    auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok) << reg.reply.error_message;

    const std::string sha = SwarmCrypto::sha256_hex(std::string("file-bytes"));
    auto q = state.query(reg.session, sha, t0);
    ASSERT_TRUE(q.ok()) << q.error_message;
    EXPECT_EQ(q.result.at(kFieldSha256).get<std::string>(), sha);
    ASSERT_TRUE(q.result.at(kFieldSources).is_array());
    EXPECT_TRUE(q.result.at(kFieldSources).empty());

    // 无效 session → -32003
    auto q2 = state.query("s-ffffffffffffffffffffffffffffffff", sha, t0);
    EXPECT_FALSE(q2.ok());
    EXPECT_EQ(q2.error_code, kErrUnknownSession);
}

TEST(SwarmServerState, UnsubscribeRemovesSessionAndEmitsPeerLeft) {
    SwarmServerState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmServerState::Clock::now();
    auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok) << reg.reply.error_message;

    auto off = state.unsubscribe(reg.session, t0);
    ASSERT_TRUE(off.ok()) << off.error_message;
    EXPECT_EQ(off.result.at(kFieldStatus).get<std::string>(), "ok");
    EXPECT_EQ(state.peer_count(), 0u);
    EXPECT_FALSE(state.has_session(reg.session));

    // 注销伴随 onPeerLeft
    ASSERT_EQ(off.notifications.size(), 1u);
    EXPECT_EQ(off.notifications[0].method, kNotifyPeerLeft);

    // 重复注销：session 已失效 → -32003
    auto off2 = state.unsubscribe(reg.session, t0);
    EXPECT_FALSE(off2.ok());
    EXPECT_EQ(off2.error_code, kErrUnknownSession);
}

TEST(SwarmServerState, ReregisterRotatesSessionWithoutPeerJoinedReplay) {
    SwarmServerState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmServerState::Clock::now();

    auto first = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(first.ok) << first.reply.error_message;
    ASSERT_EQ(first.reply.notifications.size(), 1u);  // 首次 → onPeerJoined
    EXPECT_EQ(first.reply.notifications[0].method, kNotifyPeerJoined);
    ASSERT_TRUE(first.reply.notifications[0].params.is_object());
    EXPECT_EQ(first.reply.notifications[0].params.at(kFieldNodeId)
                  .get<std::string>(),
              node.node_id());

    // 重注册：session 轮换 + 无第二次 onPeerJoined（peer 已知）
    auto second = register_via_state(state, node, "8899aabbccddeeff", t0);
    ASSERT_TRUE(second.ok) << second.reply.error_message;
    EXPECT_NE(second.session, first.session);
    EXPECT_TRUE(second.reply.notifications.empty());
    EXPECT_FALSE(state.has_session(first.session));  // 旧会话失效
    EXPECT_TRUE(state.has_session(second.session));
    EXPECT_EQ(state.peer_count(), 1u);
}

}  // namespace falcon::swarm
