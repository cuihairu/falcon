// ============================================================================
// swarm_rpc_transport_test —— SwarmRendezvousServer 传输层边界收口
//
// 覆盖面（gcov 单对象口径 2026-09-28，swarm_rpc_server.cpp miss 75 行中
// 可确定性到达的 ~48 行）：
//   HTTP 解析失败族 —— 缺 Content-Length / 头区超限 / 半截头断连 / 垃圾
//   请求行 / 超限声明 body / 短 body 断连 / 头值尾随空白 trim；
//   Bearer 边界 —— "Bearer" 无 token / 非 Bearer scheme；
//   JSON-RPC 信封 —— body 合法 JSON 但非 object；
//   start() 生命周期 —— 重入幂等 / SwarmRdvSocket·SwarmRdvListen 注入
//   （changelog 曾称"socket/listen 注入已被测"实为零消费，本文件是首个
//   消费方）/ 非法 bind host / 端口冲突；
//   WS 传输层 —— 非 /jsonrpc 升级 404 / TEXT·BINARY JSON-RPC 请求-响应
//   （WS 请求路径此前从未被测）/ CLOSE 回显断开 / 非法操作码 CLOSE 1002 /
//   PONG 忽略后会话存活。
//
// 定性跳过（不写用例，证据在案）：
//   L222/L564 —— 请求响应/101 应答 send 失败需 RST 在 recv→send 微秒窗
//   到达，竞速不可锚（批次 P 824/644 同族）；
//   L428 —— accept 失败 ECONNABORTED 竞速；
//   L498/L557 —— gcc 行归属伪影（L498 hits=0 但 L500 hits=83；L557
//   hits=0 但 L560-563 hits=13，语句确已执行）；
//   L633 —— ws_send_frame 表查无防御：调用点全部持有表内 fd，快照-发送
//   窗即 722 同族竞速；
//   L722 —— 广播命中死 fd：RST 到达与会话线程 recv 唤醒摘表之间微秒窗；
//   L749 —— 活 fd 上 getpeername 恒成功；
//   L757-764 —— 服务器 AF_INET-only（peer 恒 v4），IPv6 分支与
//   inet_ntop 失败防御结构不可达；
//   L252-253 —— status_text default：调用点全部传枚举内状态码。
//
// 形制母本：swarm_rdv_loopback_test.cpp（harness 共用）+ daemon 批次 P
// （raw socket 到达传输层分支的先例）。
// ============================================================================

#include "swarm_rdv_harness.hpp"

#include <falcon/detail/injection.hpp>

#include "rdv/swarm_rpc_server.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace {

using namespace falcon::swarm;
using namespace falcon::swarm::test;
namespace detail = ::falcon::detail;

// 裸交换：发送请求后可选半关写端（drop 连接剧本必须半关——否则服务器
// recv 等 body 与客户端 recv 等应答互等，只能靠 SO_RCVTIMEO 兜底）。
// 返回应答原文；空串 = 服务器未发任何字节即断连。
std::string raw_exchange(std::uint16_t port, const std::string& request,
                         bool half_close_after_send) {
    swarm_test_ensure_winsock();
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        swarm_test_socket_close(fd);
        return "";
    }
    set_recv_timeout_ms(fd, 5000);
    if (!send_all(fd, request)) {
        swarm_test_socket_close(fd);
        return "";
    }
#ifdef _WIN32
    if (half_close_after_send) {
        ::shutdown(fd, SD_SEND);
    }
#else
    if (half_close_after_send) {
        ::shutdown(fd, SHUT_WR);
    }
#endif
    std::string raw;
    char buf[4096];
    for (;;) {
        const recv_send_size_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<std::size_t>(n));
    }
    swarm_test_socket_close(fd);
    return raw;
}

}  // namespace

TEST(SwarmRpcTransport, MissingContentLengthPostYieldsParseError) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // POST 无 Content-Length：parse_content_length 缺头 nullopt → body 按
    // 空 → JSON 解析失败 -32700（连接照常应答，不 drop）
    const std::string req =
        "POST /jsonrpc HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{}";
    const std::string raw = raw_exchange(harness.port(), req, false);
    ASSERT_FALSE(raw.empty());
    EXPECT_NE(raw.find("200 OK"), std::string::npos);
    const std::size_t sep = raw.find("\r\n\r\n");
    ASSERT_NE(sep, std::string::npos);
    const RpcOutcome env = parse_rpc_envelope(raw.substr(sep + 4));
    EXPECT_FALSE(env.ok);
    EXPECT_EQ(env.error_code, -32700);
}

