/**
 * @file http_commands_chunked_test.cpp
 * @brief V2 引擎 chunked 响应端到端测试（Transfer-Encoding 激活）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 覆盖的核心不变量：
 * - 分块响应下载完成，落盘内容为**净载荷**（块大小行 / CRLF / 终止
 *   块等协议杂质绝不进文件），块行跨 recv 边界（粘包/半包）不乱
 * - RFC 7230：Transfer-Encoding 优先于伪造的 Content-Length——
 *   实际接收量超过声明值不越界损坏，总长未知不建续传追踪
 *   （无 .falcon.ctrl 残留）
 * - 终止块未到先断连即截断：总长未知下 EOF 不构成完成证据，
 *   必须按失败收尾，绝不 COMPLETED + 半截数据
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#define CLOSE_SOCKET(fd) closesocket(fd)
#define POLL(fd_ptr, count, timeout_ms) WSAPoll((fd_ptr), (count), (timeout_ms))
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#define CLOSE_SOCKET(fd) close(fd)
#define POLL(fd_ptr, count, timeout_ms) ::poll((fd_ptr), (count), (timeout_ms))
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/resume_control.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_chunked_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int chunked_test_getpid() { return _getpid(); }
using sock_len = int;
using recv_ssize = int;
#else
inline int chunked_test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
using recv_ssize = ssize_t;
#endif

std::string make_body(std::size_t size) {
    std::string body;
    body.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<char>('a' + (i % 26)));
    }
    return body;
}

std::string read_file_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string temp_dir_for(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            (std::string("falcon_v2_chunked_") + tag + "_" +
             std::to_string(chunked_test_getpid())))
        .string();
}

template <typename Pred>
bool wait_for(Pred&& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/**
 * @brief chunked 响应测试服务器
 *
 * 把 body 按块编码发送：`<hex size>\r\n<data>\r\n` ... `0\r\n\r\n`。
 * 模式开关：
 * - fake_content_length：头部再带一个错误的 Content-Length（小于实际
 *   净载荷——验证 RFC 7230 优先级：传输不按声明值截断）
 * - truncate_after_blocks：发送前 N 块后粗暴断连（不发明细终止块），
 *   制造"总长未知 + EOF 截断"
 */
class ChunkedServer {
public:
    ~ChunkedServer() { stop(); }

    bool start(std::string body, std::size_t chunk_size) {
#ifdef _WIN32
        ensure_winsock_for_chunked_test();
#endif
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body_ = std::move(body);
        }
        chunk_size_ = chunk_size;

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        sockaddr_in bound{};
        sock_len len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
            stop();
            return false;
        }
        port_ = ntohs(bound.sin_port);

        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
    }

    int port() const { return port_; }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    void set_fake_content_length(std::size_t n) { fake_content_length_ = n; }

    /// 发送 truncate_blocks 块后直接断连（不发终止块）
    void set_truncate_after_blocks(std::size_t n) { truncate_blocks_ = n; }

private:
    void accept_loop() {
        while (running_) {
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (POLL(&pfd, 1, 500) <= 0) {
                continue;
            }
            sockaddr_in peer{};
            sock_len peer_len = sizeof(peer);
            int conn = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (conn < 0) {
                if (!running_) return;
                continue;
            }
            conn_threads_.emplace_back([this, conn] { serve(conn); });
        }
    }

    void serve(int conn) {
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64 * 1024) {
            recv_ssize n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                CLOSE_SOCKET(conn);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }

        std::string body;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body = body_;
        }

        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n";
        // RFC 7230 冲突场景：chunked 与 Content-Length 并存时传输编码
        // 优先，声明值是错误信息
        if (fake_content_length_ > 0) {
            header += "Content-Length: " +
                      std::to_string(fake_content_length_) + "\r\n";
        }
        header += "\r\n";

        // 编码发送：块行与数据交错（回环上通常粘包到达，正好压
        // 状态机的粘包/半包处理）
        std::string wire = header;
        std::size_t sent_blocks = 0;
        for (std::size_t off = 0; off < body.size(); off += chunk_size_) {
            const std::size_t n = std::min(chunk_size_, body.size() - off);
            wire += to_hex(n) + "\r\n";
            wire.append(body, off, n);
            wire += "\r\n";
            ++sent_blocks;
            if (truncate_blocks_ > 0 && sent_blocks >= truncate_blocks_) {
                break;  // 截断：不发剩余块与终止块
            }
        }
        if (truncate_blocks_ == 0 ||
            truncate_blocks_ * chunk_size_ >= body.size()) {
            wire += "0\r\n\r\n";  // 终止块 + 尾部空行
        }

        send_all(conn, wire.data(), wire.size());
        CLOSE_SOCKET(conn);
    }

    static std::string to_hex(std::size_t v) {
        if (v == 0) return "0";
        std::string hex;
        while (v > 0) {
            const int d = static_cast<int>(v % 16);
            hex.insert(hex.begin(),
                       static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10));
            v /= 16;
        }
        return hex;
    }

    void send_all(int conn, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            // MSG_NOSIGNAL：客户端可能提前关闭（截断场景的服务器侧）
            recv_ssize n = ::send(conn, data + sent,
#ifdef _WIN32
                                  static_cast<int>(size - sent),
#else
                                  size - sent,
#endif
#ifdef _WIN32
                                  0);
#else
                                  MSG_NOSIGNAL);
