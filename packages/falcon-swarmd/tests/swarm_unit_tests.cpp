// ============================================================================
// falcon-swarmd 线协议对拍 + 限频器 + Rendezvous 状态单测（阶段 0）
//
// 三块：
//   SwarmProtocol  —— canonical JSON / signing payload / RFC 8032 Ed25519
//                     向量（TEST1/TEST2 全链独立实算入库，见各用例注释）/
//                     指纹推导 / hex 与 RFC 3339 助手
//   SwarmRateLimiter —— 虚拟时钟（now 显式入参）零 sleep
//   SwarmRendezvousState —— 注册两步/挑战单次消耗/参数快照绑定/黑名单/群组令牌/
//                     心跳续租/清扫摘除/空表查询（阶段 0「空表往返」裁决）
//
// 时钟纪律：State 与限频器的 now 一律显式入参——单测用
// SwarmRendezvousState::Clock::now() 取基点后自行加减推进，零真实等待。
// ============================================================================

#include "common/swarm_crypto.hpp"
#include "common/swarm_protocol.hpp"
#include "rdv/swarm_rate_limiter.hpp"
#include "rdv/swarm_rdv_state.hpp"
#include "rdv/swarm_rpc_handlers.hpp"
#include "swarm_rdv_harness.hpp"

#include <falcon/detail/injection.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>
#include <utility>
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
SwarmRendezvousState::Config state_config(std::string group_token = {},
                                      std::vector<std::string> blacklist = {}) {
    SwarmRendezvousState::Config cfg;
    cfg.group_token = std::move(group_token);
    cfg.blacklist = std::move(blacklist);
    cfg.heartbeat_interval = std::chrono::seconds(10);
    cfg.heartbeat_timeout = std::chrono::seconds(60);
    cfg.challenge_ttl = std::chrono::seconds(60);
    return cfg;
}

// 完整注册两步（State 直调形态）；返回 SwarmReply 与 session。
struct StateRegisterOutcome {
    SwarmRendezvousState::SwarmReply reply;
    std::string session;
    bool ok = false;
};

