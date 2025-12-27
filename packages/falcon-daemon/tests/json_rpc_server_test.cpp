#include "rpc/json_rpc_server.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

namespace {

using json = nlohmann::json;

struct ScopedFd {
    int fd = -1;
    ~ScopedFd() {
        if (fd >= 0) ::close(fd);
    }
    ScopedFd() = default;
    explicit ScopedFd(int f) : fd(f) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

static bool send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

static std::optional<std::string> recv_all(int fd) {
    std::string buf;
    char tmp[4096];
    while (true) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
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
        ::close(fd);
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
