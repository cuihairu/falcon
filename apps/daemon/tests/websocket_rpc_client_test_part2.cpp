// WebSocketRpcClient × 真实 JsonRpcServer 回环测试。
// 覆盖：WS 握手与 RPC 往返、token 认证、服务器通知接收（事件流驱动桌面端
// 的核心路径）、断线后自动重连、服务器停机时挂起请求失败返回、并发调用
// 的 id 匹配、客户端掩码帧编码。

#include "rpc/json_rpc_server.hpp"
#include "rpc/websocket_rpc_client.hpp"
#include "rpc/websocket_frame.hpp"

#include <falcon/detail/injection.hpp>
#include <falcon/download_engine.hpp>
#include <falcon/download_task.hpp>

#include <csignal>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
using falcon::daemon::rpc::JsonRpcClientConfig;
using falcon::daemon::rpc::JsonRpcError;
using falcon::daemon::rpc::JsonRpcServer;
using falcon::daemon::rpc::JsonRpcServerConfig;
using falcon::daemon::rpc::WebSocketRpcClient;
using falcon::daemon::rpc::WS_OP_BINARY;
using falcon::daemon::rpc::WS_OP_CLOSE;
using falcon::daemon::rpc::WS_OP_PING;
using falcon::daemon::rpc::WS_OP_PONG;
using falcon::daemon::rpc::WS_OP_TEXT;
using falcon::daemon::rpc::WsFrameParser;

// 任务开始后立即完成：产生 onDownloadStart → onDownloadComplete 通知链
class AutoCompleteHandler final : public falcon::IProtocolHandler {
public:
    std::string protocol_name() const override { return "test"; }
    std::vector<std::string> supported_schemes() const override { return {"test"}; }
    bool can_handle(const std::string& url) const override {
        return url.rfind("test://", 0) == 0;
    }

    falcon::FileInfo get_file_info(const std::string& url,
                                   const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "ws-client.bin";
        info.total_size = 100;
        info.supports_resume = false;
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->set_status(falcon::TaskStatus::Downloading);
    }

    void pause(falcon::DownloadTask::Ptr task) override {
        task->set_status(falcon::TaskStatus::Paused);
    }

    void resume(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->set_status(falcon::TaskStatus::Downloading);
    }

    void cancel(falcon::DownloadTask::Ptr task) override {
        task->set_status(falcon::TaskStatus::Cancelled);
    }

    bool supports_resume() const override { return false; }
};

class WsRpcClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_.register_handler(std::make_unique<AutoCompleteHandler>());

        cfg_.listen_port = 0;
        cfg_.secret = "ws-client-secret";
        cfg_.allow_origin_all = false;
        cfg_.bind_address = "127.0.0.1";

        start_server();
    }

    void TearDown() override {
        for (const auto& task : engine_.get_all_tasks()) {
            if (task && !task->is_finished()) task->cancel();
        }
        for (int i = 0; i < 2500; ++i) {
            bool all_done = true;
            for (const auto& task : engine_.get_all_tasks()) {
                if (task && !task->is_finished()) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (server_) server_->stop();
    }

    void start_server() {
        server_ = std::make_unique<JsonRpcServer>(&engine_, cfg_, /*storage=*/nullptr);
        ASSERT_TRUE(server_->start());
        ASSERT_NE(server_->port(), 0);
    }

    JsonRpcClientConfig client_config() const {
        JsonRpcClientConfig cfg;
        cfg.url = "ws://127.0.0.1:" + std::to_string(server_->port()) + "/jsonrpc";
        cfg.secret = cfg_.secret;
        cfg.timeout_seconds = 5;
        return cfg;
    }

    /// 添加一个任务并等 daemon 侧进入完成（保证通知链走完）
    void add_task(const std::string& name) {
        falcon::DownloadOptions options;
        options.output_directory = "/tmp";
        auto task = engine_.add_task("test://" + name, options);
        ASSERT_TRUE(task);
        (void)engine_.start_task(task->id());
    }

    falcon::DownloadEngine engine_;
    JsonRpcServerConfig cfg_;
    std::unique_ptr<JsonRpcServer> server_;
};


TEST_F(WsRpcClientTest, SetUrlParsesIpv6Literal) {
    WebSocketRpcClient client(client_config());
    ASSERT_TRUE(client.connect());

    // 服务器只监听 127.0.0.1：IPv6 回环解析正确但连接必败（解析分支被走通）
    client.set_url("ws://[::1]:1/jsonrpc");
    EXPECT_FALSE(client.connect());

    client.set_url(client_config().url);
    JsonRpcError err;
    auto r = client.call("aria2.tellActive", json::array(), &err);
    ASSERT_TRUE(r) << err.message;
}

// 裸主机 URL（无 path 无端口）：path 默认 /jsonrpc、端口默认 6800
TEST(WsRpcClientEdge, ParseUrlDefaultsForBareHost) {
    WebSocketRpcClient client(raw_client_config(
        "ws://nonexistent-falcon-test.invalid", /*timeout=*/2));
    EXPECT_FALSE(client.connect());  // 默认端口与路径解析不悬挂，按解析失败收口
}

// 自定义 path 原样进请求行
TEST(WsRpcClientEdge, CustomPathCarriedInRequestLine) {
    RawWsServer raw;
    // raw.url() = ws://127.0.0.1:<port>/jsonrpc → 换自定义 path
    const std::string base = raw.url();
    const std::string host_port = base.substr(5, base.find('/', 5) - 5);
    JsonRpcClientConfig cfg;
    cfg.url = "ws://" + host_port + "/myrpc";
    cfg.timeout_seconds = 5;
    WebSocketRpcClient client(cfg);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(raw.request_line(), "GET /myrpc HTTP/1.1");
}

// userinfo 剥离（parse_url 的 '@' 分支）：凭据段不进 host 解析。
// 不剥离时 host = "user:pass@127.0.0.1"（rfind(':') 命中端口前冒号），
// DNS 解析必败 → connect 成功 + 请求行正确即剥离生效的铁证
TEST(WsRpcClientEdge, ParseUrlStripsUserinfoBeforeHostResolution) {
    RawWsServer raw;
    const std::string base = raw.url();
    const std::string host_port = base.substr(5, base.find('/', 5) - 5);
    JsonRpcClientConfig cfg;
    cfg.url = "ws://user:pass@" + host_port + "/jsonrpc";
    cfg.timeout_seconds = 5;
    WebSocketRpcClient client(cfg);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(raw.request_line(), "GET /jsonrpc HTTP/1.1");
}

// 尾斜杠 path 归一化："ws://host:port/" 的 path = "/" → 归一化为 /jsonrpc
// （空 path 方向结构不可达：slash 命中时 substr 至少 "/"，else 分支恒 "/jsonrpc"）
TEST(WsRpcClientEdge, ParseUrlTrailingSlashPathBecomesJsonrpc) {
    RawWsServer raw;
    const std::string base = raw.url();
    const std::string host_port = base.substr(5, base.find('/', 5) - 5);
    JsonRpcClientConfig cfg;
    cfg.url = "ws://" + host_port + "/";
    cfg.timeout_seconds = 5;
    WebSocketRpcClient client(cfg);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(raw.request_line(), "GET /jsonrpc HTTP/1.1");
}

// 客户端帧必须是掩码帧，且服务端解析器能无损还原
TEST(WsClientFrameTest, MaskedFrameParserRoundtrip) {
    const std::string payload = R"({"jsonrpc":"2.0","id":1,"method":"aria2.tellActive"})";
    const auto frame = falcon::daemon::rpc::ws_encode_client_frame(WS_OP_TEXT, payload);

    ASSERT_GE(frame.size(), payload.size() + 6);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[0]) & 0x80u, 0x80u); // FIN
    EXPECT_EQ(static_cast<std::uint8_t>(frame[0]) & 0x0Fu, WS_OP_TEXT);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[1]) & 0x80u, 0x80u); // MASK

    WsFrameParser parser;
    parser.feed(frame.data(), frame.size());
    auto messages = parser.pop_messages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].opcode, WS_OP_TEXT);
    EXPECT_EQ(messages[0].payload, payload);
    EXPECT_FALSE(parser.error());
}

