
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
static int socket_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
static void ensure_winsock_started() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
using recv_send_size_t = ssize_t;
static int socket_close(int fd) { return ::close(fd); }
static void ensure_winsock_started() {}
#endif

struct ScopedFd {
    int fd = -1;
    ~ScopedFd() {
        if (fd >= 0) socket_close(fd);
    }
    ScopedFd() = default;
    explicit ScopedFd(int f) : fd(f) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

static bool send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        // POSIX send 的长度参数是 size_t，Windows 是 int
        const auto chunk_len = static_cast<
#ifdef _WIN32
            int
#else
            std::size_t
#endif
            >(data.size() - off);
        recv_send_size_t n = ::send(fd, data.data() + off, chunk_len, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// Read from the socket until the peer closes (with a safety timeout).
static std::optional<std::string> recv_all(int fd) {
    std::string buf;
    char tmp[4096];
    while (true) {
        recv_send_size_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) break;
        if (n < 0) return std::nullopt;
        buf.append(tmp, tmp + n);
        if (buf.size() > 4 * 1024 * 1024) return std::nullopt;
    }
    return buf;
}

static std::optional<std::string> extract_body(const std::string& http) {
    const std::string sep = "\r\n\r\n";
    auto pos = http.find(sep);
    if (pos == std::string::npos) return std::nullopt;
    return http.substr(pos + sep.size());
}

static ScopedFd connect_loopback(uint16_t port) {
    ensure_winsock_started();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    for (int attempt = 0; attempt < 100; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return ScopedFd{};

        // Safety timeout so a stuck server can never hang a test.
#ifdef _WIN32
        DWORD rcv_timeout = 5000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&rcv_timeout), sizeof(rcv_timeout));
#else
        timeval tv{};
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return ScopedFd{fd};
        }
        socket_close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ScopedFd{};
}

static std::string make_http_post(const std::string& body, const std::string& path = "/jsonrpc") {
    std::string http;
    http += "POST " + path + " HTTP/1.1\r\n";
    http += "Host: 127.0.0.1\r\n";
    http += "Content-Type: application/json\r\n";
    http += "Connection: close\r\n";
    http += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    http += body;
    return http;
}

static std::optional<std::string> raw_request(uint16_t port, const std::string& raw) {
    ScopedFd fd = connect_loopback(port);
    if (fd.fd < 0) return std::nullopt;
    if (!raw.empty() && !send_all(fd.fd, raw)) return std::nullopt;
    return recv_all(fd.fd);
}

static std::string to_lowercase(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

static std::string gid_of(falcon::TaskId id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    return std::string(buf);
}

// A fully deterministic in-process protocol handler. The real builtin
// handlers are irrelevant for RPC-server coverage, and using a local stub
// keeps the tests offline and reproducible.
class StubHandler final : public falcon::IProtocolHandler {
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
        info.filename = "stub.bin";
        info.total_size = 100;
        info.supports_resume = true;
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->update_progress(100, 100, 0);
        task->set_status(falcon::TaskStatus::Completed);
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

    bool supports_resume() const override { return true; }
};

class JsonRpcCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_.register_handler(std::make_unique<StubHandler>());
        cfg_.listen_port = 0;
        cfg_.secret.clear();
        cfg_.allow_origin_all = false;
        cfg_.bind_address = "127.0.0.1";
    }

    void TearDown() override {
        if (server_) server_->stop();
    }

    void start_server() {
        server_ = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(&engine_, cfg_);
        ASSERT_TRUE(server_->start());
        ASSERT_NE(server_->port(), 0);
    }

    uint16_t port() const { return server_->port(); }

    // Send a JSON-RPC request over HTTP and parse the JSON body of the reply.
    json call(const std::string& method, json params) {
        json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", std::move(params)}};
        return roundtrip(req);
    }

    json roundtrip(const json& req) {
        auto full = raw_request(port(), make_http_post(req.dump()));
        EXPECT_TRUE(full.has_value());
        if (!full.has_value()) return json();
        auto body = extract_body(*full);
        EXPECT_TRUE(body.has_value());
        if (!body.has_value()) return json();
        return json::parse(*body);
    }

    falcon::DownloadEngine engine_;
    falcon::daemon::rpc::JsonRpcServerConfig cfg_;
    std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> server_;
};