TEST(SwarmRpcTransport, HeaderValueWhitespaceTrimmedWithoutSemanticChange) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 头名尾随空白（"x-junk  "）与头值两侧空白都进 trim 循环体；请求
    // 语义不受影响（健康面照常应答）
    const std::string req =
        "GET /v1/health HTTP/1.1\r\n"
        "X-Junk  :   padded-value   \r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    const std::string raw = raw_exchange(harness.port(), req, false);
    ASSERT_FALSE(raw.empty());
    EXPECT_NE(raw.find("200 OK"), std::string::npos);
    const std::size_t sep = raw.find("\r\n\r\n");
    ASSERT_NE(sep, std::string::npos);
    const auto body = nlohmann::json::parse(raw.substr(sep + 4), nullptr, false);
    ASSERT_FALSE(body.is_discarded());
    EXPECT_EQ(body.value("status", ""), "ok");
}

TEST(SwarmRpcTransport, OversizedHeaderRegionDropsConnection) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 头区 > 64KB+4MB 上限且永无 "\r\n\r\n"：recv_into 超限 → 静默断连
    // （无任何应答字节）
    constexpr std::size_t kHeaderLimit = 64 * 1024;
    constexpr std::size_t kBodyLimit = 4 * 1024 * 1024;
    std::string req = "GET /v1/health HTTP/1.1\r\nX-Junk: ";
    req.append(kHeaderLimit + kBodyLimit + 16, 'a');
    req += "\r\n\r\n";
    EXPECT_TRUE(raw_exchange(harness.port(), req, true).empty());
}

TEST(SwarmRpcTransport, PartialHeaderThenCloseDropsConnection) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 半截头（无 "\r\n\r\n"）后写端关闭：头边界循环 recv EOF → 断连
    EXPECT_TRUE(
        raw_exchange(harness.port(),
                     "GET /v1/health HTTP/1.1\r\nHost: x", true)
            .empty());
}

TEST(SwarmRpcTransport, GarbageRequestLineDropsConnection) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 请求行不足三个 token：stream >> 失败 → 断连
    EXPECT_TRUE(raw_exchange(harness.port(), "GARBAGE\r\n\r\n", true).empty());
}

TEST(SwarmRpcTransport, OversizedDeclaredBodyDropsConnection) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 声明 Content-Length 超过 4MB body 上限：头解析后立即断连（无需
    // 真发 body）
    const std::string req =
        "POST /jsonrpc HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 5000000\r\n"
        "Connection: close\r\n\r\n"
        "tiny";
    EXPECT_TRUE(raw_exchange(harness.port(), req, true).empty());
}

TEST(SwarmRpcTransport, ShortDeclaredBodyDropsConnection) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 声明 100 字节 body 实发 5 字节即半关：recv_into EOF → 断连
    const std::string req =
        "POST /jsonrpc HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 100\r\n"
        "Connection: close\r\n\r\n"
        "hello";
    EXPECT_TRUE(raw_exchange(harness.port(), req, true).empty());
}

// ===========================================================================
// Bearer 边界（server_token 非空才进门禁）
// ===========================================================================

TEST(SwarmRpcTransport, BearerSchemeWithoutTokenRejected) {
    HarnessConfig cfg;
    cfg.server_token = "tok";
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    // "Bearer" 恰等于前缀长度（无 token）：value.size() <= kPrefix.size()
    // → 拒绝
    const auto r = http_rpc_call(
        harness.port(), kMethodQuery, nlohmann::json{{kFieldSha256, "ab"}}, 1,
        "",
        {"Authorization: Bearer\r\n"});
    EXPECT_EQ(r.http_status, 401);
    EXPECT_EQ(r.error_code, kErrBearer);
}

TEST(SwarmRpcTransport, NonBearerAuthorizationSchemeRejected) {
    HarnessConfig cfg;
    cfg.server_token = "tok";
    SwarmRendezvousHarness harness(cfg);
    ASSERT_TRUE(harness.ok());

    // 非 Bearer scheme（Basic）：前缀逐字符比对失败 → 拒绝
    const auto r = http_rpc_call(
        harness.port(), kMethodQuery, nlohmann::json{{kFieldSha256, "ab"}}, 1,
        "",
        {"Authorization: Basic dXNlcjpwYXNz\r\n"});
    EXPECT_EQ(r.http_status, 401);
    EXPECT_EQ(r.error_code, kErrBearer);
}