StateRegisterOutcome register_via_state(SwarmRendezvousState& state,
                                        const SwarmTestNode& node,
                                        const std::string& nonce,
                                        SwarmRendezvousState::Clock::time_point now) {
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
// SwarmRendezvousState：注册两步 / 挑战纪律 / 门禁 / 心跳 / 清扫 / 空表查询
// ===========================================================================

TEST(SwarmRendezvousState, RegisterChallengeFlowIssuesSession) {
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmRendezvousState::Clock::now();

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

TEST(SwarmRendezvousState, SnapshotMismatchBetweenStepsRejected) {
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmRendezvousState::Clock::now();

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

TEST(SwarmRendezvousState, ChallengeExpiredRejected) {
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmRendezvousState::Clock::now();

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

TEST(SwarmRendezvousState, ChallengeConsumedOnFirstComplete) {
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmRendezvousState::Clock::now();

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

TEST(SwarmRendezvousState, BlacklistedNodeNeverGetsChallenge) {
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    SwarmRendezvousState state(state_config({}, {node.node_id()}));
    const auto t0 = SwarmRendezvousState::Clock::now();

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

TEST(SwarmRendezvousState, GroupTokenMismatchRejected) {
    SwarmRendezvousState state(state_config("circle-token"));
    SwarmTestNode node;
    ASSERT_TRUE(node.valid());
    const auto t0 = SwarmRendezvousState::Clock::now();

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

TEST(SwarmRendezvousState, FingerprintPubkeyMismatchRejected) {
    // ID 抢注：B 节点自报 A 的 node_id（配 B 的 pubkey）→ 指纹自洽门拒绝
    SwarmRendezvousState state(state_config());
    SwarmTestNode victim;
    SwarmTestNode attacker;
    ASSERT_TRUE(victim.valid() && attacker.valid());
    const auto t0 = SwarmRendezvousState::Clock::now();

    const std::string nonce = "0011223344556677";
    const std::string canonical =
        canonical_json(attacker.step1_params(nonce));
    auto r1 = state.start_register(victim.node_id(), attacker.pubkey_hex(),
                                   nonce, {}, canonical, t0);
    EXPECT_FALSE(r1.ok());
    EXPECT_EQ(r1.error_code, kErrSignature);
}

TEST(SwarmRendezvousState, HeartbeatUnknownSessionRejected) {
    SwarmRendezvousState state(state_config());
    auto hb = state.heartbeat("s-deadbeefdeadbeefdeadbeefdeadbeef",
                              SwarmRendezvousState::Clock::now());
    EXPECT_FALSE(hb.ok());
    EXPECT_EQ(hb.error_code, kErrUnknownSession);
}

TEST(SwarmRendezvousState, HeartbeatRenewsAndExpiredSessionRejected) {
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmRendezvousState::Clock::now();
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

TEST(SwarmRendezvousState, SweepRemovesExpiredSessionWithPeerLeft) {
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmRendezvousState::Clock::now();
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

TEST(SwarmRendezvousState, QueryEmptyIndexReturnsWellFormedMiss) {
    // 阶段 0「空表往返」裁决：资源表恒空（无 announce 写入路径），
    // 合法 session 查询恒返回 {sha256 回显 + sources 空数组}
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmRendezvousState::Clock::now();
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

TEST(SwarmRendezvousState, UnsubscribeRemovesSessionAndEmitsPeerLeft) {
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmRendezvousState::Clock::now();
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

TEST(SwarmRendezvousState, ReregisterRotatesSessionWithoutPeerJoinedReplay) {
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmRendezvousState::Clock::now();

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

TEST(SwarmRendezvousState, AdvertiseCarriedIntoPeerJoined) {
    // advertise（addr/direct）从 step1 快照解析并透传进 onPeerJoined 通知
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmRendezvousState::Clock::now();
    const std::string nonce = "0011223344556677";
    const nlohmann::json adv{{kFieldAddr, "203.0.113.7:4500"},
                             {kFieldDirect, true}};
    const nlohmann::json step1 =
        node.step1_params(nonce, "falcon-test", &adv);
    const std::string canonical = canonical_json(step1);
    ASSERT_TRUE(state
                    .start_register(node.node_id(), node.pubkey_hex(), nonce,
                                    {}, canonical, t0)
                    .ok());
    const std::string sig = node.sign_register(nonce, canonical);
    auto reply =
        state.complete_register(node.node_id(), canonical, sig, t0);
    ASSERT_TRUE(reply.ok()) << reply.error_message;
    ASSERT_EQ(reply.notifications.size(), 1u);
    ASSERT_TRUE(reply.notifications[0].params.contains(kFieldAdvertise));
    const auto& got = reply.notifications[0].params.at(kFieldAdvertise);
    EXPECT_EQ(got.at(kFieldAddr).get<std::string>(), "203.0.113.7:4500");
    EXPECT_TRUE(got.at(kFieldDirect).get<bool>());
}

TEST(SwarmRendezvousState, QueryExpiredButUnsweptSessionRejected) {
    // 与 HeartbeatRenewsAndExpiredSessionRejected 互补：query 侧自己的
    // 过期守卫（记录已过期但未被 sweep 摘除的窗口内即按无效收口）
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmRendezvousState::Clock::now();
    auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok) << reg.reply.error_message;

    // timeout=60s：t+61 查询（不先 sweep）→ -32003
    auto out =
        state.query(reg.session, std::string(64, 'a'),
                    t0 + std::chrono::seconds(61));
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.error_code, kErrUnknownSession);
}

TEST(SwarmRendezvousState, SweepPurgesExpiredChallenges) {
    // sweep 对过期挑战的静默清除（挑战表先于会话/资源表）——可观测判据：
    // 清除后再 complete_register 命中「无 pending challenge」语义分支
    // （与 ChallengeExpiredRejected 的「挑战过期」分支互补），且节点
    // 重新走两步注册照常成功
    SwarmRendezvousState state(state_config());
    SwarmTestNode node;
    const auto t0 = SwarmRendezvousState::Clock::now();
    const std::string nonce = "0011223344556677";
    const nlohmann::json step1 = node.step1_params(nonce);
    const std::string canonical = canonical_json(step1);
    ASSERT_TRUE(state
                    .start_register(node.node_id(), node.pubkey_hex(), nonce,
                                    {}, canonical, t0)
                    .ok());

    // 挑战 TTL=60s：到期清扫（无过期会话 → 无通知，纯挑战清除）
    const auto swept = state.sweep(t0 + std::chrono::seconds(61));
    EXPECT_TRUE(swept.empty());

    const std::string sig = node.sign_register(nonce, canonical);
    auto late = state.complete_register(node.node_id(), canonical, sig,
                                        t0 + std::chrono::seconds(61));
    EXPECT_FALSE(late.ok());
    EXPECT_EQ(late.error_code, kErrSignature);

    // 同节点全新挑战照常下发
    auto again = register_via_state(state, node, "8899aabbccddeeff",
                                    t0 + std::chrono::seconds(62));
    EXPECT_TRUE(again.ok) << again.reply.error_message;
}

// ===========================================================================
// SwarmCryptoEdge：防御分支直调（nullptr/尺寸门/注入点/垃圾输入）
//
// 覆盖率定性登记（swarm_crypto.cpp 不可达集合，证据：EVP 调用链的失败
// 只能源于进程级 OOM/资源枯竭或 OpenSSL 内部错误——本机与 CI 均无注入
// 点以外的确定性构造手段）：
//   - generate_keypair 的 EVP 链（EVP_PKEY_new/RawPublic/Peer...失败分支
//     47/51-52/60-61/66-68/74-76/81-84）：无对应注入点
//   - sha256_hex 的 EVP_Digest 失败（128）：同上
//   - sign 的 EVP_PKEY_new_raw_private_key 失败（147）：合法 32 字节
//     种子恒成功，无注入点
//   - random_bytes 的 RAND_bytes 失败（235）：无注入点
// ===========================================================================

TEST(SwarmCryptoEdge, BytesToHexNullptrYieldsEmpty) {
    EXPECT_TRUE(SwarmCrypto::bytes_to_hex(nullptr, 8).empty());
}

TEST(SwarmCryptoEdge, Sha256NullptrGuardOnlyWhenDataExpected) {
    // nullptr+正长度 → 空；nullptr+0 = 空消息 → RFC 6234 空向量（对照）
    EXPECT_TRUE(SwarmCrypto::sha256_hex(nullptr, 1).empty());
    EXPECT_EQ(SwarmCrypto::sha256_hex(nullptr, 0),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(SwarmCryptoEdge, SignSeedSizeGateYieldsEmpty) {
    std::vector<uint8_t> seed(31, 0x5a);
    EXPECT_TRUE(SwarmCrypto::sign(seed, "payload").empty());
}

TEST(SwarmCryptoEdge, SignCtxNewInjectionYieldsEmpty) {
    const auto seed =
        SwarmCrypto::hex_to_bytes(kRfc8032Test1Seed);
    ASSERT_EQ(seed.size(), 32u);
    const detail::ScopedInjection inj{
        falcon::detail::InjectPoint::SwarmSignCtxNew};
    EXPECT_TRUE(SwarmCrypto::sign(seed, "payload").empty());
}

TEST(SwarmCryptoEdge, VerifyEmptyDerYieldsFalse) {
    const auto seed = SwarmCrypto::hex_to_bytes(kRfc8032Test1Seed);
    const std::string sig = SwarmCrypto::sign(seed, "payload");
    EXPECT_FALSE(SwarmCrypto::verify({}, "payload", sig));
}

TEST(SwarmCryptoEdge, VerifyGarbageDerYieldsFalse) {
    // 恰 44 字节但非 DER(SPKI)（d2i_PUBKEY 解析失败分支）
    const auto der = std::vector<uint8_t>(44, 0xab);
    const auto seed = SwarmCrypto::hex_to_bytes(kRfc8032Test1Seed);
    const std::string sig = SwarmCrypto::sign(seed, "payload");
    EXPECT_FALSE(SwarmCrypto::verify(der, "payload", sig));
}

TEST(SwarmCryptoEdge, VerifyCtxNewInjectionYieldsFalse) {
    // 合法 DER 先过解析（注入点在 d2i_PUBKEY 之后）再命中 ctx 创建失败
    const auto der = spki_der_from_raw_hex(kRfc8032Test1Pub);
    const auto seed = SwarmCrypto::hex_to_bytes(kRfc8032Test1Seed);
    const std::string sig = SwarmCrypto::sign(seed, "payload");
    ASSERT_TRUE(SwarmCrypto::verify(der, "payload", sig));  // 基线先绿

    const detail::ScopedInjection inj{
        falcon::detail::InjectPoint::SwarmVerifyCtxNew};
    EXPECT_FALSE(SwarmCrypto::verify(der, "payload", sig));
}

TEST(SwarmCryptoEdge, RandomBytesOverflowGuardYieldsEmpty) {
    // n > INT_MAX 在分配前即拒（否则 vector 构造本身可能抛 bad_alloc）
    const auto out = SwarmCrypto::random_bytes(
        static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1);
    EXPECT_TRUE(out.empty());
}

// ===========================================================================
// SwarmDispatch：handlers 层 JSON 形状门（-32602 族）直调分派
//
// 传输层（回环 50 用例）已覆盖成功路径与语义错误；本组钉「非法形状」
// 的精确错误消息——HTTP/WS 传输层对非 object params 先拒 -32600，故
// 「params 非 object → -32602」的 dispatch 顶层门只能经直调分派到达
// （既有 ParamsNotObjectYields32600Not32602 钉的是传输层语义）。
// 门序（源码序）：node_id/pubkey/nonce → group_token → agent →
// advertise(object→addr→direct) → challenge_sig。
// ===========================================================================

namespace {

// 合法 step1 身份参数基底（每用例单点破坏一个字段）
nlohmann::json valid_register_params(const SwarmTestNode& node) {
    return node.step1_params("0011223344556677");
}

void expect_shape_error(const SwarmReply& reply, const std::string& message) {
    EXPECT_FALSE(reply.ok());
    EXPECT_EQ(reply.error_code, -32602);  // JSON-RPC invalid params
    EXPECT_EQ(reply.error_message, message);
}

}  // namespace

TEST(SwarmDispatch, NonObjectParamsYields32602) {
    SwarmRendezvousState state(state_config());
    const auto now = SwarmRendezvousState::Clock::now();
    const auto reply = dispatch_swarm_method(
        state, kMethodRegister, nlohmann::json::array({"x"}), now);
    expect_shape_error(reply, "params must be an object");
}

TEST(SwarmDispatch, RegisterOptionalFieldShapeGates) {
    SwarmRendezvousState state(state_config());
    const auto now = SwarmRendezvousState::Clock::now();
    SwarmTestNode node;

    {  // group_token 非字符串
        auto params = valid_register_params(node);
        params[kFieldGroupToken] = 123;
        expect_shape_error(
            dispatch_swarm_method(state, kMethodRegister, params, now),
            "group_token must be a string");
    }
    {  // agent 非字符串
        auto params = valid_register_params(node);
        params[kFieldAgent] = 123;
        expect_shape_error(
            dispatch_swarm_method(state, kMethodRegister, params, now),
            "agent must be a string");
    }
    {  // advertise 非对象
        auto params = valid_register_params(node);
        params[kFieldAdvertise] = "not-an-object";
        expect_shape_error(
            dispatch_swarm_method(state, kMethodRegister, params, now),
            "advertise must be an object");
    }
    {  // advertise.addr 非字符串
        auto params = valid_register_params(node);
        params[kFieldAdvertise] = {{kFieldAddr, 5}, {kFieldDirect, true}};
        expect_shape_error(
            dispatch_swarm_method(state, kMethodRegister, params, now),
            "advertise.addr must be a string");
    }
    {  // advertise.direct 非布尔
        auto params = valid_register_params(node);
        params[kFieldAdvertise] = {{kFieldAddr, "203.0.113.7:4500"},
                                   {kFieldDirect, "yes"}};
        expect_shape_error(
            dispatch_swarm_method(state, kMethodRegister, params, now),
            "advertise.direct must be a boolean");
    }
    {  // advertise 在场但缺 addr → addr 必需（direct 才是可选项）
        auto params = valid_register_params(node);
        params[kFieldAdvertise] = nlohmann::json::object();
        expect_shape_error(
            dispatch_swarm_method(state, kMethodRegister, params, now),
            "advertise.addr must be a string");
    }
    {  // challenge_sig 形态不合法（合法身份 + 空 advertise 之外的形态）
        auto params = valid_register_params(node);
        params["challenge_sig"] = "zz";
        expect_shape_error(
            dispatch_swarm_method(state, kMethodRegister, params, now),
            "challenge_sig must be 128 lowercase hex chars");
    }
}

TEST(SwarmDispatch, HeartbeatQueryUnsubscribeSessionShapeGates) {
    SwarmRendezvousState state(state_config());
    const auto now = SwarmRendezvousState::Clock::now();

    {  // heartbeat：session 非字符串
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodHeartbeat, {{kFieldSession, 5}}, now),
            "session must be a string");
    }
    {  // query：session 合法但缺 sha256 → sha256 门
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodQuery, {{kFieldSession, "s-x"}}, now),
            "sha256 must be 64 lowercase hex chars");
    }
    {  // unsubscribe：session 非字符串
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodUnsubscribe, {{kFieldSession, nlohmann::json::array()}}, now),
            "session must be a string");
    }
}

// ===========================================================================
// SwarmAnnounce：announce/retract 状态面 + 分派全链（§16.2，阶段 1）
//
// dispatch_swarm_method 驱动 handlers 形状/语义门 + state 归并/摘除；
// 签名由 SwarmTestNode 独立重导 payload（防同一 bug 自我印证）。时钟
// 纪律不变：虚拟时刻显式入参，session 过期由 heartbeat 续租精确控制。
// ===========================================================================

namespace {

// 64 字符小写 hex 哈希工厂（首字符变化区分不同资源）
std::string sha64(char variant) {
    std::string s(64, '0');
    s[0] = variant;
    return s;
}

// announce params：session + resources + 对「去 sig 后 params」的签名
// （与 register step2 同法——签名对象不含 sig 自身；§16.2 nonce 位填 session）
nlohmann::json signed_announce(const SwarmTestNode& node,
                               const std::string& session,
                               const nlohmann::json& resources) {
    nlohmann::json params;
    params[kFieldSession] = session;
    params[kFieldResources] = resources;
    params[kFieldSig] = node.sign_announce(session, canonical_json(params));
    return params;
}

// retract params：session + sha256s + 同法签名
nlohmann::json signed_retract(const SwarmTestNode& node,
                              const std::string& session,
                              const nlohmann::json& sha256s) {
    nlohmann::json params;
    params[kFieldSession] = session;
    params[kFieldSha256s] = sha256s;
    params[kFieldSig] = node.sign_retract(session, canonical_json(params));
    return params;
}

// file 条目（name/size/ttl_s 负值 = 缺省不携带）
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

// mirror 条目（etag/ttl_s 负值/空 = 缺省；accept_ranges 恒显式携带）
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

// 指定方法的通知条数
std::size_t count_notifications(
    const std::vector<SwarmRendezvousState::Notification>& ns,
    const char* method) {
    std::size_t n = 0;
    for (const auto& item : ns) {
        if (item.method == method) ++n;
    }
    return n;
}

}  // namespace

TEST(SwarmAnnounce, FileEntryMergeQueryHitAndReAnnounceIdempotent) {
    SwarmRendezvousState state(state_config());
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode node;
    const auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok);
    const std::string sha = sha64('a');

    // file 条目：name+size + ttl_s=0（夹到窗口下限 3600）
    const auto reply = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array(
                            {file_entry(sha, "f.bin", 1024, 0)})),
        t0);
    ASSERT_TRUE(reply.ok());
    EXPECT_EQ(reply.result.at(kFieldAccepted), 1);
    EXPECT_EQ(reply.result.at(kFieldRejected), 0);
    EXPECT_EQ(reply.result.at(kFieldExpiresAt), 3600);  // ttl 0 → 下限

    // onResourceAdded 恰一条：by = 公告方 + 归并后渲染的 node 源
    ASSERT_EQ(reply.notifications.size(), 1u);
    EXPECT_EQ(reply.notifications[0].method, kNotifyResourceAdded);
    EXPECT_EQ(reply.notifications[0].params.at(kFieldBy), node.node_id());
    EXPECT_EQ(reply.notifications[0].params.at(kFieldSha256), sha);
    const auto& added = reply.notifications[0].params.at(kFieldSources);
    ASSERT_EQ(added.size(), 1u);
    EXPECT_EQ(added[0].at(kFieldSourceNodeType), "node");
    EXPECT_EQ(added[0].at(kFieldNodeId), node.node_id());

    // query 命中：name/size/agent 实时渲染（注册态，非公告冻结快照）
    const auto q = dispatch_swarm_method(
        state, kMethodQuery,
        nlohmann::json{{kFieldSession, reg.session}, {kFieldSha256, sha}},
        t0);
    ASSERT_TRUE(q.ok());
    EXPECT_EQ(q.result.at(kFieldName), "f.bin");
    EXPECT_EQ(q.result.at(kFieldSize), 1024);
    ASSERT_EQ(q.result.at(kFieldSources).size(), 1u);
    EXPECT_EQ(q.result.at(kFieldSources)[0].at(kFieldAgent), "falcon-test");

    // 幂等重公告（hash_only）：零通知 + 源不重复 + name 不被空值覆写
    const auto again = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array({file_entry(sha)})),
        t0);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.result.at(kFieldAccepted), 1);
    EXPECT_TRUE(again.notifications.empty());
    const auto q2 = dispatch_swarm_method(
        state, kMethodQuery,
        nlohmann::json{{kFieldSession, reg.session}, {kFieldSha256, sha}},
        t0);
    ASSERT_TRUE(q2.ok());
    EXPECT_EQ(q2.result.at(kFieldSources).size(), 1u);
    EXPECT_EQ(q2.result.at(kFieldName), "f.bin");  // hash_only 不覆写
}

