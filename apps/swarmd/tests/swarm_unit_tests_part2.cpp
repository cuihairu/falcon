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
