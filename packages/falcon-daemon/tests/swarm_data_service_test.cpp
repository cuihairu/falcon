// P2SP 阶段 2 增量 1：入站只读 HTTP 数据服务单测（§10.3）
//
// 全部走真实 socket 回环——start() 绑 127.0.0.1:0 由 OS 分配随机端口，
// port() 访问器取回。客户端为裸 socket 手写请求，逐字节核对响应。
// swarm_data_service.cpp 直接编进本目标（零外部依赖），无需 swarmd client。

#include "daemon/swarm_data_service.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_len_t = int;
using ssize_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_len_t = socklen_t;
#endif

#include <falcon/protocols/file_hash.hpp>

namespace {

using falcon::daemon::SwarmDataService;
using falcon::daemon::SwarmDataServiceConfig;

// 一次原始 HTTP 请求的响应：状态行 + 原始头 + 体。
struct HttpResponse {
    int status = 0;
    std::string headers;  // 原始头区（含状态行）
    std::string body;
    bool connection_closed = false;
};

#ifndef _WIN32

void close_fd(int fd) { ::close(fd); }

// 建立到 127.0.0.1:port 的连接。
int connect_to(std::uint16_t port) {
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_fd(fd);
        return -1;
    }
    return fd;
}

// 发送请求字符串并读回完整响应（服务端 Connection: close，读到 EOF 为止）。
HttpResponse raw_exchange(std::uint16_t port, const std::string& request) {
    HttpResponse resp;
    int fd = connect_to(port);
    if (fd < 0) return resp;

    std::size_t off = 0;
    while (off < request.size()) {
        auto n = ::send(fd, request.data() + off, request.size() - off, 0);
        if (n <= 0) break;
        off += static_cast<std::size_t>(n);
    }

    std::string raw;
    char buf[8192];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n == 0) resp.connection_closed = true;
            break;
        }
        raw.append(buf, static_cast<std::size_t>(n));
    }
    close_fd(fd);

    // 解析状态行。
    auto sp1 = raw.find(' ');
    if (sp1 != std::string::npos) {
        resp.status = std::atoi(raw.c_str() + sp1 + 1);
    }
    // 头区 / 体切分。
    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        resp.headers = raw;
        return resp;
    }
    resp.headers = raw.substr(0, hdr_end);
    resp.body = raw.substr(hdr_end + 4);
    return resp;
}

std::string extract_header(const std::string& headers, const std::string& name) {
    std::string lower = headers;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string key = "\n" + name;
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto pos = lower.find(key);
    if (pos == std::string::npos) {
        // 状态行后的首头无前导 \n：也检查绝对开头
        auto alt = lower.find(name + ":");
        if (alt != 0) return {};
        pos = alt;
        pos -= 1;  // 让 pos+key.size() 对齐
    }
    auto colon = lower.find(':', pos + key.size());
    if (colon == std::string::npos) return {};
    auto vs = colon + 1;
    auto ve = lower.find("\r\n", vs);
    if (ve == std::string::npos) ve = headers.size();
    std::string v = headers.substr(vs, ve - vs);
    auto b = v.find_first_not_of(" \t");
    auto e = v.find_last_not_of(" \t");
    if (b == std::string::npos) return {};
    return v.substr(b, e - b + 1);
}

// 生成临时文件写指定字节，返回路径。
std::string write_temp(const std::string& name, const std::string& data) {
    std::string path = "/tmp/falcon_ds_" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    return path;
}

std::string make_bytes(std::size_t n, unsigned seed) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>((i * 31 + seed) % 256);
    }
    return s;
}

#endif  // !_WIN32

}  // namespace

#ifndef _WIN32