TEST(SwarmAnnounce, MirrorOwnerUrlKeyAndMetadataRefresh) {
    SwarmRendezvousState state(state_config());
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode a, b;
    const auto ra = register_via_state(state, a, "0011223344556677", t0);
    const auto rb = register_via_state(state, b, "1122334455667700", t0);
    ASSERT_TRUE(ra.ok);
    ASSERT_TRUE(rb.ok);
    const std::string sha = sha64('b');
    const std::string url = "http://mirror.example/f.bin";

    // A、B 各公告同一 URL：url 源去重键 = (owner, url) → 两个并列源
    const auto r1 = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(a, ra.session,
                        nlohmann::json::array(
                            {mirror_entry(sha, url, "etag-1")})),
        t0);
    ASSERT_TRUE(r1.ok());
    const auto r2 = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(b, rb.session,
                        nlohmann::json::array(
                            {mirror_entry(sha, url, "etag-2")})),
        t0);
    ASSERT_TRUE(r2.ok());
    // onResourceAdded 只在资源首次出现时发（第二次公告同一资源零通知）
    EXPECT_EQ(r1.notifications.size(), 1u);
    EXPECT_TRUE(r2.notifications.empty());

    // B 重公告刷新自身 url 源元数据（后写胜出），源数不变
    const auto r3 = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(b, rb.session,
                        nlohmann::json::array(
                            {mirror_entry(sha, url, "etag-3", true)})),
        t0);
    ASSERT_TRUE(r3.ok());
    EXPECT_TRUE(r3.notifications.empty());

    auto query_sources = [&] {
        const auto q = dispatch_swarm_method(
            state, kMethodQuery,
            nlohmann::json{{kFieldSession, ra.session}, {kFieldSha256, sha}},
            t0);
        return q.result.at(kFieldSources);
    };
    {
        const auto sources = query_sources();
        ASSERT_EQ(sources.size(), 2u);
        int e1 = 0, e3 = 0;
        for (const auto& s : sources) {
            const std::string etag = s.value(kFieldEtag, std::string());
            if (etag == "etag-1") ++e1;
            if (etag == "etag-3") ++e3;
            EXPECT_EQ(s.at(kFieldUrl), url);
        }
        EXPECT_EQ(e1, 1);  // A 的源未被 B 刷新
        EXPECT_EQ(e3, 1);  // B 的源 etag-2 → etag-3
    }

    // A 再加 file 条目 → 三源并列（node + 2 url）
    const auto r4 = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(a, ra.session,
                        nlohmann::json::array(
                            {file_entry(sha, "f.bin", 1024)})),
        t0);
    ASSERT_TRUE(r4.ok());
    EXPECT_EQ(query_sources().size(), 3u);
}