// 126/127 扩展长度的掩码帧
TEST(WsClientFrameTest, MaskedFrameExtendedLengths) {
    for (std::size_t size : {std::size_t{125}, std::size_t{126},
                             std::size_t{65535}, std::size_t{65536}}) {
        const std::string payload(size, 'x');
        const auto frame =
            falcon::daemon::rpc::ws_encode_client_frame(WS_OP_TEXT, payload);
        WsFrameParser parser;
        parser.feed(frame.data(), frame.size());
        auto messages = parser.pop_messages();
        ASSERT_EQ(messages.size(), 1u) << "size=" << size;
        EXPECT_EQ(messages[0].payload, payload) << "size=" << size;
        EXPECT_FALSE(parser.error());
    }
}

// Sec-WebSocket-Key 的 base64：16 随机字节 → 24 字符（含 = 填充）
TEST(WsClientFrameTest, Base64ForHandshakeKey) {
    const std::uint8_t raw[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const auto encoded =
        falcon::daemon::rpc::ws_base64_encode(raw, sizeof(raw));
    EXPECT_EQ(encoded.size(), 24u);
    EXPECT_EQ(encoded.back(), '=');
}


// 服务器推非 JSON 文本帧 / 合法 JSON 非 object 帧：客户端静默忽略
//（不断连、不误认为响应），随后同一连接上的请求-应答照常工作
TEST(WsRpcClientEdge, NonJsonFramesIgnoredAndConnectionStaysUsable) {
    RawWsServer raw;
    raw.on_connected = [](RawWsServer& s, int fd) {
        s.send_server_frame(fd, WS_OP_TEXT, "hello, not json");
        s.send_server_frame(fd, WS_OP_TEXT, "[1,2,3]");
    };
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    raw.on_request = [](const json& req) {
        return json{{"jsonrpc", "2.0"},
                    {"id", req["id"]},
                    {"result", json::array()}}.dump();
    };
    JsonRpcError err;
    auto result = client.call("aria2.tellActive", json::array(), &err);
    ASSERT_TRUE(result) << err.message;
}

#if defined(FALCON_FAILURE_INJECTION)

// 握手请求发送失败：connect 报 false，fd 被回收
TEST(WsRpcClientEdge, ConnectFailsWhenHandshakeSendInjected) {
    RawWsServer raw;
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::WsClientSendFail);
    EXPECT_FALSE(client.connect());
}

// 已连接后请求帧发送失败：挂起请求以 -32000 唤醒并标注失败原因
TEST(WsRpcClientEdge, CallFailsWhenSendFrameInjected) {
    RawWsServer raw;
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::WsClientSendFail);
    JsonRpcError err;
    EXPECT_FALSE(client.call("aria2.getGlobalStat", json::array(), &err));
    EXPECT_EQ(err.code, -32000);
    EXPECT_EQ(err.message, "WebSocket send failed");
}

#endif  // FALCON_FAILURE_INJECTION

} // namespace} // namespace
