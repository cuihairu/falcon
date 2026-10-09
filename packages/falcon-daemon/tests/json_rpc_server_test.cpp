#include "rpc/json_rpc_server.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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

#include <chrono>
#include <cstring>
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

static std::optional<std::string> recv_all(int fd) {
    std::string buf;
    char tmp[4096];
    while (true) {
        recv_send_size_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) break;
        if (n < 0) return std::nullopt;
        buf.append(tmp, tmp + n);
        if (buf.size() > 1024 * 1024) return std::nullopt;
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
    for (int attempt = 0; attempt < 50; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return ScopedFd{};

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return ScopedFd{fd};
        }
        socket_close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ScopedFd{};
}

static json jsonrpc_call(const std::string& host, uint16_t port, const json& req) {
    (void)host;
    ScopedFd fd = connect_loopback(port);
    EXPECT_GE(fd.fd, 0);

    const std::string body = req.dump();
    std::string http;
    http += "POST /jsonrpc HTTP/1.1\r\n";
    http += "Host: 127.0.0.1\r\n";
    http += "Content-Type: application/json\r\n";
    http += "Connection: close\r\n";
    http += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    http += body;

    EXPECT_TRUE(send_all(fd.fd, http));
    auto resp = recv_all(fd.fd);
    EXPECT_TRUE(resp.has_value());
    auto body_out = extract_body(*resp);
    EXPECT_TRUE(body_out.has_value());
    return json::parse(*body_out);
}

} // namespace

TEST(JsonRpcServerTest, ListMethods) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    cfg.secret.clear();
    cfg.allow_origin_all = false;

    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    ASSERT_TRUE(server.start());
    ASSERT_NE(server.port(), 0);

    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "system.listMethods"}, {"params", json::array()}};
    json resp = jsonrpc_call("127.0.0.1", server.port(), req);

    ASSERT_TRUE(resp.contains("result"));
    ASSERT_TRUE(resp["result"].is_array());

    bool found_add = false;
    for (const auto& m : resp["result"]) {
        if (m.is_string() && m.get<std::string>() == "aria2.addUri") {
            found_add = true;
        }
    }
    EXPECT_TRUE(found_add);

    server.stop();
}

TEST(JsonRpcServerTest, SecretTokenRequired) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    cfg.secret = "s3cr3t";

    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    ASSERT_TRUE(server.start());

    // Missing token => Unauthorized
    json req1 = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "system.listMethods"}, {"params", json::array()}};
    json resp1 = jsonrpc_call("127.0.0.1", server.port(), req1);
    ASSERT_TRUE(resp1.contains("error"));
    EXPECT_EQ(resp1["error"]["code"], -32001);

    // Correct token => OK
    json req2 = {{"jsonrpc", "2.0"},
                {"id", 2},
                {"method", "system.listMethods"},
                {"params", json::array({"token:s3cr3t"})}};
    json resp2 = jsonrpc_call("127.0.0.1", server.port(), req2);
    ASSERT_TRUE(resp2.contains("result"));

    server.stop();
}

TEST(JsonRpcServerTest, SwarmStatusSetShareHandlers) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    cfg.secret.clear();

    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    ASSERT_TRUE(server.start());

    // status 处理器：返回 announcer 快照载荷
    server.set_swarm_status_handler([]() {
        return json{{"enabled", true},
                    {"registered", true},
                    {"node_id", "abc123"},
                    {"session", "s-xyz"},
                    {"announced_count", 3},
                    {"queue_depth", 1}};
    });

    // setShare 处理器：记录 bool 入参，回显快照
    std::mutex share_mutex;
    std::optional<bool> share_arg;
    server.set_swarm_share_handler([&share_mutex, &share_arg](bool enabled) {
        std::lock_guard<std::mutex> lock(share_mutex);
        share_arg = enabled;
        return json{{"enabled", enabled},
                    {"registered", false},
                    {"node_id", "abc123"},
                    {"session", ""},
                    {"announced_count", 0},
                    {"queue_depth", 0}};
    });

    // status 往返
    json req_status = {{"jsonrpc", "2.0"},
                       {"id", 10},
                       {"method", "falcon.swarm.status"},
                       {"params", json::array()}};
    json resp_status = jsonrpc_call("127.0.0.1", server.port(), req_status);
    ASSERT_TRUE(resp_status.contains("result"));
    EXPECT_EQ(resp_status["result"]["enabled"], true);
    EXPECT_EQ(resp_status["result"]["registered"], true);
    EXPECT_EQ(resp_status["result"]["node_id"], "abc123");
    EXPECT_EQ(resp_status["result"]["session"], "s-xyz");
    EXPECT_EQ(resp_status["result"]["announced_count"], 3);
    EXPECT_EQ(resp_status["result"]["queue_depth"], 1);

    // setShare 合法参数到达处理器
    json req_share = {{"jsonrpc", "2.0"},
                      {"id", 11},
                      {"method", "falcon.swarm.setShare"},
                      {"params", json::array({json{{"enabled", false}}})}};
    json resp_share = jsonrpc_call("127.0.0.1", server.port(), req_share);
    ASSERT_TRUE(resp_share.contains("result"));
    {
        std::lock_guard<std::mutex> lock(share_mutex);
        ASSERT_TRUE(share_arg.has_value());
        EXPECT_FALSE(*share_arg);
    }
    EXPECT_EQ(resp_share["result"]["enabled"], false);

    // 参数形状错误 => -32602（空 params / enabled 非 bool / 缺 enabled）
    json bad1 = {{"jsonrpc", "2.0"},
                 {"id", 12},
                 {"method", "falcon.swarm.setShare"},
                 {"params", json::array()}};
    EXPECT_EQ(jsonrpc_call("127.0.0.1", server.port(), bad1)["error"]["code"], -32602);

    json bad2 = {{"jsonrpc", "2.0"},
                 {"id", 13},
                 {"method", "falcon.swarm.setShare"},
                 {"params", json::array({json{{"enabled", "true"}}})}};
    EXPECT_EQ(jsonrpc_call("127.0.0.1", server.port(), bad2)["error"]["code"], -32602);

    json bad3 = {{"jsonrpc", "2.0"},
                 {"id", 14},
                 {"method", "falcon.swarm.setShare"},
                 {"params", json::array({json{{"other", true}}})}};
    EXPECT_EQ(jsonrpc_call("127.0.0.1", server.port(), bad3)["error"]["code"], -32602);

    server.stop();
}

TEST(JsonRpcServerTest, SwarmHandlersUnsetFailsCleanly) {
    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    cfg.secret.clear();

    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    ASSERT_TRUE(server.start());

    // 处理器未接线（swarm 支持缺席构建）=> 业务错误 -32603 而非崩溃
    json req_status = {{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "falcon.swarm.status"},
                       {"params", json::array()}};
    EXPECT_EQ(jsonrpc_call("127.0.0.1", server.port(), req_status)["error"]["code"], -32603);

    json req_share = {{"jsonrpc", "2.0"},
                      {"id", 2},
                      {"method", "falcon.swarm.setShare"},
                      {"params", json::array({json{{"enabled", true}}})}};
    EXPECT_EQ(jsonrpc_call("127.0.0.1", server.port(), req_share)["error"]["code"], -32603);

    server.stop();
}