TEST(SwarmAnnounce, TtlClampedToWindowAndExpiresAtTakesMax) {
    SwarmRendezvousState state(state_config());
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode node;
    const auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok);

    // ttl 0 → clamp 下限 3600
    const auto lo = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array(
                            {file_entry(sha64('c'), "c.bin", 1, 0)})),
        t0);
    ASSERT_TRUE(lo.ok());
    EXPECT_EQ(lo.result.at(kFieldExpiresAt), 3600);

    // ttl 巨大 → clamp 上限 604800
    const auto hi = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array({file_entry(
                            sha64('d'), "d.bin", 1, 999999999)})),
        t0);
    ASSERT_TRUE(hi.ok());
    EXPECT_EQ(hi.result.at(kFieldExpiresAt), 604800);

    // ttl 缺省 → 86400
    const auto def = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array({file_entry(sha64('e'))})),
        t0);
    ASSERT_TRUE(def.ok());
    EXPECT_EQ(def.result.at(kFieldExpiresAt), 86400);

    // expires_at = max(现值, now+ttl)：7200 公告后再 3600 → 取 7200 不缩短
    const std::string sha = sha64('f');
    const auto first = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array(
                            {file_entry(sha, "f.bin", 1, 7200)})),
        t0);
    ASSERT_TRUE(first.ok());
    const auto second = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array(
                            {file_entry(sha, "f.bin", 1, 3600)})),
        t0);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.result.at(kFieldExpiresAt), 7200);
}