TEST(SwarmRpcTransport, NonObjectJsonRpcEnvelopeRejected) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // body 合法 JSON 但是数组（非 object）→ -32600；与既有"params 数组"
    // 用例（envelope object + params 非 object）可区分
    const std::string req =
        "POST /jsonrpc HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 7\r\n"
        "Connection: close\r\n\r\n"
        "[1,2,3]";
    const std::string raw = raw_exchange(harness.port(), req, false);
    ASSERT_FALSE(raw.empty());
    const std::size_t sep = raw.find("\r\n\r\n");
    ASSERT_NE(sep, std::string::npos);
    const RpcOutcome env = parse_rpc_envelope(raw.substr(sep + 4));
    EXPECT_FALSE(env.ok);
    EXPECT_EQ(env.error_code, -32600);
}

// ===========================================================================
// start() 生命周期与 socket/listen 注入
// ===========================================================================

TEST(SwarmRpcLifecycle, StartIsIdempotentWhenAlreadyRunning) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    const auto port_before = harness.port();
    EXPECT_TRUE(harness.server().start());  // 重入：直接 true，不动线程
    EXPECT_EQ(harness.port(), port_before);

    // 幂等后服务照常
    const auto health = http_request(
        harness.port(), "GET /v1/health HTTP/1.1\r\nHost: x\r\n"
                        "Connection: close\r\n\r\n");
    ASSERT_TRUE(health.has_value());
    EXPECT_EQ(health->status, 200);
}

namespace {

std::unique_ptr<SwarmRendezvousState> make_state() {
    SwarmRendezvousState::Config cfg;
    return std::make_unique<SwarmRendezvousState>(cfg);
}

SwarmRendezvousOptions make_opts(std::uint16_t port) {
    SwarmRendezvousOptions opts;
    opts.host = "127.0.0.1";
    opts.port = port;
    return opts;
}

}  // namespace

TEST(SwarmRpcLifecycle, SocketCreationInjectionFailsStartWithLastError) {
    auto state = make_state();
    SwarmRendezvousServer server(make_opts(0), *state);

    {
        detail::ScopedInjection guard(
            detail::InjectPoint::SwarmRdvSocket);
        EXPECT_FALSE(server.start());
        EXPECT_EQ(server.last_error(), "socket() failed");
    }
    // 注入解除后同一实例重试成功（fd 重建路径；start() 入口清 last_error）
    EXPECT_TRUE(server.start());
    EXPECT_TRUE(server.last_error().empty());
    server.stop();
}

TEST(SwarmRpcLifecycle, ListenInjectionFailsStartWithLastError) {
    auto state = make_state();
    SwarmRendezvousServer server(make_opts(0), *state);

    {
        detail::ScopedInjection guard(
            detail::InjectPoint::SwarmRdvListen);
        EXPECT_FALSE(server.start());
        EXPECT_EQ(server.last_error(), "listen() failed");
    }
    EXPECT_TRUE(server.start());  // 注入解除后重试成功
    server.stop();
}

TEST(SwarmRpcLifecycle, InvalidBindHostFailsStartWithLastError) {
    auto state = make_state();
    SwarmRendezvousOptions opts = make_opts(0);
    opts.host = "not-an-ip";  // inet_pton 失败（无 getaddrinfo 回落）
    SwarmRendezvousServer server(opts, *state);

    EXPECT_FALSE(server.start());
    EXPECT_EQ(server.last_error(), "invalid bind host: not-an-ip");
    server.stop();
}

TEST(SwarmRpcLifecycle, BindConflictFailsStartWithLastError) {
    SwarmRendezvousHarness harness;  // 占住 127.0.0.1:port
    ASSERT_TRUE(harness.ok());

    auto state = make_state();
    SwarmRendezvousServer second(make_opts(harness.port()), *state);
    EXPECT_FALSE(second.start());
    EXPECT_EQ(second.last_error(), "bind() failed");
    second.stop();
}

// ===========================================================================
// WS 传输层（升级路径之外的帧协议面）
// ===========================================================================

