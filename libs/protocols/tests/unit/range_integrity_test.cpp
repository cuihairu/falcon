/**
 * @file range_integrity_test.cpp
 * @brief V1 引擎 Range 完整性端到端测试（"撒谎服务器"静默损坏防护）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 缺陷背景：服务器宣称 Accept-Ranges 却在 GET 时忽略 Range 回 200 全量
 * 响应（透明代理/CGI 常见），旧版把整个文件追加进已有段/临时文件——
 * 段超尺寸被 min() clamp "祝福"为完成，损坏数据静默并入成品且任务报
 * COMPLETED。修复后：start>0 的请求要求 206（否则撤销追加按失败收尾）、
 * 段成功路径精确尺寸校验、merge 前逐段校验。
 *
 * 本测试用回环"撒谎服务器"验证两条下载路径的终局不变量：
 * 绝不允许 COMPLETED + 内容损坏
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
#include <csignal>
#define CLOSE_SOCKET(fd) close(fd)
#define POLL(fd_ptr, count, timeout_ms) ::poll((fd_ptr), (count), (timeout_ms))
#endif

#include <gtest/gtest.h>
#include <falcon/download_engine.hpp>

#include "../../plugins/http/http_handler.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_range_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int test_getpid() { return _getpid(); }
// Winsock（winsock2.h）无 socklen_t/ssize_t：长度参数与 recv/send 返回值均为 int
using sock_len = int;
using recv_ssize = int;
#else
inline int test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
using recv_ssize = ssize_t;
#endif

/// "撒谎服务器"：HEAD 宣称 Accept-Ranges: bytes，GET 却忽略 Range
/// 一律回 200 + 整个文件（真实世界的透明代理/CGI 行为）
class RangeLiarServer {
public:
    bool start(std::string body) {
#ifdef _WIN32
        ensure_winsock_for_range_test();
#else
        // CI 红面（run 35947196187，clang Release job 0.01s 死于 SIGPIPE）：
        // 本服务器的全部意义就是被客户端「失败收口」——分段路径收到
        // 200 头（非 206）即 abort 连接（RST），conn 线程对已关/RST 的
        // socket 继续 send body 时内核触发 SIGPIPE 杀死整个测试进程。
        // 该窗口是纯竞速两面（send 先完成则绿，插桩树恒绿掩盖；无插
        // 桩 Release 客户端失败更快更易命中）。进程级忽略让写失败以
        // EPIPE/ECONNRESET 返回值出现（send_all 的 n<=0 分支正常吞掉），
        // 与 edges 测试文件服务器 / TlsTestServer 的既有先例同法
        signal(SIGPIPE, SIG_IGN);
#endif
        body_ = std::move(body);

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
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
    const std::string& body() const { return body_; }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

private:
    void accept_loop() {
        // poll 带超时轮询 running_：阻塞 accept 上直接 close(fd) 在
        // Linux 不保证唤醒（复用 global_speed_limit_test 的模板）
        while (running_) {
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            const int ready = POLL(&pfd, 1, 500);
            if (ready <= 0) {
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
        char buf[2048];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 16 * 1024) {
            recv_ssize n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                CLOSE_SOCKET(conn);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }

        const bool is_head = request.rfind("HEAD", 0) == 0;
        const bool has_range = request.find("Range:") != std::string::npos ||
                               request.find("range:") != std::string::npos;
        if (has_range) {
            range_requests_.fetch_add(1);
        }

        // 谎言本体：带 Range 的 GET 也回 200 + 整个文件（正确实现应为
        // 206 + Content-Range 切片）
        std::string header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: application/octet-stream\r\n"
                             "Content-Length: " + std::to_string(body_.size()) + "\r\n"
                             "Accept-Ranges: bytes\r\n"
                             "Connection: close\r\n\r\n";
        send_all(conn, header.data(), header.size());
        if (!is_head) {
            send_all(conn, body_.data(), body_.size());
        }
        CLOSE_SOCKET(conn);
    }

    void send_all(int conn, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            recv_ssize n = ::send(conn, data + sent,
#ifdef _WIN32
                               static_cast<int>(size - sent),
#else
                               size - sent,
#endif
                               0);
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    std::string body_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;

public:
    std::atomic<int> range_requests_{0};  ///< 收到过多少个带 Range 的请求
};

std::string make_body(std::size_t size) {
    std::string body;
    body.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<char>('a' + (i % 26)));
    }
    return body;
}

std::string temp_output_dir() {
    return (std::filesystem::temp_directory_path() /
            ("falcon_range_test_" + std::to_string(test_getpid())))
        .string();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

} // namespace

// 单连接路径 + 临时文件已有续传前缀：撒谎服务器把整个文件回给续传
// 请求。终局不变量：要么降级重下成功（内容逐字节一致），要么明确
// 失败（curl 自身的 resume 守卫也可能先拒）——绝不 COMPLETED + 损坏
TEST(RangeIntegrity, SinglePathResumeNeverPublishesCorruptFile) {
    const std::string body = make_body(64 * 1024);
    RangeLiarServer server;
    ASSERT_TRUE(server.start(body));

    DownloadEngine engine;
    engine.register_handler(protocols::create_http_handler());

    DownloadOptions options;
    options.output_directory = temp_output_dir();
    options.output_filename = "single_liar.bin";
    options.max_connections = 1;
    options.retry_delay_seconds = 0;  // 失败重试不引入真实退避等待
    std::filesystem::create_directories(options.output_directory);

    // 预置续传前缀：模拟上一次会话的部分下载
    const auto temp_path =
        std::filesystem::path(options.output_directory) / "single_liar.bin.falcon.tmp";
    {
        std::ofstream temp(temp_path, std::ios::binary | std::ios::trunc);
        temp.write(body.data(), 512);
    }

    auto task = engine.add_task(server.url("/single_liar.bin"), options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    ASSERT_TRUE(task->wait_for(std::chrono::seconds(30)));
    const auto final_status = task->status();

    const auto final_path =
        std::filesystem::path(options.output_directory) / "single_liar.bin";
    if (final_status == TaskStatus::Completed) {
        // 唯一可接受的 Completed：降级重下后内容逐字节一致
        EXPECT_EQ(read_file(final_path), body);
    } else {
        EXPECT_EQ(final_status, TaskStatus::Failed);
    }
    if (std::filesystem::exists(final_path)) {
        // 只要成品出现，内容必须正确（损坏数据绝不许顶着最终名存在）
        EXPECT_EQ(read_file(final_path), body);
    }
    EXPECT_GE(server.range_requests_.load(), 1)
        << "续传前缀存在时必须发出过 Range 请求，测试前提不成立";

    engine.cancel_all();
    server.stop();
    std::filesystem::remove_all(options.output_directory);
}

// 分段路径 + 撒谎服务器：每个段请求都收到 200 全量响应。修复后
// 206 门禁与精确尺寸校验必须让任务失败——旧版会把损坏段静默并入
// 成品并报 COMPLETED
TEST(RangeIntegrity, SegmentedPathAgainstRangeLiarFailsClean) {
    const std::string body = make_body(8 * 1024);
    RangeLiarServer server;
    ASSERT_TRUE(server.start(body));

    DownloadEngine engine;
    engine.register_handler(protocols::create_http_handler());

    DownloadOptions options;
    options.output_directory = temp_output_dir();
    options.output_filename = "segmented_liar.bin";
    options.max_connections = 4;
    options.min_segment_size = 1024;  // 8KB / 1KB → 4 段
    options.max_retries = 0;          // 快速失败（每次重试都会整段重传）
    options.retry_delay_seconds = 0;
    std::filesystem::create_directories(options.output_directory);

    auto task = engine.add_task(server.url("/segmented_liar.bin"), options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    ASSERT_TRUE(task->wait_for(std::chrono::seconds(30)));

    EXPECT_EQ(task->status(), TaskStatus::Failed)
        << "Range 被忽略的分段下载必须失败，不得静默出成品";
    EXPECT_GE(server.range_requests_.load(), 1)
        << "分段下载必然发 Range 请求，测试前提不成立";

    // 终局不变量：成品绝不出现（merge 前校验拦截了损坏段）
    const auto final_path =
        std::filesystem::path(options.output_directory) / "segmented_liar.bin";
    EXPECT_FALSE(std::filesystem::exists(final_path));

    engine.cancel_all();
    server.stop();
    std::filesystem::remove_all(options.output_directory);
}