TEST(SwarmAnnounce, SemanticRejectsCountedNotWholeRequestFailure) {
    SwarmRendezvousState state(state_config());
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode node;
    const auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok);

    nlohmann::json bad_sha = file_entry(sha64('a'), "x", 1);
    bad_sha[kFieldSha256] = "not-hex!";  // 语义门：非 64 小写 hex
    const nlohmann::json bad_url =
        mirror_entry(sha64('b'), "ftp://mirror.example/f.bin");  // 非 http(s)
    const nlohmann::json good = file_entry(sha64('c'), "c.bin", 42);

    const auto reply = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array({bad_sha, bad_url, good})),
        t0);
    ASSERT_TRUE(reply.ok());  // 语义失败不炸整请求（形状门才 -32602）
    EXPECT_EQ(reply.result.at(kFieldAccepted), 1);
    EXPECT_EQ(reply.result.at(kFieldRejected), 2);

    // 合法条目照常可查；两条被拒条目不可查
    const auto q = dispatch_swarm_method(
        state, kMethodQuery,
        nlohmann::json{{kFieldSession, reg.session}, {kFieldSha256, sha64('c')}},
        t0);
    ASSERT_TRUE(q.ok());
    EXPECT_EQ(q.result.at(kFieldSize), 42);
    for (const char c : {'a', 'b'}) {
        const auto miss = dispatch_swarm_method(
            state, kMethodQuery,
            nlohmann::json{{kFieldSession, reg.session},
                           {kFieldSha256, sha64(c)}},
            t0);
        ASSERT_TRUE(miss.ok());
        EXPECT_TRUE(miss.result.at(kFieldSources).empty()) << c;
    }
}

