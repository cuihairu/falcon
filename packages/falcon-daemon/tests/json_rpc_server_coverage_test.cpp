// JSON-RPC server coverage tests.
//
// Complements json_rpc_server_test.cpp with exhaustive coverage of the
// HTTP layer (methods / paths / CORS / fragmented requests) and the
// JSON-RPC dispatch layer (every aria2.* method, parameter validation,
// error paths, notifications and system.multicall).

#include "rpc/json_rpc_server.hpp"

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
                             "system.multicall"}) {
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

TEST_F(JsonRpcCoverageTest, AddUriParsesAllTypedOptions) {
    start_server();

    const std::string dir = ::testing::TempDir();
    json opts = {
        {"dir", dir},
        {"out", "falcon-cov-a.bin"},
        {"user-agent", "FalconCov/1.0"},
        {"referer", "http://referer.example/"},
        {"load-cookies", dir + "cov-cookies.txt"},
        {"save-cookies", dir + "cov-jar.txt"},
        {"http-user", "covuser"},
        {"http-passwd", "covpass"},
        {"all-proxy", "http://127.0.0.1:9"},
        {"all-proxy-user", "puser"},
        {"all-proxy-passwd", "ppass"},
        {"check-certificate", false},
        {"max-tries", "7"},
        {"retry-wait", 3},
        {"max-connection-per-server", "5"},
        {"max-download-limit", 2048},
        {"header", json::array({"X-Custom-A: 1", "X-Custom-B:2", "no-colon-here", 42})},
    };
    auto parsed = call("aria2.addUri",
                       json::array({json::array({"test://local/a.bin"}), opts}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();

    json opts2 = {
        {"dir", dir},
        {"out", "falcon-cov-b.bin"},
        {"check-certificate", "false"},
        {"max-tries", 9},
        {"retry-wait", "2"},
        {"max-connection-per-server", 6},
        {"max-download-limit", "4096"},
        {"header", "X-Custom-C: 3"},
    };
    parsed = call("aria2.addUri",
                  json::array({json::array({"test://local/b.bin"}), opts2}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
}

// ===========================================================================
// gid validation / task control
// ===========================================================================

TEST_F(JsonRpcCoverageTest, GidValidationErrors) {
    start_server();

    for (const char* const bad : {"not-a-gid", "zzzz", "0", "0000000000000000",
                                  "12345678901234567", "ffffffffffffff", "-1"}) {
        auto parsed = call("aria2.tellStatus", json::array({std::string(bad)}));
        EXPECT_EQ(parsed["error"]["code"], 2) << "gid=" << bad;
    }

    // Valid hex but unknown task.
    auto parsed = call("aria2.tellStatus", json::array({"ffffffffffffffff"}));
    EXPECT_EQ(parsed["error"]["code"], 2);

    // Missing / wrong-typed params.
    parsed = call("aria2.pause", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.pause", json::array({123}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.unpause", json::array({json::array({"x"})}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.remove", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, GidAcceptsHexPrefixes) {
    start_server();
    auto task = engine_.add_task("test://local/prefix");
    ASSERT_TRUE(task);

    for (const std::string& gid : {gid_of(task->id()),
                                   "0x" + gid_of(task->id()),
                                   "0X" + gid_of(task->id())}) {
        auto parsed = call("aria2.tellStatus", json::array({gid}));
        EXPECT_TRUE(parsed.contains("result")) << parsed.dump() << " gid=" << gid;
    }
}

TEST_F(JsonRpcCoverageTest, PausePendingTaskSucceeds) {
    start_server();
    auto task = engine_.add_task("test://local/pauseme");
    ASSERT_TRUE(task);
    const std::string gid = gid_of(task->id());

    auto parsed = call("aria2.pause", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], gid);

    auto status = call("aria2.tellStatus", json::array({gid}));
    ASSERT_TRUE(status.contains("result"));
    EXPECT_EQ(status["result"]["status"], "paused");
}

TEST_F(JsonRpcCoverageTest, UnpauseNotPausedTaskFails) {
    start_server();
    auto task = engine_.add_task("test://local/notpaused");
    ASSERT_TRUE(task);  // stays Pending: never started, never paused

    auto parsed = call("aria2.unpause", json::array({gid_of(task->id())}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 1);
}

TEST_F(JsonRpcCoverageTest, UnpausePausedTaskSucceeds) {
    start_server();
    auto task = engine_.add_task("test://local/resumeme");
    ASSERT_TRUE(task);
    ASSERT_TRUE(engine_.pause_task(task->id()));
    const std::string gid = gid_of(task->id());

    auto parsed = call("aria2.unpause", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], gid);
}

TEST_F(JsonRpcCoverageTest, RemoveTaskSucceeds) {
    start_server();
    auto task = engine_.add_task("test://local/removeme");
    ASSERT_TRUE(task);
    const std::string gid = gid_of(task->id());

    auto parsed = call("aria2.remove", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], gid);

    auto status = call("aria2.tellStatus", json::array({gid}));
    ASSERT_TRUE(status.contains("result"));
    EXPECT_EQ(status["result"]["status"], "removed");
}

TEST_F(JsonRpcCoverageTest, StatusMappingForAllTaskStates) {
    start_server();

    struct Case {
        falcon::TaskStatus status;
        const char* expected;
    };
    std::vector<Case> cases = {
        {falcon::TaskStatus::Pending, "waiting"},
        {falcon::TaskStatus::Preparing, "active"},
        {falcon::TaskStatus::Downloading, "active"},
        {falcon::TaskStatus::Paused, "paused"},
        {falcon::TaskStatus::Completed, "complete"},
        {falcon::TaskStatus::Cancelled, "removed"},
        {falcon::TaskStatus::Failed, "error"},
    };

    for (const auto& c : cases) {
        auto task = engine_.add_task("test://local/state");
        ASSERT_TRUE(task);
        task->set_status(c.status);
        auto parsed = call("aria2.tellStatus", json::array({gid_of(task->id())}));
        ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
        EXPECT_EQ(parsed["result"]["status"], c.expected)
            << "status " << static_cast<int>(c.status);
    }
}

// ===========================================================================
// aria2.tellActive / tellWaiting / tellStopped
// ===========================================================================

TEST_F(JsonRpcCoverageTest, TellActiveEmpty) {
    start_server();
    auto parsed = call("aria2.tellActive", json::array());
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_TRUE(parsed["result"].is_array());
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});
}

TEST_F(JsonRpcCoverageTest, TellActiveReturnsActiveTasks) {
    start_server();
    auto task = engine_.add_task("test://local/active");
    ASSERT_TRUE(task);
    task->set_status(falcon::TaskStatus::Downloading);

    auto parsed = call("aria2.tellActive", json::array());
    ASSERT_TRUE(parsed.contains("result"));
    ASSERT_EQ(parsed["result"].size(), std::size_t{1});
    EXPECT_EQ(parsed["result"][0]["gid"], gid_of(task->id()));
    EXPECT_EQ(parsed["result"][0]["status"], "active");
}

TEST_F(JsonRpcCoverageTest, TellWaitingInvalidParams) {
    start_server();
    auto parsed = call("aria2.tellWaiting", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.tellWaiting", json::array({0}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.tellWaiting", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, TellWaitingWithIntParams) {
    start_server();
    std::vector<falcon::DownloadTask::Ptr> tasks;
    for (int i = 0; i < 3; ++i) {
        auto t = engine_.add_task("test://local/w" + std::to_string(i));
        ASSERT_TRUE(t);
        tasks.push_back(t);
    }

    auto parsed = call("aria2.tellWaiting", json::array({0, 2}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});

    parsed = call("aria2.tellWaiting", json::array({2, 5}));
    ASSERT_TRUE(parsed.contains("result"));
    ASSERT_EQ(parsed["result"].size(), std::size_t{1});

    parsed = call("aria2.tellWaiting", json::array({0, 100}));
    ASSERT_EQ(parsed["result"].size(), std::size_t{3});

    // Negative offset/num clamp to zero.
    parsed = call("aria2.tellWaiting", json::array({-5, -5}));
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});

    // Offset beyond the end.
    parsed = call("aria2.tellWaiting", json::array({50, 10}));
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});
}