TEST(SwarmDataServiceTest, StartsOnRandomPortAndServesFullFile) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);

    std::string err;
    ASSERT_TRUE(svc.start(&err)) << err;
    ASSERT_GT(svc.port(), 0u);
    EXPECT_TRUE(svc.running());

    const std::string bytes = make_bytes(4096, 7);
    const std::string path = write_temp("full.bin", bytes);
    const std::string hex = falcon::FileHasher::calculate(
        path, falcon::HashAlgorithm::SHA256);
    svc.register_resource(hex, path);
    EXPECT_EQ(svc.resource_count(), 1u);

    auto resp = raw_exchange(svc.port(), "GET /by-sha256/" + hex + " HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(extract_header(resp.headers, "Accept-Ranges"), "bytes");
    EXPECT_EQ(resp.body.size(), bytes.size());
    EXPECT_EQ(resp.body, bytes);
    EXPECT_EQ(svc.served_requests(), 1u);

    svc.stop();
    EXPECT_FALSE(svc.running());
    std::remove(path.c_str());
}

TEST(SwarmDataServiceTest, RangeRequestServesExactSlice) {
    SwarmDataService svc({[] {
        SwarmDataServiceConfig c;
        c.bind_address = "127.0.0.1";
        c.listen_port = 0;
        return c;
    }()});
    ASSERT_TRUE(svc.start());

    const std::string bytes = make_bytes(1000, 3);
    const std::string path = write_temp("range.bin", bytes);
    const std::string hex = falcon::FileHasher::calculate(
        path, falcon::HashAlgorithm::SHA256);
    svc.register_resource(hex, path);

    // bytes=100-199 → 恰 100 字节
    auto resp = raw_exchange(
        svc.port(), "GET /by-sha256/" + hex + " HTTP/1.1\r\nRange: bytes=100-199\r\n\r\n");
    EXPECT_EQ(resp.status, 206);
    EXPECT_EQ(extract_header(resp.headers, "Content-Range"), "bytes 100-199/1000");
    EXPECT_EQ(resp.body.size(), 100u);
    EXPECT_EQ(resp.body, bytes.substr(100, 100));

    // bytes=500- 开放末端
    auto resp2 = raw_exchange(
        svc.port(), "GET /by-sha256/" + hex + " HTTP/1.1\r\nRange: bytes=500-\r\n\r\n");
    EXPECT_EQ(resp2.status, 206);
    EXPECT_EQ(resp2.body.size(), 500u);
    EXPECT_EQ(resp2.body, bytes.substr(500));

    // 越界 → 416
    auto resp3 = raw_exchange(
        svc.port(), "GET /by-sha256/" + hex + " HTTP/1.1\r\nRange: bytes=5000-\r\n\r\n");
    EXPECT_EQ(resp3.status, 416);
    EXPECT_EQ(extract_header(resp3.headers, "Content-Range"), "bytes */1000");

    // 尾部范围 bytes=-40 → 末 40 字节
    auto resp4 = raw_exchange(
        svc.port(), "GET /by-sha256/" + hex + " HTTP/1.1\r\nRange: bytes=-40\r\n\r\n");
    EXPECT_EQ(resp4.status, 206);
    EXPECT_EQ(resp4.body.size(), 40u);
    EXPECT_EQ(resp4.body, bytes.substr(bytes.size() - 40));

    svc.stop();
    std::remove(path.c_str());
}

TEST(SwarmDataServiceTest, HeadReturnsHeadersWithoutBody) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());

    const std::string bytes = make_bytes(256, 9);
    const std::string path = write_temp("head.bin", bytes);
    const std::string hex = falcon::FileHasher::calculate(
        path, falcon::HashAlgorithm::SHA256);
    svc.register_resource(hex, path);

    auto resp = raw_exchange(svc.port(), "HEAD /by-sha256/" + hex + " HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(extract_header(resp.headers, "Content-Length"), "256");
    EXPECT_TRUE(resp.body.empty());

    svc.stop();
    std::remove(path.c_str());
}

TEST(SwarmDataServiceTest, UnknownHashIs404) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());

    auto resp = raw_exchange(svc.port(), "GET /by-sha256/deadbeef HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp.status, 404);

    svc.stop();
}