// ===========================================================================
// Start / stop lifecycle
// ===========================================================================

TEST(JsonRpcLifecycleTest, StartWithNullEngineFails) {
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    falcon::daemon::rpc::JsonRpcServer server(nullptr, cfg);
    EXPECT_FALSE(server.start());
    server.stop();
}

TEST(JsonRpcLifecycleTest, StartTwiceReturnsTrue) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    ASSERT_TRUE(server.start());
    const uint16_t first_port = server.port();
    EXPECT_TRUE(server.start());
    EXPECT_EQ(server.port(), first_port);
    server.stop();
}

TEST(JsonRpcLifecycleTest, StartWithInvalidBindAddressFails) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    cfg.bind_address = "not-an-ip-address";
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    EXPECT_FALSE(server.start());
    server.stop();
}

TEST(JsonRpcLifecycleTest, StartWithOutOfRangeAddressFails) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    cfg.bind_address = "999.999.999.999";
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    EXPECT_FALSE(server.start());
    server.stop();
}

TEST(JsonRpcLifecycleTest, StopWithoutStartIsSafe) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    server.stop();
    server.stop();
}

TEST(JsonRpcLifecycleTest, ServerIsReusableAfterStop) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    ASSERT_TRUE(server.start());
    server.stop();
    EXPECT_TRUE(server.start());

    auto resp = raw_request(server.port(), make_http_post(
        json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "aria2.getVersion"}, {"params", json::array()}}.dump()));
    ASSERT_TRUE(resp.has_value());
    auto body = extract_body(*resp);
    ASSERT_TRUE(body.has_value());
    auto parsed = json::parse(*body);
    EXPECT_EQ(parsed["id"], 7);
    EXPECT_EQ(parsed["result"]["version"], "0.1.0");
    server.stop();
}

// ===========================================================================
// HTTP protocol layer
// ===========================================================================

TEST_F(JsonRpcCoverageTest, OptionsRequestReturns204) {
    start_server();
    auto resp = raw_request(port(), "OPTIONS /jsonrpc HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(resp.has_value());
    EXPECT_NE(resp->find("HTTP/1.1 204"), std::string::npos) << *resp;
    EXPECT_EQ(resp->find("method not allowed"), std::string::npos);
}

TEST_F(JsonRpcCoverageTest, GetRequestReturns405) {
    start_server();
    auto resp = raw_request(port(), "GET /jsonrpc HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(resp.has_value());
    EXPECT_NE(resp->find("HTTP/1.1 405"), std::string::npos) << *resp;
    EXPECT_NE(resp->find("method not allowed"), std::string::npos);
}

TEST_F(JsonRpcCoverageTest, PutRequestReturns405) {
    start_server();
    auto resp = raw_request(port(), "PUT /jsonrpc HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    ASSERT_TRUE(resp.has_value());
    EXPECT_NE(resp->find("HTTP/1.1 405"), std::string::npos) << *resp;
}

TEST_F(JsonRpcCoverageTest, UnknownPathReturns404) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto resp = raw_request(port(), make_http_post(req.dump(), "/nope"));
    ASSERT_TRUE(resp.has_value());
    EXPECT_NE(resp->find("HTTP/1.1 404"), std::string::npos) << *resp;
    EXPECT_NE(resp->find("not found"), std::string::npos);
}

TEST_F(JsonRpcCoverageTest, PostRootPathIsAccepted) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto resp = raw_request(port(), make_http_post(req.dump(), "/"));
    ASSERT_TRUE(resp.has_value());
    auto body = extract_body(*resp);
    ASSERT_TRUE(body.has_value());
    auto parsed = json::parse(*body);
    EXPECT_EQ(parsed["result"]["version"], "0.1.0");
}

TEST_F(JsonRpcCoverageTest, CorsHeadersWhenAllowOriginAll) {
    cfg_.allow_origin_all = true;
    start_server();

    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto resp = raw_request(port(), make_http_post(req.dump()));
    ASSERT_TRUE(resp.has_value());
    EXPECT_NE(resp->find("Access-Control-Allow-Origin: *"), std::string::npos) << *resp;

    auto preflight = raw_request(port(), "OPTIONS /jsonrpc HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(preflight.has_value());
    EXPECT_NE(preflight->find("HTTP/1.1 204"), std::string::npos) << *preflight;
    EXPECT_NE(preflight->find("Access-Control-Allow-Origin: *"), std::string::npos) << *preflight;
}

TEST_F(JsonRpcCoverageTest, NoCorsHeadersByDefault) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto resp = raw_request(port(), make_http_post(req.dump()));
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(to_lowercase(*resp).find("access-control-allow-origin"), std::string::npos) << *resp;
}

TEST_F(JsonRpcCoverageTest, HeaderNamesAreCaseInsensitive) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    const std::string body = req.dump();
    std::string http;
    http += "POST /jsonrpc HTTP/1.1\r\n";
    http += "HOST: 127.0.0.1\r\n";
    http += "CONTENT-TYPE: application/json\r\n";
    http += "CONNECTION: close\r\n";
    http += "CONTENT-LENGTH: " + std::to_string(body.size()) + "\r\n\r\n";
    http += body;

    auto resp = raw_request(port(), http);
    ASSERT_TRUE(resp.has_value());
    auto body_out = extract_body(*resp);
    ASSERT_TRUE(body_out.has_value());
    auto parsed = json::parse(*body_out);
    EXPECT_EQ(parsed["result"]["version"], "0.1.0");
}

