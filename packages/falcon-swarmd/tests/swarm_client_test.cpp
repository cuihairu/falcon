// ============================================================================
// SwarmClient × 回环 server（SwarmClientTest）
//
// 覆盖面（阶段 0 client 全生命周期）：
//   - 两步注册往返拿 session + 心跳续期（wait_until 计数锚，零 sleep）
//   - unsubscribe 退订 → 服务器 peer 归零
//   - 会话过期自愈：测试直调 sweep(now+Δ) 确定性摘除 → client 心跳收
//     -32003 → 自动重注册（虚拟时间支点，零竞速）
//   - key store：PEM 落盘往返（同路径两次加载 node_id 一致）+ POSIX 0600
//   - 注入面：死端口传输失败干净收口 / 错 server 令牌 -32001 /
//     错群组令牌 -32004
//   - 空表查询往返：合法 session → sha256 回显 + sources 空数组
//     （用户裁决「空表往返」——阶段 0 无 announce，资源表恒空）
// ============================================================================

#include "client/swarm_client.hpp"
#include "swarm_server_harness.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <random>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace falcon::swarm;
using falcon::swarm::test::HarnessConfig;
using falcon::swarm::test::SwarmServerHarness;
using falcon::swarm::test::wait_until;

namespace {

// 唯一临时路径（熵源 = random_device 两次采样，CI 时钟粒度粗——c12b2eb 教训）
std::string make_temp_pem_path(const std::string& tag) {
    std::random_device rd;
    std::ostringstream name;
    name << "falcon_swarm_client_" << tag << "_" << rd() << "_" << rd()
         << ".pem";
    return (fs::temp_directory_path() / name.str()).string();
}

SwarmClientConfig make_client_config(std::uint16_t port,
                                     const std::string& server_token = {},
                                     const std::string& group_token = {}) {
    SwarmClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<int>(port);
    cfg.server_token = server_token;
    cfg.group_token = group_token;
    cfg.rpc_timeout_ms = std::chrono::milliseconds(2000);
    cfg.reconnect_delay = std::chrono::milliseconds(100);
    return cfg;
}

}  // namespace

// ===========================================================================
// 注册往返 + 会话观测
// ===========================================================================

TEST(SwarmClientTest, RegisterRoundtripObtainsSession) {
    SwarmServerHarness h;
    ASSERT_TRUE(h.ok());

    auto key = load_or_create_swarm_key(make_temp_pem_path("reg"));
    ASSERT_TRUE(key.valid());

    SwarmClient client(make_client_config(h.port()), key);
    std::string error;
    ASSERT_TRUE(client.start(&error)) << error;

    EXPECT_FALSE(client.session().empty());
    EXPECT_TRUE(h.state().has_session(client.session()));
    EXPECT_EQ(h.state().peer_count(), 1u);

    client.stop();
    EXPECT_TRUE(client.session().empty());  // stop 已退订并清会话
}

TEST(SwarmClientTest, HeartbeatKeepsSessionAlive) {
    SwarmServerHarness h;  // 心跳周期 1s（服务器下发），clamp 下界即 1s
    ASSERT_TRUE(h.ok());

    auto key = load_or_create_swarm_key(make_temp_pem_path("hb"));
    ASSERT_TRUE(key.valid());

    SwarmClient client(make_client_config(h.port()), key);
    std::string error;
    ASSERT_TRUE(client.start(&error)) << error;

    // 至少两跳心跳 = 周期性续期真实发生（会话未过期）
    ASSERT_TRUE(wait_until([&] { return client.heartbeat_count() >= 2; },
                           10000));
    EXPECT_TRUE(h.state().has_session(client.session()));

    client.stop();
}

TEST(SwarmClientTest, UnsubscribeRemovesPeer) {
    SwarmServerHarness h;
    ASSERT_TRUE(h.ok());

    auto key = load_or_create_swarm_key(make_temp_pem_path("unsub"));
    ASSERT_TRUE(key.valid());

    SwarmClient client(make_client_config(h.port()), key);
    std::string error;
    ASSERT_TRUE(client.start(&error)) << error;
    ASSERT_EQ(h.state().peer_count(), 1u);

    client.stop();
    // unsubscribe 走通 → 服务器侧 peer 表归零（一次性 HTTP，无竞速）
    ASSERT_TRUE(wait_until([&] { return h.state().peer_count() == 0; },
                           3000));
}

// ===========================================================================
// 会话过期自愈（-32003 → 重注册）
// ===========================================================================

TEST(SwarmClientTest, ReregisterSelfHealsAfterSessionExpiry) {
    SwarmServerHarness h;  // 心跳周期 1s；timeout 2s（本用例用虚拟时间提前摘）
    ASSERT_TRUE(h.ok());

    auto key = load_or_create_swarm_key(make_temp_pem_path("heal"));
    ASSERT_TRUE(key.valid());

    SwarmClient client(make_client_config(h.port()), key);
    std::string error;
    ASSERT_TRUE(client.start(&error)) << error;

    // 先等首跳心跳（session 已在服务器侧续期过一次）
    ASSERT_TRUE(wait_until([&] { return client.heartbeat_count() >= 1; },
                           10000));

    // 确定性摘除：sweep 显式虚拟时间入参——now + 10min 必越过
    // heartbeat_timeout(2s)，client 会话即时失效
    const auto removed =
        h.state().sweep(std::chrono::steady_clock::now() +
                        std::chrono::minutes(10));
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_FALSE(h.state().has_session(client.session()));

    // 下一跳心跳收 -32003 → 自动重注册换新 session
    ASSERT_TRUE(wait_until([&] { return client.reregister_count() >= 1; },
                           10000));
    EXPECT_TRUE(h.state().has_session(client.session()));
    EXPECT_FALSE(client.session().empty());  // 重注册后新 session 非空

    client.stop();
}

