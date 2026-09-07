/**
 * @file http_commands_coverage_test.cpp
 * @brief HTTP 命令（V2 socket 管线）覆盖补充单元测试
 * @author Falcon Team
 * @date 2026-09-05
 *
 * 离线测试策略：
 * - HttpInitiateConnectionCommand 通过本地回环 TCP listener（127.0.0.1）验证
 *   连接、请求发送与失败路径，不访问外部网络
 * - HttpResponseCommand / HttpDownloadCommand 在 socketpair 上注入预构造的
 *   HTTP 响应字节流，验证头部解析、重定向、错误码与文件写入逻辑
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define CLOSE_SOCKET(fd) closesocket(fd)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#define CLOSE_SOCKET(fd) close(fd)
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/request_group.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace falcon;

namespace {

//==============================================================================
// 平台辅助
//==============================================================================

#ifdef _WIN32
void ensure_winsock_for_coverage() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#endif

/// 创建一对已连接的非阻塞 Socket（Unix 用 socketpair，Windows 用回环 TCP）
std::array<int, 2> make_socket_pair_nb() {
#ifdef _WIN32
    ensure_winsock_for_coverage();

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) return {-1, -1};

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listen_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_sock, 1) != 0) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    int len = sizeof(addr);
    if (getsockname(listen_sock, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    SOCKET client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_sock == INVALID_SOCKET) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    if (connect(client_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listen_sock);
        closesocket(client_sock);
        return {-1, -1};
    }

    SOCKET server_sock = accept(listen_sock, NULL, NULL);
    closesocket(listen_sock);
    if (server_sock == INVALID_SOCKET) {
        closesocket(client_sock);
        return {-1, -1};
    }

    u_long mode = 1;
    ioctlsocket(client_sock, FIONBIO, &mode);
    ioctlsocket(server_sock, FIONBIO, &mode);

    return {static_cast<int>(client_sock), static_cast<int>(server_sock)};
#else
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {-1, -1};
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }
    return {fds[0], fds[1]};
#endif
}

/// RAII fd 守卫（release() 用于命令已接管关闭责任的场景）
class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() { reset(); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const { return fd_; }
    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            CLOSE_SOCKET(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

/// 获取一个当前未被监听的回环端口（用于触发 ECONNREFUSED）
uint16_t get_closed_loopback_port() {
#ifdef _WIN32
    ensure_winsock_for_coverage();
#endif
    int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return 9;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    uint16_t port = 9;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        struct sockaddr_in bound;
        socklen_t len = sizeof(bound);
        if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &len) == 0) {
            port = ntohs(bound.sin_port);
        }
    }
    CLOSE_SOCKET(fd);
    return port;
}

/// 向 fd 写入完整数据块（小数据量，一次或多次写完）
bool write_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        int n = send(fd, data.data() + sent,
                     static_cast<int>(data.size() - sent), 0);
#else
        ssize_t n = write(fd, data.data() + sent, data.size() - sent);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

//==============================================================================
// 本地回环 HTTP 服务器（单连接、单响应）
//==============================================================================

class LocalTcpServer {
public:
    enum class Mode {
        kRespondAndClose,   ///< 读取请求后发送预设响应并关闭
        kCloseImmediately,  ///< 接受连接后立即关闭（用于 TLS 失败等场景）
    };

    LocalTcpServer(Mode mode, std::string response)
        : mode_(mode), response_(std::move(response)) {
#ifdef _WIN32
        ensure_winsock_for_coverage();
#endif
        listen_fd_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
        if (listen_fd_ < 0) return;

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listen_fd_, 1) != 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        thread_ = std::thread([this]() { run(); });
    }

    ~LocalTcpServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
        }
    }

    LocalTcpServer(const LocalTcpServer&) = delete;
    LocalTcpServer& operator=(const LocalTcpServer&) = delete;

    bool valid() const { return listen_fd_ >= 0 && port_ != 0; }
    uint16_t port() const { return port_; }
    const std::string& received_request() const { return received_; }

    /// 等待服务器线程结束（请求已处理完毕）
    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        // 有界等待客户端连接（5s），避免测试失败时线程悬挂
        struct pollfd pfd;
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;
#ifdef _WIN32
        if (WSAPoll(&pfd, 1, 5000) <= 0) return;
#else
        if (poll(&pfd, 1, 5000) <= 0) return;
#endif

        int fd = static_cast<int>(accept(listen_fd_, nullptr, nullptr));
        if (fd < 0) return;

        if (mode_ == Mode::kCloseImmediately) {
            CLOSE_SOCKET(fd);
            return;
        }

        // 读超时 2s：读到头部结束符或超时为止
#ifdef _WIN32
        DWORD timeout_ms = 2000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        char buf[2048];
        while (received_.find("\r\n\r\n") == std::string::npos &&
               received_.size() < 65536) {
            const ssize_t n = recv(fd, buf, static_cast<int>(sizeof(buf)), 0);
            if (n <= 0) break;
            received_.append(buf, static_cast<std::size_t>(n));
        }

        if (!response_.empty()) {
            std::size_t sent = 0;
            while (sent < response_.size()) {
                const std::size_t remaining = response_.size() - sent;
#ifdef _WIN32
                const int send_len = static_cast<int>(remaining);
#else
                const std::size_t send_len = remaining;
#endif
                const ssize_t n = send(fd, response_.data() + sent, send_len, 0);
                if (n <= 0) break;
                sent += static_cast<std::size_t>(n);
            }
        }

        CLOSE_SOCKET(fd);
    }

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    Mode mode_;
    std::string response_;
    std::string received_;
    std::thread thread_;
};

//==============================================================================
// 引擎 + 任务辅助
//==============================================================================

struct TaskHandle {
    TaskId id = 0;
    RequestGroup* group = nullptr;
    DownloadTask::Ptr task;
};

/// 在引擎中注册一个下载任务（http URL 保证 init() 成功）
TaskHandle make_engine_task(DownloadEngineV2& engine,
                            const std::string& out_path,
                            bool create_dir = false) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.create_directory = create_dir;

    TaskHandle handle;
    handle.id = engine.add_download("http://127.0.0.1/file.bin", options);
    handle.group = engine.request_group_man()->find_group(handle.id);
    if (handle.group) {
        handle.group->init();
        handle.task = handle.group->download_task();
    }
    return handle;
}

std::string read_file_content(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

} // namespace

//==============================================================================
// 测试夹具
//==============================================================================

class HttpCommandsCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifndef _WIN32
        // 忽略 SIGPIPE：TLS 握手失败路径中 OpenSSL 可能向已关闭的对端
        // 写 close_notify，触发 EPIPE；忽略信号让错误以返回值形式出现
        signal(SIGPIPE, SIG_IGN);
#endif
        auto base = std::filesystem::temp_directory_path();
        std::ostringstream unique;
        unique << "falcon_http_cmd_" << ::testing::UnitTest::GetInstance()
                                                     ->random_seed()
               << "_" << counter_.fetch_add(1);
        test_dir_ = (base / unique.str()).string();
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    std::string test_dir_;

    static std::atomic<unsigned> counter_;
};

std::atomic<unsigned> HttpCommandsCoverageTest::counter_{0};

//==============================================================================
// HttpInitiateConnectionCommand 测试
//==============================================================================

TEST_F(HttpCommandsCoverageTest, InitiateConnectionNullEngineFails) {
    DownloadOptions options;
    HttpInitiateConnectionCommand cmd(1, "http://127.0.0.1/file.bin", options);

    EXPECT_TRUE(cmd.execute(nullptr));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST_F(HttpCommandsCoverageTest, InitiateConnectionRefusedPortFails) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    const uint16_t closed_port = get_closed_loopback_port();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(closed_port) + "/file.bin";

    DownloadOptions options;
    HttpInitiateConnectionCommand cmd(1, url, options);

    bool done = false;
    for (int i = 0; i < 10 && !done; ++i) {
        done = cmd.execute(&engine);
        if (!done) {
            engine.event_poll()->poll(10);
        }
    }

    EXPECT_TRUE(done);
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST_F(HttpCommandsCoverageTest, InitiateConnectionHttpSuccess) {
    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    LocalTcpServer server(LocalTcpServer::Mode::kRespondAndClose, response);
    ASSERT_TRUE(server.valid());

    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin";

    DownloadOptions options;
    options.user_agent = "FalconCoverage/1.0";
    options.referer = "http://127.0.0.1/refer";
    options.headers["X-Coverage"] = "yes";

    HttpInitiateConnectionCommand cmd(1, url, options);

    bool done = false;
    for (int i = 0; i < 100 && !done; ++i) {
        done = cmd.execute(&engine);
        if (!done) {
            engine.event_poll()->poll(10);
        }
    }

    ASSERT_TRUE(done) << "connection/request did not finish in time";
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
    EXPECT_EQ(cmd.connection_state(), HttpConnectionState::REQUEST_SENT);
    ASSERT_GE(cmd.socket_fd(), 0);
    ASSERT_NE(cmd.http_request(), nullptr);

    // 等待服务器线程读完请求并发送响应，保证后续断言确定
    server.join();

    // 请求已完整到达服务器端
    EXPECT_NE(server.received_request().find("GET /file.bin HTTP/1.1"),
              std::string::npos);
    EXPECT_NE(server.received_request().find("Host: 127.0.0.1"),
              std::string::npos);
    EXPECT_NE(server.received_request().find("FalconCoverage/1.0"),
              std::string::npos);
    EXPECT_NE(server.received_request().find("X-Coverage: yes"),
              std::string::npos);

    CLOSE_SOCKET(cmd.socket_fd());
}

TEST_F(HttpCommandsCoverageTest, InitiateConnectionHttpsToPlainServerFails) {
    LocalTcpServer server(LocalTcpServer::Mode::kCloseImmediately, "");
    ASSERT_TRUE(server.valid());

    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    const std::string url =
        "https://127.0.0.1:" + std::to_string(server.port()) + "/file.bin";

    DownloadOptions options;
    HttpInitiateConnectionCommand cmd(1, url, options);

    bool done = false;
    for (int i = 0; i < 100 && !done; ++i) {
        done = cmd.execute(&engine);
        if (!done) {
            engine.event_poll()->poll(10);
        }
    }

    // TLS 握手必然失败（无 OpenSSL 时同样报错）
    EXPECT_TRUE(done);
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST_F(HttpCommandsCoverageTest, InitiateConnectionInvalidPortThrows) {
    DownloadOptions options;
    // 端口非数字：std::stoi 抛出异常（构造函数内 URL 解析分支）
    EXPECT_THROW(
        HttpInitiateConnectionCommand(1, "http://127.0.0.1:notaport/f", options),
        std::exception);
}

//==============================================================================
// HttpResponseCommand 测试
//==============================================================================

TEST_F(HttpCommandsCoverageTest, Response200SchedulesDownloadCommand) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    ASSERT_TRUE(write_all(fd1, raw));

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(9999, fd0, request, options);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
    EXPECT_EQ(cmd.status_code(), 200);
    EXPECT_EQ(cmd.content_length(), 5);
    EXPECT_FALSE(cmd.is_redirect());
    EXPECT_FALSE(cmd.accepts_range());
}

TEST_F(HttpCommandsCoverageTest, Response200WithAcceptRangesUsesSegments) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: 16\r\n"
        "\r\n"
        "0123";
    ASSERT_TRUE(write_all(fd1, raw));

    DownloadOptions options;
    options.min_segment_size = 4;   // 强制走多分段分支
    options.max_connections = 2;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(9999, fd0, request, options);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
    EXPECT_TRUE(cmd.accepts_range());
    EXPECT_TRUE(cmd.supports_resume());
    EXPECT_EQ(cmd.content_length(), 16);
}

TEST_F(HttpCommandsCoverageTest, ResponseRedirect302FailsTask) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    const std::string out_path = test_dir_ + "/redirect.bin";
    TaskHandle handle = make_engine_task(engine, out_path);
    ASSERT_NE(handle.group, nullptr);
    ASSERT_NE(handle.task, nullptr);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    const std::string raw =
        "HTTP/1.1 302 Found\r\n"
        "Location: /moved.bin\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    ASSERT_TRUE(write_all(fd1, raw));

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(handle.id, fd0, request, options);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
    EXPECT_TRUE(cmd.is_redirect());
    EXPECT_EQ(handle.task->status(), TaskStatus::Failed);
    EXPECT_EQ(handle.group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(handle.group->error_message().find("redirect"), std::string::npos);
}

TEST_F(HttpCommandsCoverageTest, Response404Fails) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    const std::string raw =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    ASSERT_TRUE(write_all(fd1, raw));

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(9999, fd0, request, options);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
    EXPECT_EQ(cmd.status_code(), 404);
}

TEST_F(HttpCommandsCoverageTest, ResponseEmptyReplyFails) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);

    // 对端立即关闭且未发送任何数据
    CLOSE_SOCKET(fd1);

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(9999, fd0, request, options);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST_F(HttpCommandsCoverageTest, ResponseNoDataWaitsForSocket) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(9999, fd0, request, options);

    // 无数据可读：应注册读事件并返回等待
    EXPECT_FALSE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::ACTIVE);
}

TEST_F(HttpCommandsCoverageTest, ResponseMalformedStatusLineFails) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    ASSERT_TRUE(write_all(fd1, "GARBAGE-STATUS-LINE\r\n\r\n"));

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(9999, fd0, request, options);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST_F(HttpCommandsCoverageTest, ResponseMalformedHeaderLineFails) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    ASSERT_TRUE(write_all(fd1, "HTTP/1.1 200 OK\r\nheader-without-colon\r\n\r\n"));

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(9999, fd0, request, options);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST_F(HttpCommandsCoverageTest, ResponseHeadersArrivingInParts) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(9999, fd0, request, options);

    // 第一段：仅状态行，未出现 \r\n\r\n → 等待
    ASSERT_TRUE(write_all(fd1, "HTTP/1.1 200 OK\r\n"));
    EXPECT_FALSE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::ACTIVE);

    // 第二段：头部结束 + 初始 body
    ASSERT_TRUE(write_all(fd1, "Content-Length: 3\r\n\r\nabc"));
    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
    EXPECT_EQ(cmd.content_length(), 3);
}

TEST_F(HttpCommandsCoverageTest, Response200UpdatesTaskProgress) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    const std::string out_path = test_dir_ + "/progress.bin";
    TaskHandle handle = make_engine_task(engine, out_path);
    ASSERT_NE(handle.group, nullptr);
    ASSERT_NE(handle.task, nullptr);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "0123456789";
    ASSERT_TRUE(write_all(fd1, raw));

    DownloadOptions options;
    auto request = std::make_shared<HttpRequest>();
    HttpResponseCommand cmd(handle.id, fd0, request, options);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(handle.task->total_bytes(), 10U);
}

//==============================================================================
// HttpDownloadCommand 测试
//==============================================================================

TEST_F(HttpCommandsCoverageTest, DownloadWritesBodyToFileAndCompletes) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    const std::string out_path = test_dir_ + "/body.bin";
    TaskHandle handle = make_engine_task(engine, out_path);
    ASSERT_NE(handle.group, nullptr);
    ASSERT_NE(handle.task, nullptr);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);
    ScopedFd guard1(fd1);

    ASSERT_TRUE(write_all(fd1, "hello body"));  // 10 字节 body

    auto response = std::make_shared<HttpResponse>();
    HttpDownloadCommand cmd(handle.id, fd0, response,
                            /*segment_id=*/0, /*offset=*/0, /*length=*/0);

    // 第一轮：写入数据后无更多数据 → 等待 socket
    EXPECT_FALSE(cmd.execute(&engine));
    EXPECT_EQ(cmd.downloaded_bytes(), 10U);
    EXPECT_FALSE(cmd.is_complete());

    // 对端关闭后再次执行：length 未知(0) → 视为下载完成
    guard1.reset();
    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_TRUE(cmd.is_complete());
    EXPECT_EQ(handle.task->status(), TaskStatus::Completed);
    EXPECT_EQ(handle.group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), "hello body");
}