TEST(SwarmAnnounce, RetractCountsPartialRemovalAndEmptyingDeletes) {
    SwarmRendezvousState state(state_config());
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode a, b;
    const auto ra = register_via_state(state, a, "0011223344556677", t0);
    const auto rb = register_via_state(state, b, "1122334455667700", t0);
    ASSERT_TRUE(ra.ok);
    ASSERT_TRUE(rb.ok);

    // a：h1+h2；b：h1（h1 双源，h2 仅 a）
    const std::string h1 = sha64('1');
    const std::string h2 = sha64('2');
    const std::string h3 = sha64('3');
    ASSERT_TRUE(dispatch_swarm_method(
                    state, kMethodAnnounce,
                    signed_announce(a, ra.session,
                                    nlohmann::json::array({file_entry(h1, "h1", 1),
                                                           file_entry(h2, "h2", 2)})),
                    t0)
                    .ok());
    ASSERT_TRUE(dispatch_swarm_method(
                    state, kMethodAnnounce,
                    signed_announce(b, rb.session,
                                    nlohmann::json::array({file_entry(h1)})),
                    t0)
                    .ok());

    // b 撤 [h1(自有 1 源), h3(未知), h2(在表但非己有 → 两边不计)]
    const auto reply = dispatch_swarm_method(
        state, kMethodRetract,
        signed_retract(b, rb.session, nlohmann::json::array({h1, h3, h2})),
        t0);
    ASSERT_TRUE(reply.ok());
    EXPECT_EQ(reply.result.at(kFieldRemoved), 1);
    EXPECT_EQ(reply.result.at(kFieldUnknown), 1);
    EXPECT_TRUE(reply.notifications.empty());  // h1 仍有 a 的源，未清空

    // a 撤 h1 → 唯一源摘除 → 删表 + onResourceExpired
    const auto fin = dispatch_swarm_method(
        state, kMethodRetract,
        signed_retract(a, ra.session, nlohmann::json::array({h1})), t0);
    ASSERT_TRUE(fin.ok());
    EXPECT_EQ(fin.result.at(kFieldRemoved), 1);
    ASSERT_EQ(fin.notifications.size(), 1u);
    EXPECT_EQ(fin.notifications[0].method, kNotifyResourceExpired);
    EXPECT_EQ(fin.notifications[0].params.at(kFieldSha256), h1);

    // h1 miss；h2 仍命中（a 的源）
    for (const auto& [sha, expect_hit] :
         std::vector<std::pair<std::string, bool>>{{h1, false}, {h2, true}}) {
        const auto q = dispatch_swarm_method(
            state, kMethodQuery,
            nlohmann::json{{kFieldSession, ra.session}, {kFieldSha256, sha}},
            t0);
        ASSERT_TRUE(q.ok());
        EXPECT_EQ(!q.result.at(kFieldSources).empty(), expect_hit) << sha;
    }
}

TEST(SwarmAnnounce, SessionAndSignatureGatesForAnnounceAndRetract) {
    SwarmRendezvousState state(state_config());
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode node, other;
    const auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok);
    const nlohmann::json one_file =
        nlohmann::json::array({file_entry(sha64('a'))});

    // 未知 session → -32003（announce）
    const auto bad_session = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, "s-nonexistent", one_file), t0);
    EXPECT_FALSE(bad_session.ok());
    EXPECT_EQ(bad_session.error_code, -32003);
    EXPECT_EQ(bad_session.error_message, "Unknown or expired session");

    // 垃圾 sig（形状 128 hex 过 handlers 门，内容全零）→ -32005
    nlohmann::json garbage =
        signed_announce(node, reg.session, one_file);
    garbage[kFieldSig] = std::string(128, '0');
    const auto bad_sig =
        dispatch_swarm_method(state, kMethodAnnounce, garbage, t0);
    EXPECT_FALSE(bad_sig.ok());
    EXPECT_EQ(bad_sig.error_code, -32005);
    EXPECT_EQ(bad_sig.error_message, "Announce signature verification failed");

    // 他钥签名（other 签、session 却是 node 的）→ -32005
    const auto foreign_sig = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(other, reg.session, one_file), t0);
    EXPECT_EQ(foreign_sig.error_code, -32005);

    // payload nonce 位错填注册 nonce（而非 session）→ 拒：证明位填 session
    nlohmann::json wrong_slot;
    wrong_slot[kFieldSession] = reg.session;
    wrong_slot[kFieldResources] = one_file;
    wrong_slot[kFieldSig] =
        node.sign_announce("0011223344556677", canonical_json(wrong_slot));
    EXPECT_EQ(dispatch_swarm_method(state, kMethodAnnounce, wrong_slot, t0)
                  .error_code,
              -32005);

    // 对「含 sig 的完整 params」签名 → 拒：证明签名对象是去 sig 形态
    nlohmann::json with_sig;
    with_sig[kFieldSession] = reg.session;
    with_sig[kFieldResources] = one_file;
    with_sig[kFieldSig] = std::string(128, '0');  // 占位后对含 sig 形态签
    with_sig[kFieldSig] =
        node.sign_announce(reg.session, canonical_json(with_sig));
    EXPECT_EQ(dispatch_swarm_method(state, kMethodAnnounce, with_sig, t0)
                  .error_code,
              -32005);

    // retract 同门：未知 session / 垃圾 sig
    const auto r1 = dispatch_swarm_method(
        state, kMethodRetract,
        signed_retract(node, "s-nonexistent",
                       nlohmann::json::array({sha64('a')})),
        t0);
    EXPECT_EQ(r1.error_code, -32003);
    nlohmann::json g2 = signed_retract(
        node, reg.session, nlohmann::json::array({sha64('a')}));
    g2[kFieldSig] = std::string(128, '0');
    const auto r2 = dispatch_swarm_method(state, kMethodRetract, g2, t0);
    EXPECT_EQ(r2.error_code, -32005);
    EXPECT_EQ(r2.error_message, "Retract signature verification failed");
}