// ===========================================================================
// 空表查询往返（用户裁决：阶段 0 资源表恒空，验证协议形态正确）
// ===========================================================================

TEST(SwarmClientTest, QueryEmptySourcesViaClient) {
    SwarmServerHarness h;
    ASSERT_TRUE(h.ok());

    auto key = load_or_create_swarm_key(make_temp_pem_path("query"));
    ASSERT_TRUE(key.valid());

    SwarmClient client(make_client_config(h.port()), key);
    std::string error;
    ASSERT_TRUE(client.start(&error)) << error;

    // 与服务器编码定案一致的 64 位 hex 摘要（任意形态均可——服务器只回显）
    const std::string digest(64, 'a');
    nlohmann::json result;
    const SwarmError err = client.query(digest, &result);
    ASSERT_TRUE(err.ok()) << err.message;

    EXPECT_EQ(result.value(kFieldSha256, std::string()), digest);
    const auto sources = result.find(kFieldSources);
    ASSERT_NE(sources, result.end());
    EXPECT_TRUE(sources->is_array());
    EXPECT_EQ(sources->size(), 0u);

    client.stop();
}

// ===========================================================================
// 注入面：传输失败 / 错令牌 → 干净错误路径
// ===========================================================================

TEST(SwarmClientTest, TransportFailureClean) {
#ifdef _WIN32
    // Winsock 初始化（测试进程无其他保证；重复初始化引用计数无害）
    WSADATA wsa{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
#endif
    // 造死端口：bind→listen→getsockname→close（连接必被拒）
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr),
                     sizeof(addr)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr),
                            &len), 0);
    const std::uint16_t dead_port = ntohs(addr.sin_port);
#ifdef _WIN32
    ::closesocket(listener);
#else
    ::close(listener);
#endif

    auto key = load_or_create_swarm_key(make_temp_pem_path("dead"));
    ASSERT_TRUE(key.valid());

    SwarmClient client(make_client_config(dead_port), key);
    std::string error;
    EXPECT_FALSE(client.start(&error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(client.session().empty());
    // start 失败路径已收 WS 收订（析构不挂死、无线程残留）
}

TEST(SwarmClientTest, WrongServerTokenRejected) {
    HarnessConfig cfg;
    cfg.server_token = "secret";
    SwarmServerHarness h(cfg);
    ASSERT_TRUE(h.ok());

    auto key = load_or_create_swarm_key(make_temp_pem_path("token"));
    ASSERT_TRUE(key.valid());

    // client 未带令牌 → Bearer 门 401/-32001，start 干净失败
    SwarmClient client(make_client_config(h.port()), key);
    std::string error;
    EXPECT_FALSE(client.start(&error));
    EXPECT_NE(error.find("32001"), std::string::npos) << error;
    EXPECT_EQ(h.state().peer_count(), 0u);
}

TEST(SwarmClientTest, WrongGroupTokenRejected) {
    HarnessConfig cfg;
    cfg.server_token = "secret";
    cfg.group_token = "group-secret";
    SwarmServerHarness h(cfg);
    ASSERT_TRUE(h.ok());

    auto key = load_or_create_swarm_key(make_temp_pem_path("group"));
    ASSERT_TRUE(key.valid());

    // client 带了 Bearer（过传输门）但群组令牌错 → -32004
    SwarmClientConfig cfg_c = make_client_config(h.port(), "secret",
                                                 "wrong-group");
    SwarmClient client(cfg_c, key);
    std::string error;
    EXPECT_FALSE(client.start(&error));
    EXPECT_NE(error.find("32004"), std::string::npos) << error;
    EXPECT_EQ(h.state().peer_count(), 0u);
}

// ===========================================================================
// key store：PEM 往返 + 权限
// ===========================================================================

TEST(SwarmClientTest, KeyPersistLoadRoundtrip) {
    const std::string path = make_temp_pem_path("persist");

    auto first = load_or_create_swarm_key(path);
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(fs::exists(path));

    // 同路径二次加载 = 同一身份（私钥即节点身份，绝不重新生成）
    auto second = load_or_create_swarm_key(path);
    ASSERT_TRUE(second.valid());
    EXPECT_EQ(first.node_id, second.node_id);
    EXPECT_EQ(first.pubkey_hex, second.pubkey_hex);
    EXPECT_EQ(first.private_seed, second.private_seed);

    std::error_code ec;
    fs::remove(path, ec);
}

#ifndef _WIN32
TEST(SwarmClientTest, Permission0600) {
    const std::string path = make_temp_pem_path("perm");

    auto key = load_or_create_swarm_key(path);
    ASSERT_TRUE(key.valid());

    struct stat st{};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u)
        << "PEM 私钥必须收权 owner-only（0600）";

    std::error_code ec;
    fs::remove(path, ec);
}
#endif