TEST_F(HttpCommandsCoverageTest, DownloadInitialDataCompletesImmediately) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    const std::string out_path = test_dir_ + "/initial.bin";
    TaskHandle handle = make_engine_task(engine, out_path);
    ASSERT_NE(handle.group, nullptr);
    ASSERT_NE(handle.task, nullptr);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);

    // 初始数据正好覆盖全部长度；对端直接关闭
    CLOSE_SOCKET(fd1);

    auto response = std::make_shared<HttpResponse>();
    HttpDownloadCommand cmd(handle.id, fd0, response,
                            /*segment_id=*/0, /*offset=*/0, /*length=*/6,
                            /*initial_data=*/"ABCDEF");

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_TRUE(cmd.is_complete());
    EXPECT_EQ(cmd.downloaded_bytes(), 6U);
    EXPECT_EQ(handle.task->status(), TaskStatus::Completed);
    EXPECT_EQ(handle.group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), "ABCDEF");
}

TEST_F(HttpCommandsCoverageTest, DownloadRecvErrorBeforeCompleteFailsTask) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    const std::string out_path = test_dir_ + "/truncated.bin";
    TaskHandle handle = make_engine_task(engine, out_path);
    ASSERT_NE(handle.group, nullptr);
    ASSERT_NE(handle.task, nullptr);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard0(fd0);

    // 对端立即关闭：0 字节 < 期望长度 → 错误
    CLOSE_SOCKET(fd1);

    auto response = std::make_shared<HttpResponse>();
    HttpDownloadCommand cmd(handle.id, fd0, response,
                            /*segment_id=*/0, /*offset=*/0, /*length=*/100);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
    EXPECT_EQ(handle.task->status(), TaskStatus::Failed);
    EXPECT_EQ(handle.group->status(), RequestGroupStatus::FAILED);
    EXPECT_FALSE(handle.task->error_message().empty());
}