TEST(SwarmAnnounce, HeartbeatTimeoutSweepRemovesOwnedSources) {
    SwarmRendezvousState state(state_config());  // heartbeat_timeout 60s
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode a, b, c;
    const auto ra = register_via_state(state, a, "0011223344556677", t0);
    const auto rb = register_via_state(state, b, "1122334455667700", t0);
    // c 晚注册（t0+50 → 到期 t0+110）：两轮 sweep 后的存活查询方
    const auto rc = register_via_state(
        state, c, "2233445566770011", t0 + std::chrono::seconds(50));
    ASSERT_TRUE(ra.ok);
    ASSERT_TRUE(rb.ok);
    ASSERT_TRUE(rc.ok);
    const std::string h1 = sha64('a');

    // a、b 各公告 h1（两个 node 源）
    ASSERT_TRUE(dispatch_swarm_method(
                    state, kMethodAnnounce,
                    signed_announce(a, ra.session,
                                    nlohmann::json::array(
                                        {file_entry(h1, "h1", 1)})),
                    t0)
                    .ok());
    ASSERT_TRUE(dispatch_swarm_method(
                    state, kMethodAnnounce,
                    signed_announce(b, rb.session,
                                    nlohmann::json::array({file_entry(h1)})),
                    t0)
                    .ok());

    // t0+30：b 心跳续命（到期 t0+90）；a 保持 t0+60 到期
    state.heartbeat(rb.session, t0 + std::chrono::seconds(30));

    // t0+61 sweep：仅 a 下线——摘 a 名下源；h1 由 b 的源保活，无 Expired
    const auto out1 = state.sweep(t0 + std::chrono::seconds(61));
    EXPECT_EQ(count_notifications(out1, kNotifyPeerLeft), 1u);
    EXPECT_EQ(count_notifications(out1, kNotifyResourceExpired), 0u);
    {
        const auto q = dispatch_swarm_method(
            state, kMethodQuery,
            nlohmann::json{{kFieldSession, rb.session}, {kFieldSha256, h1}},
            t0 + std::chrono::seconds(61));
        ASSERT_TRUE(q.ok());
        ASSERT_EQ(q.result.at(kFieldSources).size(), 1u);
        EXPECT_EQ(q.result.at(kFieldSources)[0].at(kFieldNodeId), b.node_id());
    }

    // t0+91 sweep：b 也下线 → h1 源清空 → 删表 + Expired + PeerLeft
    const auto out2 = state.sweep(t0 + std::chrono::seconds(91));
    EXPECT_EQ(count_notifications(out2, kNotifyResourceExpired), 1u);
    EXPECT_EQ(count_notifications(out2, kNotifyPeerLeft), 1u);
    const auto q = dispatch_swarm_method(
        state, kMethodQuery,
        nlohmann::json{{kFieldSession, rc.session}, {kFieldSha256, h1}},
        t0 + std::chrono::seconds(91));
    ASSERT_TRUE(q.ok());
    EXPECT_TRUE(q.result.at(kFieldSources).empty());
}

TEST(SwarmAnnounce, TtlSweepDeletesExpiredResourceWithNotification) {
    SwarmRendezvousState state(state_config());
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode node;
    const auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok);
    const std::string sha = sha64('a');

    ASSERT_TRUE(dispatch_swarm_method(
                    state, kMethodAnnounce,
                    signed_announce(node, reg.session,
                                    nlohmann::json::array(
                                        {file_entry(sha, "f", 1, 0)})),
                    t0)
                    .ok());
    // 会话保活：虚拟时钟 30s 步进心跳循环续命到 t0+3650（TTL 观测窗
    // 3601 之后仍有合法查询方）。单次心跳不可能覆盖——会话窗只有
    // timeout 60s，且对已过期会话心跳是 -32003（客户端自愈靠重注册，
    // 状态层无复活语义）。
    for (int t = 30; t <= 3590; t += 30) {
        state.heartbeat(reg.session, t0 + std::chrono::seconds(t));
    }

    // t0+3601：过期未清扫 → query 按 miss 收口（不等 sweep）
    const auto q = dispatch_swarm_method(
        state, kMethodQuery,
        nlohmann::json{{kFieldSession, reg.session}, {kFieldSha256, sha}},
        t0 + std::chrono::seconds(3601));
    ASSERT_TRUE(q.ok());
    EXPECT_TRUE(q.result.at(kFieldSources).empty());

    // sweep 摘除 + onResourceExpired（节点未下线 → 无 PeerLeft）
    const auto out = state.sweep(t0 + std::chrono::seconds(3601));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].method, kNotifyResourceExpired);
    EXPECT_EQ(out[0].params.at(kFieldSha256), sha);

    // 删表后再查仍 miss
    const auto q2 = dispatch_swarm_method(
        state, kMethodQuery,
        nlohmann::json{{kFieldSession, reg.session}, {kFieldSha256, sha}},
        t0 + std::chrono::seconds(3602));
    ASSERT_TRUE(q2.ok());
    EXPECT_TRUE(q2.result.at(kFieldSources).empty());
}

TEST(SwarmAnnounce, SourceQuotaRejectsBatchConservatively) {
    SwarmRendezvousState::Config cfg = state_config();
    cfg.max_sources_per_node = 2;
    SwarmRendezvousState state(cfg);
    const auto t0 = SwarmRendezvousState::Clock::now();
    SwarmTestNode node;
    const auto reg = register_via_state(state, node, "0011223344556677", t0);
    ASSERT_TRUE(reg.ok);

    // 3 条批量 > 上限 2 → 整批 -32002，零部分归并
    const auto over = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array({file_entry(sha64('1')),
                                               file_entry(sha64('2')),
                                               file_entry(sha64('3'))})),
        t0);
    EXPECT_FALSE(over.ok());
    EXPECT_EQ(over.error_code, -32002);
    EXPECT_EQ(over.error_message, "Source quota exceeded for node");
    for (const char c : {'1', '2', '3'}) {
        const auto q = dispatch_swarm_method(
            state, kMethodQuery,
            nlohmann::json{{kFieldSession, reg.session},
                           {kFieldSha256, sha64(c)}},
            t0);
        ASSERT_TRUE(q.ok());
        EXPECT_TRUE(q.result.at(kFieldSources).empty()) << c;
    }

    // 2 条恰满；追加第 3 条即拒
    const auto fill = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array({file_entry(sha64('1')),
                                               file_entry(sha64('2'))})),
        t0);
    EXPECT_TRUE(fill.ok());
    const auto third = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array({file_entry(sha64('4'))})),
        t0);
    EXPECT_EQ(third.error_code, -32002);

    // 幂等重公告既有 2 条：保守多计仍拒（宁可错拒，§16.2 裁决）
    const auto again = dispatch_swarm_method(
        state, kMethodAnnounce,
        signed_announce(node, reg.session,
                        nlohmann::json::array({file_entry(sha64('1')),
                                               file_entry(sha64('2'))})),
        t0);
    EXPECT_EQ(again.error_code, -32002);
}