TEST_F(JsonRpcCoverageTest, FragmentedRequestIsReassembled) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    const std::string http = make_http_post(req.dump());

    ScopedFd fd = connect_loopback(port());
    ASSERT_GE(fd.fd, 0);
    for (char c : http) {
        ASSERT_TRUE(send_all(fd.fd, std::string(1, c)));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto resp = recv_all(fd.fd);
    ASSERT_TRUE(resp.has_value());
    auto body = extract_body(*resp);
    ASSERT_TRUE(body.has_value());
    auto parsed = json::parse(*body);
    EXPECT_EQ(parsed["result"]["version"], "0.1.0");
}

TEST_F(JsonRpcCoverageTest, MalformedRequestLineGetsNoResponse) {
    start_server();
    auto resp = raw_request(port(), "\r\n\r\n");
    if (resp.has_value()) {
        EXPECT_EQ(resp->size(), std::size_t{0});
    }
}

TEST_F(JsonRpcCoverageTest, RequestLineWithoutPathGetsNoResponse) {
    start_server();
    auto resp = raw_request(port(), "GARBAGE\r\nHost: x\r\n\r\n");
    if (resp.has_value()) {
        EXPECT_EQ(resp->size(), std::size_t{0});
    }
}

TEST_F(JsonRpcCoverageTest, ImmediateDisconnectDoesNotBreakServer) {
    start_server();
    {
        ScopedFd fd = connect_loopback(port());
        ASSERT_GE(fd.fd, 0);
    }
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto resp = raw_request(port(), make_http_post(req.dump()));
    ASSERT_TRUE(resp.has_value());
    auto body = extract_body(*resp);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(json::parse(*body)["result"]["version"], "0.1.0");
}

TEST_F(JsonRpcCoverageTest, TruncatedBodyGetsNoResponse) {
    start_server();
    std::string http;
    http += "POST /jsonrpc HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n";
    http += "short";
    auto resp = raw_request(port(), http);
    if (resp.has_value()) {
        EXPECT_EQ(resp->size(), std::size_t{0});
    }
}

TEST_F(JsonRpcCoverageTest, GarbageContentLengthFallsBackToZero) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    const std::string body = req.dump();
    std::string http;
    http += "POST /jsonrpc HTTP/1.1\r\nHost: x\r\n";
    http += "Content-Length: not-a-number\r\n\r\n";
    http += body;

    auto resp = raw_request(port(), http);
    ASSERT_TRUE(resp.has_value());
    auto body_out = extract_body(*resp);
    ASSERT_TRUE(body_out.has_value());
    auto parsed = json::parse(*body_out);
    // Body was ignored (content length unparseable -> 0) -> empty body -> parse error.
    EXPECT_EQ(parsed["error"]["code"], -32700);
}