#endif
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    mutable std::mutex mutex_;
    std::string body_;
    std::size_t chunk_size_ = 0;
    std::size_t fake_content_length_ = 0;
    std::size_t truncate_blocks_ = 0;

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

/// 引擎线程 RAII 守卫（复用暂停测试的模式）：ASSERT 失败提前退出时
/// 仍正确停机并 join，防 joinable thread 析构 terminate 掩盖断言
class ChunkedEngineRunner {
public:
    explicit ChunkedEngineRunner(DownloadEngineV2& engine)
        : engine_(engine), thread_([this] { engine_.run(); }) {}
    ~ChunkedEngineRunner() {
        if (thread_.joinable()) {
            engine_.force_shutdown();
            thread_.join();
        }
    }
    ChunkedEngineRunner(const ChunkedEngineRunner&) = delete;
    ChunkedEngineRunner& operator=(const ChunkedEngineRunner&) = delete;

    void shutdown_and_join() {
        engine_.shutdown();
        if (thread_.joinable()) thread_.join();
    }

private:
    DownloadEngineV2& engine_;
    std::thread thread_;
};

bool wait_group_terminal(DownloadEngineV2& engine, RequestGroup* group,
                         int timeout_ms) {
    return wait_for(
        [&] {
            const auto st = group->status();
            return st == RequestGroupStatus::COMPLETED ||
                   st == RequestGroupStatus::FAILED;
        },
        timeout_ms);
}

} // namespace

/// 分块下载端到端：净载荷落盘（协议杂质不进文件）、块行跨 recv 边界
/// 不乱、完成无续传追踪残留（总长未知不建追踪）
TEST(DownloadEngineV2Chunked, ChunkedPayloadDecodedCompletely) {
    // 96KB、块 5KB（非 2 的幂，压大小行的十六进制与跨包切分）
    const std::string body = make_body(96 * 1024 - 137);  // 非整块尾巴
    ChunkedServer server;
    ASSERT_TRUE(server.start(body, 5 * 1024));

    const std::string dir = temp_dir_for("complete");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "c.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(server.url("/c.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ChunkedEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body)
        << "落盘必须是净载荷（块行/CRLF/终止块不得混入）";
    // 总长未知：不建续传追踪，完成也不残留控制文件
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension, ec));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// RFC 7230 冲突场景：chunked 优先于伪造的 Content-Length——实际
/// 接收量超过声明值不按声明值截断（成品完整），也不越声明值越界
TEST(DownloadEngineV2Chunked, TransferEncodingBeatsFakeContentLength) {
    const std::string body = make_body(48 * 1024);
    ChunkedServer server;
    ASSERT_TRUE(server.start(body, 4 * 1024));
    server.set_fake_content_length(1024);  // 声明 1KB，实际发 48KB

    const std::string dir = temp_dir_for("fakecl");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "f.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ChunkedEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << "chunked 传输不得被伪造的 Content-Length 截断或判败";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 截断防护：终止块未到先断连——总长未知下 EOF 不构成完成证据，
/// 必须 FAILED（绝不 COMPLETED + 半截数据）
TEST(DownloadEngineV2Chunked, TruncatedChunkedStreamFails) {
    const std::string body = make_body(64 * 1024);
    ChunkedServer server;
    ASSERT_TRUE(server.start(body, 4 * 1024));
    server.set_truncate_after_blocks(8);  // 发 32KB 后断连，无终止块

    const std::string dir = temp_dir_for("trunc");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "t.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(server.url("/t.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ChunkedEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "截断的分块流不得假报完成";

    runner.shutdown_and_join();
    server.stop();

    // 半截数据不得顶着最终名出现（temp_extension 语义）
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(out_path, ec));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}