TEST_F(HttpCommandsCoverageTest, DownloadUnknownTaskIdCompletesQuietly) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard1(fd1);

    auto response = std::make_shared<HttpResponse>();
    HttpDownloadCommand cmd(/*task_id=*/999999, fd0, response,
                            /*segment_id=*/0, /*offset=*/0, /*length=*/0);

    // 无关联 RequestGroup：命令直接成功结束
    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::COMPLETED);
}

TEST_F(HttpCommandsCoverageTest, DownloadGroupWithoutTaskFails) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    // 注册组但不调用 init()：download_task() 为空
    DownloadOptions options;
    options.output_filename = test_dir_ + "/no_task.bin";
    TaskId id = engine.add_download("http://127.0.0.1/file.bin", options);
    RequestGroup* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->download_task(), nullptr);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard1(fd1);

    auto response = std::make_shared<HttpResponse>();
    HttpDownloadCommand cmd(id, fd0, response,
                            /*segment_id=*/0, /*offset=*/0, /*length=*/0);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("DownloadTask"), std::string::npos);
}

TEST_F(HttpCommandsCoverageTest, DownloadOutputFileOpenFailureFails) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    // 输出目录不存在且不自动创建
    const std::string out_path = test_dir_ + "/missing_dir/out.bin";
    TaskHandle handle = make_engine_task(engine, out_path, /*create_dir=*/false);
    ASSERT_NE(handle.group, nullptr);
    ASSERT_NE(handle.task, nullptr);

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ScopedFd guard1(fd1);

    auto response = std::make_shared<HttpResponse>();
    HttpDownloadCommand cmd(handle.id, fd0, response,
                            /*segment_id=*/0, /*offset=*/0, /*length=*/4);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
    EXPECT_EQ(handle.task->status(), TaskStatus::Failed);
    EXPECT_NE(handle.task->error_message().find("Failed to open output file"),
              std::string::npos);
}