TEST(SwarmDispatch, AnnounceShapeGates) {
    SwarmRendezvousState state(state_config());
    const auto now = SwarmRendezvousState::Clock::now();
    const std::string sig128(128, '0');

    {  // session 非字符串（缺省）
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldResources, nlohmann::json::array()},
                               {kFieldSig, sig128}},
                now),
            "session must be a string");
    }
    {  // sig 缺失
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldResources, nlohmann::json::array()}},
                now),
            "sig must be 128 lowercase hex chars");
    }
    {  // sig 形态非法
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldResources, nlohmann::json::array()},
                               {kFieldSig, "zz"}},
                now),
            "sig must be 128 lowercase hex chars");
    }
    {  // resources 缺失 / 非数组
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"}, {kFieldSig, sig128}},
                now),
            "resources must be an array");
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, "nope"}},
                now),
            "resources must be an array");
    }
    {  // 条目数 > 256（257 个合法形状条目）
        nlohmann::json many = nlohmann::json::array();
        for (int i = 0; i < 257; ++i) {
            many.push_back(file_entry(sha64('a')));
        }
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, many}},
                now),
            "resources must hold at most 256 entries");
    }
    {  // 条目非对象
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({5})}},
                now),
            "resources entries must be objects");
    }
    {  // kind 缺失 / 非字符串 / 非法枚举
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array(
                                   {nlohmann::json{{kFieldSha256, sha64('a')}}})}},
                now),
            "entry.kind must be a string");
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array(
                                   {nlohmann::json{{kFieldKind, 7},
                                                   {kFieldSha256, sha64('a')}}})}},
                now),
            "entry.kind must be a string");
        auto chunk = file_entry(sha64('a'));
        chunk[kFieldKind] = "chunk";
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({chunk})}},
                now),
            "entry.kind must be \"file\" or \"mirror\"");
    }
    {  // sha256 缺失 / 非字符串
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array(
                                   {nlohmann::json{{kFieldKind, "file"}}})}},
                now),
            "entry.sha256 must be a string");
    }
    {  // 未知字段（file 条目带 mirror 专属字段 / 反之 / 全新键）
        auto file_with_url = file_entry(sha64('a'));
        file_with_url[kFieldUrl] = "http://x/";
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({file_with_url})}},
                now),
            "unknown entry field: url");
        auto mirror_with_name = mirror_entry(sha64('a'), "http://x/");
        mirror_with_name[kFieldName] = "f";
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({mirror_with_name})}},
                now),
            "unknown entry field: name");
        auto bogus = file_entry(sha64('a'));
        bogus["bogus"] = 1;
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({bogus})}},
                now),
            "unknown entry field: bogus");
    }
    {  // ttl_s / size 负数；name / etag / last_modified 非字符串；
       // accept_ranges 非布尔；mirror 缺 url
        auto neg_ttl = file_entry(sha64('a'), "f", 1, -5);
        neg_ttl[kFieldTtlS] = -5;
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({neg_ttl})}},
                now),
            "entry.ttl_s must be a non-negative integer");
        auto neg_size = file_entry(sha64('a'), "f", -7);
        neg_size[kFieldSize] = -7;
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({neg_size})}},
                now),
            "entry.size must be a non-negative integer");
        auto bad_name = file_entry(sha64('a'), "f");
        bad_name[kFieldName] = 9;
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({bad_name})}},
                now),
            "entry.name must be a string");
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array(
                                   {nlohmann::json{{kFieldKind, "mirror"},
                                                   {kFieldSha256, sha64('a')}}})}},
                now),
            "entry.url must be a string");
        auto bad_etag = mirror_entry(sha64('a'), "http://x/");
        bad_etag[kFieldEtag] = 1;
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({bad_etag})}},
                now),
            "entry.etag must be a string");
        auto bad_lm = mirror_entry(sha64('a'), "http://x/");
        bad_lm[kFieldLastModified] = 1;
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({bad_lm})}},
                now),
            "entry.last_modified must be a string");
        auto bad_ar = mirror_entry(sha64('a'), "http://x/");
        bad_ar[kFieldAcceptRanges] = "yes";
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodAnnounce,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldResources, nlohmann::json::array({bad_ar})}},
                now),
            "entry.accept_ranges must be a boolean");
    }
}

TEST(SwarmDispatch, RetractShapeGates) {
    SwarmRendezvousState state(state_config());
    const auto now = SwarmRendezvousState::Clock::now();
    const std::string sig128(128, '0');

    {  // session 非字符串（缺省）
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodRetract,
                nlohmann::json{{kFieldSha256s, nlohmann::json::array()},
                               {kFieldSig, sig128}},
                now),
            "session must be a string");
    }
    {  // sig 形态非法
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodRetract,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSha256s, nlohmann::json::array()},
                               {kFieldSig, "zz"}},
                now),
            "sig must be 128 lowercase hex chars");
    }
    {  // sha256s 缺失 / 非数组
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodRetract,
                nlohmann::json{{kFieldSession, "s-x"}, {kFieldSig, sig128}},
                now),
            "sha256s must be an array");
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodRetract,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldSha256s, "nope"}},
                now),
            "sha256s must be an array");
    }
    {  // 条目非字符串 / 非 64 小写 hex → params 级错误（哈希是标识符）
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodRetract,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldSha256s, nlohmann::json::array({5})}},
                now),
            "sha256s entries must be 64 lowercase hex chars");
        expect_shape_error(
            dispatch_swarm_method(
                state, kMethodRetract,
                nlohmann::json{{kFieldSession, "s-x"},
                               {kFieldSig, sig128},
                               {kFieldSha256s, nlohmann::json::array({"not-hex!"})}},
                now),
            "sha256s entries must be 64 lowercase hex chars");
    }
}

}  // namespace falcon::swarm