TEST(SwarmRpcWs, WsUpgradeOutsideJsonrpcPathReturns404) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    // 升级三件套齐但 path 非 /jsonrpc：handle_websocket 404（HTTP 应答，
    // 非帧）
    const std::string req =
        "GET /other HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    const std::string raw = raw_exchange(harness.port(), req, false);
    ASSERT_FALSE(raw.empty());
    EXPECT_NE(raw.find("404 Not Found"), std::string::npos);
    const std::size_t sep = raw.find("\r\n\r\n");
    ASSERT_NE(sep, std::string::npos);
    const RpcOutcome env = parse_rpc_envelope(raw.substr(sep + 4));
    EXPECT_EQ(env.error_code, -32600);
}

TEST(SwarmRpcWs, WsTextFrameCarriesJsonRpcRequestAndResponse) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    SwarmWsClient ws;
    ASSERT_TRUE(ws.connect(harness.port()));

    // WS TEXT 帧承载 JSON-RPC：未知 session 心跳 → 错误信封原 id 回帧
    // （WS 请求-响应路径此前从未被测——既有用例只测 ping/广播）
    const nlohmann::json body = {
        {"jsonrpc", "2.0"},
        {"id", 7},
        {"method", kMethodHeartbeat},
        {"params", nlohmann::json{{kFieldSession, "s-deadbeef"}}}};
    ws.send_text(body.dump());
    const auto frame = ws.read_frame(5000);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, 0x1);  // TEXT
    const auto resp = nlohmann::json::parse(frame->payload, nullptr, false);
    ASSERT_FALSE(resp.is_discarded());
    EXPECT_EQ(resp.value("id", 0), 7);
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp.at("error").value("code", 0), kErrUnknownSession);
}

TEST(SwarmRpcWs, WsBinaryFrameHandledLikeText) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    SwarmWsClient ws;
    ASSERT_TRUE(ws.connect(harness.port()));

    // BINARY 帧与 TEXT 同分支：未知方法 → -32601 信封
    const nlohmann::json body = {
        {"jsonrpc", "2.0"}, {"id", 8}, {"method", "no.such.method"},
        {"params", nlohmann::json::object()}};
    ws.send_frame(0x2, body.dump());
    const auto frame = ws.read_frame(5000);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, 0x1);  // 应答恒 TEXT
    const auto resp = nlohmann::json::parse(frame->payload, nullptr, false);
    ASSERT_FALSE(resp.is_discarded());
    EXPECT_EQ(resp.value("id", 0), 8);
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp.at("error").value("code", 0), -32601);
}

TEST(SwarmRpcWs, WsCloseFrameEchoedThenDisconnected) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    SwarmWsClient ws;
    ASSERT_TRUE(ws.connect(harness.port()));

    // CLOSE 帧原样回显后服务器断开
    ws.send_frame(0x8, std::string("\x03\xe8", 2));  // 1000
    const auto echo = ws.read_frame(5000);
    ASSERT_TRUE(echo.has_value());
    EXPECT_EQ(echo->opcode, 0x8);
    EXPECT_EQ(echo->payload, std::string("\x03\xe8", 2));
    // 断开：EOF（read_frame nullopt）
    EXPECT_FALSE(ws.read_frame(5000).has_value());
}

TEST(SwarmRpcWs, WsBadOpcodeClosedWith1002) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    SwarmWsClient ws;
    ASSERT_TRUE(ws.connect(harness.port()));

    // 非法操作码 0x3：解析器进错误态 → CLOSE 1002（0x03EA 大端）→ 断开
    ws.send_frame(0x3, "x");
    const auto close = ws.read_frame(5000);
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->opcode, 0x8);
    EXPECT_EQ(close->payload, std::string("\x03\xEA", 2));
    EXPECT_FALSE(ws.read_frame(5000).has_value());
}

TEST(SwarmRpcWs, WsPongFrameIgnoredSessionStaysAlive) {
    SwarmRendezvousHarness harness;
    ASSERT_TRUE(harness.ok());

    SwarmWsClient ws;
    ASSERT_TRUE(ws.connect(harness.port()));

    // PONG → default 分支忽略（无应答帧）；随后的 PING 得到 PONG 即
    // 证明会话存活且 PONG 未产生任何排队帧
    ws.send_frame(0xA, "stale-pong");
    ws.send_ping("live-ping");
    const auto frame = ws.read_frame(5000);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, 0xA);
    EXPECT_EQ(frame->payload, "live-ping");
}
