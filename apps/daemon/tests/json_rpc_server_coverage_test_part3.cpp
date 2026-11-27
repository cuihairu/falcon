// JSON-RPC server coverage tests.
//
// Complements json_rpc_server_test.cpp with exhaustive coverage of the
// HTTP layer (methods / paths / CORS / fragmented requests) and the
// JSON-RPC dispatch layer (every aria2.* method, parameter validation,
// error paths, notifications and system.multicall).

#include "rpc/json_rpc_server.hpp"

#include <csignal>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <falcon/detail/injection.hpp>
#include <falcon/download_engine.hpp>
#include <falcon/download_task.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {
/// 本文件回环基建含「写已断连 socket」剧本（客户端失败收口主动 abort
/// 连接、服务器按剧本 drop 短 body/超限请求、超时清理关 fd 后仍有在途
/// 写）——POSIX 默认对 socket 写的 SIGPIPE 做进程级终止，任一写命中
/// RST 即杀死整个测试进程（RangeLiarServer / TlsTestServer /
/// ProxyTestServer 同族红面先例）。进程级忽略让写失败以 EPIPE 返回
/// 值出现（各 send_all 的 n<=0 分支既有吞错路径正常收口）；Windows 无
/// SIGPIPE 语义无需处理
struct SigpipeImmunity {
    SigpipeImmunity() {
#if !defined(_WIN32)
        ::signal(SIGPIPE, SIG_IGN);
#endif
    }
};

const SigpipeImmunity falcon_test_sigpipe_immunity;

using json = nlohmann::json;

#ifdef _WIN32
using recv_send_size_t = int;

TEST_F(JsonRpcCoverageTest, TruncatedRequestHeadersCloseConnection) {
    start_server();
    ScopedFd fd = connect_loopback(port());
    ASSERT_GE(fd.fd, 0);
    // 半截 HTTP 头后写端关闭：请求永不完整，服务器应直接关闭连接
    const std::string partial = "POST /jsonrpc HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Trunc";
    ASSERT_TRUE(send_all(fd.fd, partial));
#ifdef _WIN32
    ::shutdown(fd.fd, SD_SEND);
#else
    ::shutdown(fd.fd, SHUT_WR);
#endif
    auto resp = recv_all(fd.fd);
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(resp->empty());
}

TEST_F(JsonRpcCoverageTest, UpdateAuthRotatesSecretAtRuntime) {
    start_server();
    // 初始无 secret：匿名请求可用
    EXPECT_TRUE(call("aria2.getGlobalStat", json::array()).contains("result"));

    // 运行时轮换（SIGHUP 热更的编程入口）
    server_->update_auth("rotated-secret", true);

    auto denied = call("aria2.getGlobalStat", json::array());
    ASSERT_TRUE(denied.contains("error")) << denied.dump();
    EXPECT_EQ(denied["error"]["code"], -32001) << denied.dump();

    json with_token = {{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "aria2.getGlobalStat"},
                       {"params", json::array({"token:rotated-secret"})}};
    auto ok = roundtrip(with_token);
    EXPECT_TRUE(ok.contains("result")) << ok.dump();
}


/// changePriority 的 priority 非整数值：非整数字符串 / 浮点数 →
/// "Invalid priority"（-32602），与缺参、bool 形状错误区分
TEST_F(JsonRpcCoverageTest, ChangePriorityInvalidPriorityValue) {
    start_server();
    // 字符串非数字：stoi 抛
    auto parsed = call("aria2.changePriority",
                       json::array({"00000000000000ff", "abc"}));
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    EXPECT_EQ(parsed["error"]["message"], "Invalid priority") << parsed.dump();
    // 浮点数：非 number_integer，get<std::string> 抛
    parsed = call("aria2.changePriority",
                  json::array({"00000000000000ff", 1.5}));
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
}