TEST_F(JsonRpcCoverageTest, ResponseIncludesServerHeaders) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto resp = raw_request(port(), make_http_post(req.dump()));
    ASSERT_TRUE(resp.has_value());
    EXPECT_NE(resp->find("Server: falcon-daemon"), std::string::npos) << *resp;
    EXPECT_NE(resp->find("Connection: close"), std::string::npos) << *resp;
    EXPECT_NE(resp->find("Content-Length: "), std::string::npos) << *resp;
}

// ===========================================================================
// JSON-RPC envelope validation
// ===========================================================================

TEST_F(JsonRpcCoverageTest, InvalidJsonIsParseError) {
    start_server();
    auto resp = raw_request(port(), make_http_post("{this is not json"));
    ASSERT_TRUE(resp.has_value());
    auto body = extract_body(*resp);
    ASSERT_TRUE(body.has_value());
    auto parsed = json::parse(*body);
    EXPECT_EQ(parsed["error"]["code"], -32700);
}

TEST_F(JsonRpcCoverageTest, EmptyBodyIsParseError) {
    start_server();
    auto resp = raw_request(port(), make_http_post(""));
    ASSERT_TRUE(resp.has_value());
    auto body = extract_body(*resp);
    ASSERT_TRUE(body.has_value());
    auto parsed = json::parse(*body);
    EXPECT_EQ(parsed["error"]["code"], -32700);
}

TEST_F(JsonRpcCoverageTest, ArrayRequestIsInvalid) {
    start_server();
    auto parsed = roundtrip(json::array({json{"jsonrpc", "2.0"}}));
    EXPECT_EQ(parsed["error"]["code"], -32600);
}

TEST_F(JsonRpcCoverageTest, StringRequestIsInvalid) {
    start_server();
    auto parsed = roundtrip(json("hello"));
    EXPECT_EQ(parsed["error"]["code"], -32600);
}

TEST_F(JsonRpcCoverageTest, NumberRequestIsInvalid) {
    start_server();
    auto parsed = roundtrip(json(42));
    EXPECT_EQ(parsed["error"]["code"], -32600);
}

TEST_F(JsonRpcCoverageTest, MissingMethodIsInvalidRequest) {
    start_server();
    auto parsed = roundtrip(json{{"jsonrpc", "2.0"}, {"id", 1}, {"params", json::array()}});
    EXPECT_EQ(parsed["error"]["code"], -32600);
}

TEST_F(JsonRpcCoverageTest, EmptyMethodIsInvalidRequest) {
    start_server();
    auto parsed = roundtrip(
        json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", ""}, {"params", json::array()}});
    EXPECT_EQ(parsed["error"]["code"], -32600);
}

TEST_F(JsonRpcCoverageTest, StringParamsIsInvalidRequest) {
    start_server();
    auto parsed = roundtrip(
        json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}, {"params", "oops"}});
    EXPECT_EQ(parsed["error"]["code"], -32600);
}

TEST_F(JsonRpcCoverageTest, NumberParamsIsInvalidRequest) {
    start_server();
    auto parsed = roundtrip(
        json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}, {"params", 5}});
    EXPECT_EQ(parsed["error"]["code"], -32600);
}

TEST_F(JsonRpcCoverageTest, MissingParamsDefaultsToArray) {
    start_server();
    auto parsed = roundtrip(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.getVersion"}});
    EXPECT_EQ(parsed["result"]["version"], "0.1.0");
}

TEST_F(JsonRpcCoverageTest, UnknownMethodIsMethodNotFound) {
    start_server();
    auto parsed = call("no.such.method", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32601);
    EXPECT_NE(parsed["error"]["message"].get<std::string>().find("Method not found"), std::string::npos);
}

TEST_F(JsonRpcCoverageTest, NotificationHasNullId) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto parsed = roundtrip(req);
    EXPECT_TRUE(parsed.contains("result"));
    EXPECT_TRUE(parsed["id"].is_null());
}

TEST_F(JsonRpcCoverageTest, StringIdIsEchoed) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", "my-id"}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto parsed = roundtrip(req);
    EXPECT_EQ(parsed["id"], "my-id");
}

TEST_F(JsonRpcCoverageTest, NumericIdIsEchoed) {
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 42}, {"method", "aria2.getVersion"}, {"params", json::array()}};
    auto parsed = roundtrip(req);
    EXPECT_EQ(parsed["id"], 42);
}