TEST_F(JsonRpcCoverageTest, TellWaitingWithStringParams) {
    start_server();
    std::vector<falcon::DownloadTask::Ptr> tasks;
    for (int i = 0; i < 3; ++i) {
        auto t = engine_.add_task("test://local/ws" + std::to_string(i));
        ASSERT_TRUE(t);
        tasks.push_back(t);
    }

    auto parsed = call("aria2.tellWaiting", json::array({"0", "2"}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});

    parsed = call("aria2.tellWaiting", json::array({"1", "10"}));
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});
}

TEST_F(JsonRpcCoverageTest, TellWaitingIncludesPausedTasks) {
    start_server();
    auto pending = engine_.add_task("test://local/wp");
    auto paused = engine_.add_task("test://local/wq");
    ASSERT_TRUE(pending && paused);
    ASSERT_TRUE(engine_.pause_task(paused->id()));

    auto parsed = call("aria2.tellWaiting", json::array({0, 10}));
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_EQ(parsed["result"].size(), std::size_t{2});
}

TEST_F(JsonRpcCoverageTest, TellStoppedInvalidParams) {
    start_server();
    auto parsed = call("aria2.tellStopped", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.tellStopped", json::array({0}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.tellStopped", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, TellStoppedReturnsCancelledTasks) {
    start_server();
    std::vector<falcon::DownloadTask::Ptr> tasks;
    for (int i = 0; i < 3; ++i) {
        auto t = engine_.add_task("test://local/s" + std::to_string(i));
        ASSERT_TRUE(t);
        tasks.push_back(t);
    }
    ASSERT_TRUE(engine_.cancel_task(tasks[0]->id()));
    ASSERT_TRUE(engine_.cancel_task(tasks[2]->id()));

    auto parsed = call("aria2.tellStopped", json::array({0, 10}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"].size(), std::size_t{2});

    parsed = call("aria2.tellStopped", json::array({1, 1}));
    ASSERT_EQ(parsed["result"].size(), std::size_t{1});

    parsed = call("aria2.tellStopped", json::array({"0", "10"}));
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});

    parsed = call("aria2.tellStopped", json::array({0, 0}));
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});
}