TEST(SwarmDataServiceTest, NonHexHashIs400) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());

    auto resp = raw_exchange(svc.port(), "GET /by-sha256/zzzz HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp.status, 400);

    svc.stop();
}

TEST(SwarmDataServiceTest, WrongPathIs404) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());

    auto resp = raw_exchange(svc.port(), "GET /other HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp.status, 404);

    svc.stop();
}

TEST(SwarmDataServiceTest, NonReadMethodIs405) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());

    auto resp = raw_exchange(svc.port(), "POST /by-sha256/aa HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp.status, 405);

    auto resp2 = raw_exchange(svc.port(), "DELETE /by-sha256/aa HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp2.status, 405);

    svc.stop();
}

TEST(SwarmDataServiceTest, UnregisterAndClearDropService) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());

    const std::string bytes = make_bytes(64, 1);
    const std::string path = write_temp("unreg.bin", bytes);
    const std::string hex = falcon::FileHasher::calculate(
        path, falcon::HashAlgorithm::SHA256);
    svc.register_resource(hex, path);
    EXPECT_EQ(svc.resource_count(), 1u);

    svc.unregister_resource(hex);
    EXPECT_EQ(svc.resource_count(), 0u);
    auto resp = raw_exchange(svc.port(), "GET /by-sha256/" + hex + " HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp.status, 404);

    // clear() 幂等
    svc.register_resource(hex, path);
    svc.clear();
    EXPECT_EQ(svc.resource_count(), 0u);

    svc.stop();
    std::remove(path.c_str());
}

TEST(SwarmDataServiceTest, RegisteredFileMissingIs404) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());

    // 注册一个不存在的路径（文件被删/移走）——打开失败按 404。
    svc.register_resource("aa", "/tmp/falcon_ds_does_not_exist_9999");
    auto resp = raw_exchange(svc.port(), "GET /by-sha256/aa HTTP/1.1\r\n\r\n");
    EXPECT_EQ(resp.status, 404);

    svc.stop();
}

TEST(SwarmDataServiceTest, StopIsIdempotentAndClosesListener) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());
    const std::uint16_t port = svc.port();

    svc.stop();
    EXPECT_FALSE(svc.running());
    svc.stop();  // 幂等
    EXPECT_FALSE(svc.running());

    // 停机后连接应被拒（listener fd 已关）。
    int fd = connect_to(port);
    EXPECT_LT(fd, 0);
}

TEST(SwarmDataServiceTest, ConcurrentClientsServed) {
    SwarmDataServiceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.listen_port = 0;
    SwarmDataService svc(cfg);
    ASSERT_TRUE(svc.start());

    const std::string bytes = make_bytes(8192, 5);
    const std::string path = write_temp("conc.bin", bytes);
    const std::string hex = falcon::FileHasher::calculate(
        path, falcon::HashAlgorithm::SHA256);
    svc.register_resource(hex, path);

    constexpr int kN = 8;
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kN; ++i) {
        threads.emplace_back([&, i] {
            // 每客户端取不同 Range 片，验证并发正确切片。
            const std::size_t seg = bytes.size() / kN;
            const std::size_t s = static_cast<std::size_t>(i) * seg;
            auto resp = raw_exchange(
                svc.port(), "GET /by-sha256/" + hex + " HTTP/1.1\r\nRange: bytes=" +
                                std::to_string(s) + "-" + std::to_string(s + seg - 1) +
                                "\r\n\r\n");
            if (resp.status == 206 && resp.body == bytes.substr(s, seg)) ok.fetch_add(1);
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(ok.load(), kN);
    EXPECT_EQ(svc.served_requests(), static_cast<std::uint64_t>(kN));

    svc.stop();
    std::remove(path.c_str());
}

#else  // _WIN32

TEST(SwarmDataServiceTest, WindowsPlaceholder) {
    GTEST_SKIP() << "data service loopback tests are POSIX-only in this suite";
}

#endif  // !_WIN32