// ===========================================================================
// system.* methods
// ===========================================================================

TEST_F(JsonRpcCoverageTest, ListMethodsContainsAllAria2Methods) {
    start_server();
    auto parsed = call("system.listMethods", json::array());
    ASSERT_TRUE(parsed.contains("result"));
    auto& arr = parsed["result"];
    ASSERT_TRUE(arr.is_array());

    for (const char* name : {"aria2.addUri",
                             "aria2.pause",
                             "aria2.pauseAll",
                             "aria2.forcePause",
                             "aria2.unpause",
                             "aria2.unpauseAll",
                             "aria2.remove",
                             "aria2.forceRemove",
                             "aria2.forceShutdown",
                             "aria2.tellStatus",
                             "aria2.tellActive",
                             "aria2.tellWaiting",
                             "aria2.tellStopped",
                             "aria2.getFiles",
                             "aria2.getUris",
                             "aria2.getOption",
                             "aria2.getGlobalOption",
                             "aria2.changeGlobalOption",
                             "aria2.getGlobalStat",
                             "aria2.getVersion",
                             "aria2.getSessionInfo",
                             "aria2.saveSession",
                             "aria2.purgeDownloadResult",
                             "aria2.removeDownloadResult",
                             "system.listMethods",
                             "system.multicall",
                             "falcon.stopSeeding",
                             "falcon.swarm.status",
                             "falcon.swarm.setShare"}) {
        bool found = false;
        for (const auto& m : arr) {
            if (m.is_string() && m.get<std::string>() == name) found = true;
        }
        EXPECT_TRUE(found) << "missing " << name;
    }
}

TEST_F(JsonRpcCoverageTest, GetVersion) {
    start_server();
    auto parsed = call("aria2.getVersion", json::array());
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_EQ(parsed["result"]["version"], "0.1.0");
    ASSERT_TRUE(parsed["result"]["enabledFeatures"].is_array());
    bool has_jsonrpc = false;
    for (const auto& f : parsed["result"]["enabledFeatures"]) {
        if (f == "jsonrpc") has_jsonrpc = true;
    }
    EXPECT_TRUE(has_jsonrpc);
}

TEST_F(JsonRpcCoverageTest, MulticallInvalidParams) {
    start_server();
    // params not an array of arrays
    auto parsed = call("system.multicall", json::array({"not-an-array"}));
    EXPECT_EQ(parsed["error"]["code"], -32602);

    // empty params
    parsed = call("system.multicall", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602);

    // object params rejected by the dispatcher
    parsed = call("system.multicall", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, MulticallEmptyCallList) {
    start_server();
    auto parsed = call("system.multicall", json::array({json::array()}));
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_TRUE(parsed["result"].is_array());
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});
}