// ===========================================================================
// Concurrency smoke test
// ===========================================================================

TEST_F(JsonRpcCoverageTest, ConcurrentRequestsAreHandled) {
    start_server();

    std::atomic<int> ok{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([this, &ok] {
            for (int i = 0; i < 5; ++i) {
                auto full = raw_request(port(), make_http_post(
                    json{{"jsonrpc", "2.0"}, {"id", i}, {"method", "aria2.getVersion"},
                         {"params", json::array()}}
                        .dump()));
                if (full.has_value()) {
                    auto body = extract_body(*full);
                    if (body.has_value() &&
                        json::parse(*body)["result"]["version"] == "0.1.0") {
                        ok.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto& t : workers) t.join();
    EXPECT_EQ(ok.load(), 20);
}

// ===========================================================================
// Batch control / session / global options (no storage required)
// ===========================================================================

TEST_F(JsonRpcCoverageTest, SessionInfoAndSaveSession) {
    start_server();
    auto parsed = call("aria2.getSessionInfo", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_FALSE(parsed["result"]["sessionId"].get<std::string>().empty());

    parsed = call("aria2.saveSession", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
}

TEST_F(JsonRpcCoverageTest, PauseAllAndUnpauseAllReturnOk) {
    start_server();
    auto parsed = call("aria2.pauseAll", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");

    parsed = call("aria2.unpauseAll", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
}

TEST_F(JsonRpcCoverageTest, ForceShutdownInvokesHandler) {
    start_server();
    std::atomic<bool> requested{false};
    server_->set_shutdown_handler([&requested] { requested.store(true); });

    auto parsed = call("aria2.forceShutdown", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
    EXPECT_TRUE(requested.load());
}

TEST_F(JsonRpcCoverageTest, GlobalOptionRoundTrip) {
    start_server();
    auto parsed = call("aria2.changeGlobalOption",
                       json::array({json{{"max-overall-download-limit", "2048"},
                                          {"max-concurrent-downloads", "2"}}}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
    EXPECT_EQ(engine_.get_global_speed_limit(), falcon::BytesPerSecond{2048});
    EXPECT_EQ(engine_.get_max_concurrent_tasks(), std::size_t{2});

    parsed = call("aria2.getGlobalOption", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"]["max-overall-download-limit"], "2048");
    EXPECT_EQ(parsed["result"]["max-concurrent-downloads"], "2");

    // "none" 取消限速
    parsed = call("aria2.changeGlobalOption",
                  json::array({json{{"max-overall-download-limit", "none"}}}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(engine_.get_global_speed_limit(), falcon::BytesPerSecond{0});

    // 非法数值
    parsed = call("aria2.changeGlobalOption",
                  json::array({json{{"max-overall-download-limit", "not-a-number"}}}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();

    // 未知键
    parsed = call("aria2.changeGlobalOption",
                  json::array({json{{"no-such-option", "1"}}}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();

    // params 形状错误
    parsed = call("aria2.changeGlobalOption", json::array({"only-one"}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, GetFilesGetUrisGetOptionOnEngineTask) {
    start_server();
    // fixture 的 StubHandler 只认 test://；不 start_task，保持 Pending 即可查询
    auto task = engine_.add_task("test://direct.bin");
    ASSERT_NE(task, nullptr);
    const std::string gid = gid_of(task->id());

    auto parsed = call("aria2.getFiles", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    ASSERT_EQ(parsed["result"].size(), std::size_t{1});
    EXPECT_EQ(parsed["result"][0]["selected"], "true");
    EXPECT_EQ(parsed["result"][0]["uris"][0]["uri"], "test://direct.bin");

    parsed = call("aria2.getUris", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"][0]["uri"], "test://direct.bin");

    parsed = call("aria2.getOption", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_TRUE(parsed["result"].contains("dir"));
    EXPECT_TRUE(parsed["result"].contains("max-download-limit"));

    // 未知 gid
    parsed = call("aria2.getFiles", json::array({"00000000000ffffffd"}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();
    parsed = call("aria2.getOption", json::array({"00000000000ffffffd"}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();
    parsed = call("aria2.getUris", json::array({"00000000000ffffffd"}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, RemoveDownloadResultUnknownGidIsError) {
    start_server();
    auto parsed = call("aria2.removeDownloadResult", json::array({"00000000000ffffffc"}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 2);
}

// ===========================================================================
// Batch P: parameter validation and edge paths across the method dispatch.
// ===========================================================================

// A handler whose download() blocks until the test releases it, so the task
// stays Downloading (active) for the duration of an RPC call.
class HangingHandler final : public falcon::IProtocolHandler {
public:
    std::string protocol_name() const override { return "hang"; }

    std::vector<std::string> supported_schemes() const override { return {"hang"}; }

    bool can_handle(const std::string& url) const override {
        return url.rfind("hang://", 0) == 0;
    }

    falcon::FileInfo get_file_info(const std::string& url,
                                   const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "hang.bin";
        info.total_size = 100;
        info.supports_resume = true;
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->update_progress(50, 100, 0);
        task->set_status(falcon::TaskStatus::Downloading);
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(10),
                     [this] { return released_.load(); });
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

    void release() {
        released_.store(true);
        cv_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> released_{false};
};

// Release the hanging worker and drive the task to a terminal state so the
// engine can be torn down without a stuck worker thread.
static void release_and_finish(falcon::DownloadEngine& engine,
                               HangingHandler& handler, falcon::TaskId id) {
    handler.release();
    engine.cancel_task(id);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto task = engine.get_task(id);
        if (!task || task->is_finished()) break;
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static bool wait_for_status(falcon::DownloadEngine& engine, falcon::TaskId id,
                            falcon::TaskStatus status) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto task = engine.get_task(id);
        if (task && task->status() == status) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

TEST_F(JsonRpcCoverageTest, ChangePriorityParameterValidation) {
    start_server();
    // 形状错误：空对象 / 单元素 / priority 非数值
    auto parsed = call("aria2.changePriority", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    parsed = call("aria2.changePriority", json::array({"00000000000000ff"}));
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    parsed = call("aria2.changePriority", json::array({"00000000000000ff", true}));
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    // gid 非法
    parsed = call("aria2.changePriority", json::array({"zzz", 1}));
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    // 合法 gid，引擎无任务
    parsed = call("aria2.changePriority", json::array({"00000000000000ff", 1}));
    EXPECT_EQ(parsed["error"]["code"], 2) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, ChangeGlobalOptionIntegerValue) {
    start_server();
    // 整数值形式的 max-concurrent-downloads（字符串形式由既有用例覆盖）
    auto parsed = call("aria2.changeGlobalOption",
                       json::array({json{{"max-concurrent-downloads", 6}}}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
    EXPECT_EQ(engine_.get_max_concurrent_tasks(), std::size_t{6});
}

TEST_F(JsonRpcCoverageTest, TellStatusAndGetFilesParameterValidation) {
    start_server();
    auto parsed = call("aria2.tellStatus", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    for (const char* method : {"aria2.getFiles", "aria2.getUris", "aria2.getOption"}) {
        parsed = call(method, json::array());
        EXPECT_EQ(parsed["error"]["code"], -32602) << method << ": " << parsed.dump();
    }
    // 合法 gid 但引擎与 storage 均无记录
    parsed = call("aria2.getFiles", json::array({"00000000000000ff"}));
    EXPECT_EQ(parsed["error"]["code"], 2) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, PauseFamilyUnknownTaskVariants) {
    start_server();
    // 非法 gid
    for (const char* method : {"aria2.pause", "aria2.forcePause"}) {
        auto parsed = call(method, json::array({"zzz"}));
        EXPECT_EQ(parsed["error"]["code"], 2) << method << ": " << parsed.dump();
    }
    // 合法 gid 但引擎无任务
    for (const char* method :
         {"aria2.pause", "aria2.forcePause", "aria2.unpause", "aria2.remove",
          "aria2.forceRemove"}) {
        auto parsed = call(method, json::array({"00000000000000ff"}));
        EXPECT_EQ(parsed["error"]["code"], 2) << method << ": " << parsed.dump();
    }
}

TEST_F(JsonRpcCoverageTest, RemoveDownloadResultParameterAndNeverExisted) {
    start_server();
    auto parsed = call("aria2.removeDownloadResult", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    // 合法 gid 但引擎与 storage 从未有过该任务
    parsed = call("aria2.removeDownloadResult", json::array({"00000000000000ff"}));
    EXPECT_EQ(parsed["error"]["code"], 2) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, RemoveDownloadResultCompletedTaskRemoved) {
    start_server();
    auto task = engine_.add_task("test://remove-me.bin");
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine_.start_task(task->id()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task->is_finished()) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto parsed = call("aria2.removeDownloadResult", json::array({gid_of(task->id())}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
}

TEST_F(JsonRpcCoverageTest, RemoveDownloadResultActiveTaskRejected) {
    auto hanging = std::make_unique<HangingHandler>();
    HangingHandler* handler = hanging.get();
    engine_.register_handler(std::move(hanging));
    start_server();
    auto task = engine_.add_task("hang://active.bin");
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine_.start_task(task->id()));
    ASSERT_TRUE(wait_for_status(engine_, task->id(), falcon::TaskStatus::Downloading));

    auto parsed = call("aria2.removeDownloadResult", json::array({gid_of(task->id())}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 1);
    EXPECT_EQ(parsed["error"]["message"], "Task cannot be removed while active");

    release_and_finish(engine_, *handler, task->id());
}

TEST_F(JsonRpcCoverageTest, GetOptionEchoesCustomHeaders) {
    start_server();
    falcon::DownloadOptions opts;
    opts.headers["X-Custom-A"] = "1";
    opts.headers["X-Custom-B"] = "two";
    auto task = engine_.add_task("test://hdr.bin", opts);
    ASSERT_NE(task, nullptr);
    auto parsed = call("aria2.getOption", json::array({gid_of(task->id())}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"]["header"]["X-Custom-A"], "1");
    EXPECT_EQ(parsed["result"]["header"]["X-Custom-B"], "two");
}

TEST_F(JsonRpcCoverageTest, ForceShutdownWithoutHandlerIgnored) {
    start_server();
    auto parsed = call("aria2.forceShutdown", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
    // 无 shutdown handler：仅告警，服务器照常服务
    parsed = call("system.listMethods", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_FALSE(parsed["result"].empty());
}

#ifndef _WIN32
TEST(JsonRpcLifecycleTest, StartFailsWhenPortAlreadyBound) {
    int blocker = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(blocker, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(blocker, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(blocker, 1), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(blocker, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    const uint16_t taken = ntohs(addr.sin_port);

    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = taken;
    cfg.bind_address = "127.0.0.1";
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    EXPECT_FALSE(server.start());
    server.stop();
    ::close(blocker);
}
#endif

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