//==============================================================================
// HttpRetryCommand 测试
//==============================================================================

TEST_F(HttpCommandsCoverageTest, RetryExceedsMaxRetriesFailsFast) {
    DownloadOptions options;
    options.max_retries = 2;

    HttpRetryCommand cmd(1, "http://127.0.0.1/file.bin", options,
                         /*retry_count=*/3);

    EXPECT_FALSE(cmd.should_retry());
    EXPECT_TRUE(cmd.execute(nullptr));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST_F(HttpCommandsCoverageTest, RetryExceedsMaxRetriesWithEngine) {
    EngineConfigV2 config;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.max_retries = 0;

    HttpRetryCommand cmd(1, "http://127.0.0.1/file.bin", options,
                         /*retry_count=*/1);

    EXPECT_TRUE(cmd.execute(&engine));
    EXPECT_EQ(cmd.status(), CommandStatus::FAILED);
}

TEST_F(HttpCommandsCoverageTest, RetryWithinLimitReportsShouldRetry) {
    DownloadOptions options;
    options.max_retries = 5;

    HttpRetryCommand cmd(1, "http://127.0.0.1/file.bin", options,
                         /*retry_count=*/4);

    // 只验证判定逻辑，不执行 execute（成功路径内含固定 5s 等待）
    EXPECT_TRUE(cmd.should_retry());
    EXPECT_EQ(cmd.retry_count(), 4);
}