TEST_F(JsonRpcCoverageTest, MulticallMixedResults) {
    start_server();
    json calls = json::array({
        json{{"methodName", "aria2.getVersion"}, {"params", json::array()}},
        json{{"methodName", "system.listMethods"}, {"params", json::array()}},
        json{{"methodName", "no.such.method"}, {"params", json::array()}},
        json{{"not-a-call", true}},
        json{{"methodName", "aria2.pause"}, {"params", json::array({123})}},
    });
    auto parsed = call("system.multicall", json::array({calls}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    auto& results = parsed["result"];
    ASSERT_TRUE(results.is_array());
    ASSERT_EQ(results.size(), std::size_t{5});

    // Success results are wrapped in a one-element array (xmlrpc multicall convention).
    ASSERT_TRUE(results[0].is_array());
    EXPECT_EQ(results[0][0]["version"], "0.1.0");
    ASSERT_TRUE(results[1].is_array());
    EXPECT_GT(results[1][0].size(), std::size_t{0});

    // Errors are returned as {"code", "message"} objects.
    EXPECT_EQ(results[2]["code"], -32601);
    EXPECT_EQ(results[3]["code"], -32600);
    EXPECT_EQ(results[4]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, MulticallInnerTokenIsStripped) {
    cfg_.secret = "topsecret";
    start_server();

    json calls = json::array({
        json{{"methodName", "aria2.getVersion"}, {"params", json::array({"token:topsecret"})}},
        json{{"methodName", "aria2.getVersion"}, {"params", json::array()}},
    });
    auto parsed = call("system.multicall",
                       json::array({std::string("token:topsecret"), calls}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});
    EXPECT_EQ(parsed["result"][0][0]["version"], "0.1.0");
    EXPECT_EQ(parsed["result"][1][0]["version"], "0.1.0");
}

// ===========================================================================
// Token authentication
// ===========================================================================

TEST_F(JsonRpcCoverageTest, SecretWrongTokenIsUnauthorized) {
    cfg_.secret = "s3cr3t";
    start_server();

    auto parsed = call("system.listMethods", json::array({"token:wrong"}));
    EXPECT_EQ(parsed["error"]["code"], -32001);

    // First param must be a string when a secret is configured.
    parsed = call("system.listMethods", json::array({123}));
    EXPECT_EQ(parsed["error"]["code"], -32001);

    // Empty params with a secret configured is unauthorized as well.
    parsed = call("system.listMethods", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32001);

    // Correct token works.
    parsed = call("system.listMethods", json::array({"token:s3cr3t"}));
    EXPECT_TRUE(parsed.contains("result"));
}

TEST_F(JsonRpcCoverageTest, SecretWithObjectParamsIsUnauthorized) {
    cfg_.secret = "s3cr3t";
    start_server();
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "system.listMethods"}, {"params", json::object()}};
    auto parsed = roundtrip(req);
    EXPECT_EQ(parsed["error"]["code"], -32001);
}

// ===========================================================================
// aria2.getGlobalStat
// ===========================================================================

TEST_F(JsonRpcCoverageTest, GlobalStatEmptyEngine) {
    start_server();
    auto parsed = call("aria2.getGlobalStat", json::array());
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_EQ(parsed["result"]["numActive"], "0");
    EXPECT_EQ(parsed["result"]["numWaiting"], "0");
    EXPECT_EQ(parsed["result"]["numStopped"], "0");
    EXPECT_EQ(parsed["result"]["numStoppedTotal"], "0");
    EXPECT_EQ(parsed["result"]["downloadSpeed"], "0");
    EXPECT_EQ(parsed["result"]["uploadSpeed"], "0");
}

TEST_F(JsonRpcCoverageTest, GlobalStatCountsTasksByStatus) {
    start_server();

    // Tasks are added directly to the engine (never started) so their
    // statuses are fully deterministic.
    auto t1 = engine_.add_task("test://local/a");  // stays Pending
    auto t2 = engine_.add_task("test://local/b");  // will be paused
    auto t3 = engine_.add_task("test://local/c");  // will be cancelled
    auto t4 = engine_.add_task("test://local/d");  // forced active
    ASSERT_TRUE(t1 && t2 && t3 && t4);
    ASSERT_TRUE(engine_.pause_task(t2->id()));
    ASSERT_TRUE(engine_.cancel_task(t3->id()));
    t4->set_status(falcon::TaskStatus::Downloading);

    auto parsed = call("aria2.getGlobalStat", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"]["numActive"], "1");
    EXPECT_EQ(parsed["result"]["numWaiting"], "2");
    EXPECT_EQ(parsed["result"]["numStopped"], "1");
    EXPECT_EQ(parsed["result"]["numStoppedTotal"], "1");
}

// ===========================================================================
// aria2.addUri
// ===========================================================================

TEST_F(JsonRpcCoverageTest, AddUriInvalidParams) {
    start_server();

    // Object params rejected.
    auto parsed = call("aria2.addUri", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602);

    // Empty params.
    parsed = call("aria2.addUri", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602);

    // First param must be an array of URIs.
    parsed = call("aria2.addUri", json::array({"http://x"}));
    EXPECT_EQ(parsed["error"]["code"], -32602);

    // Empty URI list.
    parsed = call("aria2.addUri", json::array({json::array()}));
    EXPECT_EQ(parsed["error"]["code"], -32602);

    // URI list with non-string entry -> engine-side type error surfaces as
    // an internal error (dispatch threw, not the JSON parser).
    parsed = call("aria2.addUri", json::array({json::array({42})}));
    EXPECT_EQ(parsed["error"]["code"], -32603);
}

TEST_F(JsonRpcCoverageTest, AddUriUnsupportedProtocol) {
    start_server();
    auto parsed = call("aria2.addUri", json::array({json::array({"unsupported://nowhere/file"})}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    // The engine throws for unsupported protocols; dispatch exceptions are
    // reported as internal errors, not parse errors.
    EXPECT_EQ(parsed["error"]["code"], -32603);
}


// parse_numeric_option 的 stoull 溢出捕获分支：超界字符串 → nullopt →
// -32602。
TEST_F(JsonRpcCoverageTest, AddUriNumericOptionOverflowStringRejected) {
    start_server();
    json opts = {{"max-tries", "99999999999999999999999999999999"}};
    auto parsed = call("aria2.addUri",
                       json::array({json::array({"test://local/overflow.bin"}), opts}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], -32602);
    EXPECT_NE(parsed["error"]["message"].get<std::string>().find("max-tries"),
              std::string::npos);
}

// falcon.stopSeeding 的无效参数形状 → -32602。
TEST_F(JsonRpcCoverageTest, StopSeedingInvalidParamsReturns32602) {
    start_server();
    for (const auto& params : {json::object(), json::array(), json::array({42})}) {
        auto parsed = call("falcon.stopSeeding", params);
        ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
        EXPECT_EQ(parsed["error"]["code"], -32602);
    }
}

// falcon.stopSeeding：gid 不存在 → code 2（任务未找到）。
TEST_F(JsonRpcCoverageTest, StopSeedingUnknownGidReturnsNotFound) {
    start_server();
    auto parsed = call("falcon.stopSeeding", json::array({"0000000000000abc"}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 2);
}

// falcon.stopSeeding：非做种任务 → code 1（任务不在做种）。
TEST_F(JsonRpcCoverageTest, StopSeedingNonSeedingTaskReturnsError) {
    start_server();
    auto parsed = call("aria2.addUri",
                       json::array({json::array({"test://local/nonseed.bin"})}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    const std::string gid = parsed["result"].get<std::string>();
    auto stop = call("falcon.stopSeeding", json::array({gid}));
    ASSERT_TRUE(stop.contains("error")) << stop.dump();
    EXPECT_EQ(stop["error"]["code"], 1);
}

// aria2.remove 的 gid 不存在 → code 2（任务未找到）。
TEST_F(JsonRpcCoverageTest, RemoveUnknownGidReturnsNotFound) {
    start_server();
    auto parsed = call("aria2.remove", json::array({"0000000000000def"}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 2);
}

TEST_F(JsonRpcCoverageTest, AddUriReturnsGidAndTellStatusReflectsOptions) {
    start_server();

    const std::string dir = ::testing::TempDir();
    json opts = {{"dir", dir}, {"out", "falcon-rpc-cov-out.bin"}};
    auto parsed = call("aria2.addUri",
                       json::array({json::array({"test://local/coverage.bin"}), opts}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    ASSERT_TRUE(parsed["result"].is_string());
    const std::string gid = parsed["result"].get<std::string>();
    EXPECT_EQ(gid.size(), std::size_t{16});

    // addUri 会立即 start_task，StubHandler 瞬时完成（100/100）。
    // 轮询直到任务进入终态，消除与引擎工作线程启动时序的竞态。
    json res;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto status = call("aria2.tellStatus", json::array({gid}));
        ASSERT_TRUE(status.contains("result")) << status.dump();
        res = status["result"];
        const std::string s = res["status"].get<std::string>();
        if (s != "active" && s != "waiting") break;
        ASSERT_LT(std::chrono::steady_clock::now(), deadline)
            << "task did not reach a terminal state: " << status.dump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(res["gid"], gid);
    EXPECT_EQ(res["status"], "complete");
    EXPECT_EQ(res["files"][0]["path"], dir + "falcon-rpc-cov-out.bin");
    EXPECT_EQ(res["files"][0]["uris"][0]["uri"], "test://local/coverage.bin");
    EXPECT_EQ(res["totalLength"], "100");
    EXPECT_EQ(res["completedLength"], "100");
    EXPECT_EQ(res["downloadSpeed"], "0");
    EXPECT_TRUE(res.contains("errorMessage"));
}


} // namespace