/// changePriority 越界值：数值可解析但超出 0..3 → 语义错误 code 1
///（"Priority must be 0..3"），与形状错误（-32602）区分
TEST_F(JsonRpcCoverageTest, ChangePriorityOutOfRangeValue) {
    start_server();
    auto parsed = call("aria2.changePriority",
                       json::array({"00000000000000ff", "5"}));
    EXPECT_EQ(parsed["error"]["code"], 1) << parsed.dump();
    EXPECT_EQ(parsed["error"]["message"], "Priority must be 0..3") << parsed.dump();
    // 负数同样越界
    parsed = call("aria2.changePriority",
                  json::array({"00000000000000ff", -1}));
    EXPECT_EQ(parsed["error"]["code"], 1) << parsed.dump();
}

/// changeGlobalOption 的取值抛异常路径：合法键但值无法解析
///（max-concurrent-downloads 非数字字符串）→ "Invalid option value"
TEST_F(JsonRpcCoverageTest, GlobalOptionValueParseExceptionPath) {
    start_server();
    auto parsed = call("aria2.changeGlobalOption",
                       json::array({json{{"max-concurrent-downloads", "abc"}}}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 1) << parsed.dump();
    EXPECT_NE(parsed["error"]["message"].get<std::string>()
                  .find("Invalid option value: max-concurrent-downloads"),
              std::string::npos)
        << parsed.dump();
}

/// HTTP 层解析容错的参数变体（分支覆盖批次 A9）：缺 Content-Length 头
/// → parse_content_length 走「头不存在返回 nullopt」侧（非数字值走
/// catch 侧，由 GarbageContentLengthFallsBackToZero 覆盖），body 按
/// 0 字节 → JSON 解析空串 → -32700
TEST_F(JsonRpcCoverageTest, ContentLengthMissingHeaderParsesAsEmptyBody) {
    start_server();
    auto resp = raw_request(port(),
        "POST /jsonrpc HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    ASSERT_TRUE(resp.has_value());
    auto body = extract_body(*resp);
    ASSERT_TRUE(body.has_value());
    auto parsed = json::parse(*body);
    EXPECT_EQ(parsed["error"]["code"], -32700) << parsed.dump();
}

/// 头值空白三形态（键值两侧空白 / 值全空白 / 值为空）经 trim 归一，
/// 正常 JSON 照常分发——trim 的空串不进 erase 循环与两侧擦除分支
/// 由同一请求一并命中
TEST_F(JsonRpcCoverageTest, HeaderValueWhitespaceVariantsTrimmed) {
    start_server();
    std::string http;
    http += "POST /jsonrpc HTTP/1.1\r\n";
    http += "Host:   127.0.0.1  \r\n";   // 键值两侧空白
    http += "X-Pad:   \r\n";             // 值全空白 → trim 空串
    http += "X-Empty:\r\n";              // 值为空
    http += "Connection: close\r\n";
    http += "Content-Type: application/json\r\n";
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"},
                {"params", json::array()}};
    const std::string body = req.dump();
    http += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    http += body;

    auto resp = raw_request(port(), http);
    ASSERT_TRUE(resp.has_value());
    auto body_out = extract_body(*resp);
    ASSERT_TRUE(body_out.has_value());
    auto parsed = json::parse(*body_out);
    EXPECT_EQ(parsed["result"]["version"], "0.1.0") << parsed.dump();
}

#if defined(FALCON_FAILURE_INJECTION)

// socket() 创建失败：start 立即报 false，对象状态干净可安全 stop
TEST(JsonRpcLifecycleTest, StartFailsWhenSocketCreationInjected) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::RpcServerSocket);
    EXPECT_FALSE(server.start());
    server.stop();
}

// listen() 失败：bind 成功但监听失败，fd 收回、start 报 false
TEST(JsonRpcLifecycleTest, StartFailsWhenListenInjected) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::RpcServerListen);
    EXPECT_FALSE(server.start());
    server.stop();
}

#endif  // FALCON_FAILURE_INJECTION

} // namespace} // namespace
