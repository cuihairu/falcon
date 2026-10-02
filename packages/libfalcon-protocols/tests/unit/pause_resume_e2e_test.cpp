/**
 * @file pause_resume_e2e_test.cpp
 * @brief B11 复现测试：V1 引擎任务暂停→继续 全链路（引擎级，真实 HttpHandler）
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 用户 bug：任务暂停之后点「继续」无法再恢复下载（真实 URL 为大文件直链，
 * 桌面默认 max_connections=4 → 必走分段路径）。本测试在引擎级复现完整
 * 用户操作序列：add_task → start_task → 下载中 pause_task → resume_task →
 * 等终态 → 成品逐字节对拍。覆盖单连接与分段两路径 + 多次反复。
 *
 * 服务器特性（缺一不可）：
 *  - HEAD 忠实应答 Content-Length + Accept-Ranges: bytes（分段判定四条件之一）
 *  - GET 按 Range 忠实 206 切片（SegmentDownloader 对 start>0 段强制 206 门禁；
 *    单连接续传走 CURLOPT_RESUME_FROM_LARGE → curl 发 Range: bytes=N-）
 *  - 分块慢发制造暂停窗口（持续有数据，不触发 LOW_SPEED 停滞看门狗）
 *  - send 带 MSG_NOSIGNAL：客户端 pause 中止连接后服务器继续慢发会写已关 fd
 *  - Range 请求观测：断言「继续后服务器确实收到了带 Range 的续传请求」
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
#include <poll.h>
#define CLOSE_SOCKET(fd) close(fd)
#define POLL(fd_ptr, count, timeout_ms) ::poll((fd_ptr), (count), (timeout_ms))
#endif

#include <gtest/gtest.h>
#include <falcon/download_engine.hpp>

#include "../../plugins/http/http_handler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_pause_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int test_getpid() { return _getpid(); }
using sock_len = int;
using recv_ssize = int;
using send_flags = int;
constexpr send_flags kSendFlags = 0;
#else
inline int test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
using recv_ssize = ssize_t;
using send_flags = int;
// 客户端暂停会中途关连接：服务器慢发线程对已关 fd 写不能被 SIGPIPE 杀死
// 整个测试进程（RangeLiarServer/TlsTestServer 既有教训，MSG_NOSIGNAL 路线）
constexpr send_flags kSendFlags =
#if defined(MSG_NOSIGNAL)
    MSG_NOSIGNAL;
#else
    0;
#endif
#endif

/// 慢速 + 支持 Range 的回环 HTTP 服务器（B11 复现专用）
class RangeSlowServer {
public:
    // 断言失败提前 return 时仍要收线（存活连接线程析构 terminate 防护，
    // 沿 RawWsServer/挂死模式修复先例）
    ~RangeSlowServer() { stop(); }

    bool start(std::string body) {
#ifdef _WIN32
        ensure_winsock_for_pause_test();
#endif
        body_ = std::move(body);

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
#ifdef _WIN32
            ::shutdown(listen_fd_, SD_BOTH);
#else
            ::shutdown(listen_fd_, SHUT_RDWR);
#endif
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
    }

    int port() const { return port_; }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    /// 观测：收到的全部 GET Range 头值（空串 = 无 Range 全量请求）
    std::vector<std::string> range_requests() {
        std::lock_guard<std::mutex> lock(log_mu_);
        return range_log_;
    }

    /// 等待谓词：出现非空 Range 请求（断点续传真实到达服务器的铁证）
    bool saw_ranged_request() {
        std::lock_guard<std::mutex> lock(log_mu_);
        return std::any_of(range_log_.begin(), range_log_.end(),
                           [](const std::string& r) { return !r.empty(); });
    }

    /// 慢发节奏：每 chunk_delay_ms 发 chunk_size 字节（0 = 全速）
    void set_slow_send(std::size_t chunk_size, int chunk_delay_ms) {
        chunk_size_ = chunk_size;
        chunk_delay_ms_ = chunk_delay_ms;
    }

private:
    void accept_loop() {
        // poll 轮询 running_：Linux 上 close(fd) 不唤醒阻塞 accept（既有教训）
        while (running_) {
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            const int ready = POLL(&pfd, 1, 200);
            if (ready <= 0) continue;
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

        const bool is_head = request.rfind("HEAD", 0) == 0;
        const bool is_get = request.rfind("GET", 0) == 0;

        // Range 头解析（大小写不敏感的简化查找；只取第一次出现）
        std::string range_value;
        std::size_t pos = 0;
        while (true) {
            pos = request.find("Range:", pos);
            if (pos == std::string::npos) break;
            // 行首校验（避免匹配 "X-Range:" 类头）
            if (pos == 0 || request[pos - 1] == '\n') {
                std::size_t eol = request.find("\r\n", pos);
                if (eol == std::string::npos) break;
                std::string line = request.substr(pos, eol - pos);
                auto colon = line.find(':');
                range_value = line.substr(colon + 1);
                // 去空白
                while (!range_value.empty() &&
                       (range_value.front() == ' ' || range_value.front() == '\t')) {
                    range_value.erase(range_value.begin());
                }
                break;
            }
            pos += 6;
        }

        if (!is_get) {
            // 记录请求（含 HEAD）以维持观测语义；Range 观测只针对 GET
        } else {
            std::lock_guard<std::mutex> lock(log_mu_);
            range_log_.push_back(range_value);
        }

        if (is_head) {
            std::string header =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: " + std::to_string(body_.size()) + "\r\n"
                "Accept-Ranges: bytes\r\n"
                "Connection: close\r\n\r\n";
            send_all(conn, header.data(), header.size());
            CLOSE_SOCKET(conn);
            return;
        }

        if (!is_get) {
            CLOSE_SOCKET(conn);
            return;
        }

        // Range 解析：bytes=N- / bytes=N-M（curl RESUME_FROM 与
        // SegmentDownloader 两种形态）。N-M 的 end 边界必须消费——
        // 只取 start 会把闭区间按「start→EOF」应答，Content-Range
        // 自洽声明却与请求不符，段文件超出段长被产品侧精确尺寸闸门
        // 拒绝（B11 断点续传钉子曾因此必红）
        std::size_t start = 0;
        std::size_t end = body_.size() - 1;  // 闭区间
        bool has_range = false;
        if (range_value.rfind("bytes=", 0) == 0) {
            std::string spec = range_value.substr(6);
            auto dash = spec.find('-');
            if (dash != std::string::npos) {
                std::string s = spec.substr(0, dash);
                if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos) {
                    start = static_cast<std::size_t>(std::stoull(s));
                    std::string e = spec.substr(dash + 1);
                    if (!e.empty() && e.find_first_not_of("0123456789") == std::string::npos) {
                        end = std::min<std::size_t>(std::stoull(e), body_.size() - 1);
                    }
                    if (start < body_.size()) {
                        has_range = true;
                    }
                }
            }
        }

        if (has_range) {
            std::string header =
                "HTTP/1.1 206 Partial Content\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Range: bytes " + std::to_string(start) + "-" +
                std::to_string(end) + "/" + std::to_string(body_.size()) + "\r\n"
                "Content-Length: " + std::to_string(end - start + 1) + "\r\n"
                "Accept-Ranges: bytes\r\n"
                "Connection: close\r\n\r\n";
            send_all(conn, header.data(), header.size());
            send_slow(conn, body_.data() + start, end - start + 1);
        } else {
            std::string header =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: " + std::to_string(body_.size()) + "\r\n"
                "Accept-Ranges: bytes\r\n"
                "Connection: close\r\n\r\n";
            send_all(conn, header.data(), header.size());
            send_slow(conn, body_.data(), body_.size());
        }
        CLOSE_SOCKET(conn);
    }

    void send_slow(int conn, const char* data, std::size_t size) {
        if (chunk_size_ == 0) {
            send_all(conn, data, size);
            return;
        }
        std::size_t sent = 0;
        while (sent < size) {
            const std::size_t n = std::min(chunk_size_, size - sent);
            if (!send_all(conn, data + sent, n)) return;
            sent += n;
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk_delay_ms_));
        }
    }

    bool send_all(int conn, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            recv_ssize n = ::send(conn, data + sent,
#ifdef _WIN32
                                  static_cast<int>(size - sent),
#else
                                  size - sent,
#endif
                                  kSendFlags);
            if (n <= 0) return false;  // 客户端暂停中止连接：写失败即退出
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::string body_;
    std::size_t chunk_size_ = 4 * 1024;
    int chunk_delay_ms_ = 4;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
    std::mutex log_mu_;
    std::vector<std::string> range_log_;
};

std::string make_body(std::size_t size) {
    std::string body;
    body.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<char>('a' + (i % 26)));
    }
    return body;
}

std::string temp_output_dir(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            ("falcon_pause_test_" + std::string(tag) + "_" +
             std::to_string(test_getpid())))
        .string();
}

EngineConfig engine_config_one_slot() {
    EngineConfig config;
    config.max_concurrent_tasks = 1;
    return config;
}

/// 轮询等待任务进度达到阈值（progress 回调 200ms 节流，谓词预算 15s）。
/// 失败时输出终态与错误消息辅助定位（诊断输出，不在此断言）。
bool wait_downloaded_at_least(DownloadTask* task, Bytes threshold) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        // Pending 属合法活动态：start_task 返回后 worker 出队是异步的，
        // 任务可能在队列里停留数十 ms（首版谓词漏掉 Pending 一迭代即假失败）
        const TaskStatus st = task->status();
        if (st != TaskStatus::Pending && st != TaskStatus::Preparing &&
            st != TaskStatus::Downloading) {
            GTEST_LOG_(INFO) << "任务意外离开活动态：status=" << static_cast<int>(st)
                             << " downloaded=" << task->downloaded_bytes()
                             << " error=\"" << task->error_message() << "\"";
            return false;  // 任务意外离开活动态
        }
        if (task->downloaded_bytes() >= threshold) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    GTEST_LOG_(INFO) << "进度等待超时：status=" << static_cast<int>(task->status())
                     << " downloaded=" << task->downloaded_bytes()
                     << " error=\"" << task->error_message() << "\"";
    return false;
}

/// TearDown 统一收线：任何 ASSERT 提前 return 都走这里（cancel → stop server
/// → 清目录），不会把存活线程留给析构期 terminate
class PauseResumeE2E : public ::testing::Test {
protected:
    void TearDown() override {
        if (engine) engine->cancel_all();
        server.stop();
        if (!output_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(output_dir, ec);
        }
    }

    RangeSlowServer server;
    std::unique_ptr<DownloadEngine> engine;
    std::filesystem::path output_dir;
};

} // namespace

/// 单连接路径：下载中暂停 → 继续 → 从断点续传（Range 请求到达服务器）→ 完成
TEST_F(PauseResumeE2E, SingleConnectionPauseThenResumeCompletes) {
    const std::size_t kSize = 512 * 1024;
    ASSERT_TRUE(server.start(make_body(kSize)));
    server.set_slow_send(4 * 1024, 15);  // ~266KB/s → 全程 ~2s，暂停窗口充足

    engine = std::make_unique<DownloadEngine>(engine_config_one_slot());
    engine->register_handler(protocols::create_http_handler());

    DownloadOptions options;
    output_dir = temp_output_dir("single");
    options.output_directory = output_dir.string();
    options.output_filename = "single.bin";
    options.max_connections = 1;  // 单连接路径
    std::filesystem::create_directories(options.output_directory);

    auto task = engine->add_task(server.url("/single.bin"), options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine->start_task(task->id()));

    // 等首批进度（64KB ≈ 0.25s 处），此时下载必然已进入数据阶段
    ASSERT_TRUE(wait_downloaded_at_least(task.get(), 64 * 1024))
        << "下载未按慢发节奏推进";

    ASSERT_TRUE(engine->pause_task(task->id())) << "pause_task 应成功";
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    const Bytes paused_at = task->downloaded_bytes();
    EXPECT_GT(paused_at, 0u);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(task->status(), TaskStatus::Paused) << "暂停后状态应保持稳定";

    ASSERT_TRUE(engine->resume_task(task->id())) << "resume_task 应成功";

    EXPECT_TRUE(task->wait_for(std::chrono::seconds(60)));
    EXPECT_EQ(task->status(), TaskStatus::Completed)
        << "暂停→继续后任务应完成（B11 用户报「无法再恢复」）";

    // 断点续传铁证：继续后服务器必须收到带 Range 的请求
    EXPECT_TRUE(server.saw_ranged_request())
        << "继续后应从断点续传（服务器应收到 Range 请求）";

    // 成品逐字节一致
    const std::filesystem::path final_path = output_dir / "single.bin";
    ASSERT_TRUE(std::filesystem::exists(final_path));
    std::string got(kSize, '\0');
    {
        std::ifstream f(final_path, std::ios::binary);
        f.read(got.data(), static_cast<std::streamsize>(got.size()));
        ASSERT_EQ(f.gcount(), static_cast<std::streamsize>(kSize));
    }
    const std::string want = make_body(kSize);
    EXPECT_TRUE(got == want) << "成品内容应与源数据逐字节一致";
}

/// 分段路径（用户默认场景：桌面 max_connections=4 + 大文件）：
/// 下载中暂停 → 继续 → 完成 + 逐字节一致。
/// 观测打印暂停时进度与继续后的 Range 使用（诊断「进度归零」面）。
TEST_F(PauseResumeE2E, SegmentedPauseThenResumeCompletes) {
    const std::size_t kSize = 8 * 1024 * 1024;  // 4 段 × 2MB（默认 min_segment 1MB）
    ASSERT_TRUE(server.start(make_body(kSize)));
    server.set_slow_send(4 * 1024, 6);  // ~666KB/s 单连接，4 并发 ~2.6MB/s

    engine = std::make_unique<DownloadEngine>(engine_config_one_slot());
    engine->register_handler(protocols::create_http_handler());

    DownloadOptions options;
    output_dir = temp_output_dir("seg");
    options.output_directory = output_dir.string();
    options.output_filename = "segmented.bin";
    options.max_connections = 4;  // 桌面默认 → 分段路径
    std::filesystem::create_directories(options.output_directory);

    auto task = engine->add_task(server.url("/segmented.bin"), options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine->start_task(task->id()));

    ASSERT_TRUE(wait_downloaded_at_least(task.get(), 1024 * 1024))
        << "分段下载未按慢发节奏推进";

    ASSERT_TRUE(engine->pause_task(task->id())) << "pause_task 应成功";
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    const Bytes paused_at = task->downloaded_bytes();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    ASSERT_TRUE(engine->resume_task(task->id())) << "resume_task 应成功";

    EXPECT_TRUE(task->wait_for(std::chrono::seconds(90)));
    if (task->status() != TaskStatus::Completed) {
        GTEST_LOG_(INFO) << "分段任务终态非 Completed：status="
                         << static_cast<int>(task->status())
                         << " downloaded=" << task->downloaded_bytes()
                         << "/" << task->total_bytes()
                         << " error=\"" << task->error_message() << "\"";
        // 失败面诊断：全部请求形态 + 输出目录现场（断言前打印）
        {
            const auto ranges = server.range_requests();
            for (std::size_t i = 0; i < ranges.size(); ++i) {
                GTEST_LOG_(INFO) << "  请求[" << i << "] Range=\""
                                 << ranges[i] << "\"";
            }
            std::error_code ec;
            for (const auto& entry :
                 std::filesystem::directory_iterator(output_dir, ec)) {
                GTEST_LOG_(INFO) << "  现场文件 " << entry.path().filename()
                                 << " size=" << entry.file_size(ec);
            }
        }
    }
    EXPECT_EQ(task->status(), TaskStatus::Completed)
        << "分段任务暂停→继续后应完成（用户场景：nightly AppImage 直链）";

    const std::filesystem::path final_path = output_dir / "segmented.bin";
    ASSERT_TRUE(std::filesystem::exists(final_path));
    EXPECT_EQ(std::filesystem::file_size(final_path), kSize);
    std::string got(kSize, '\0');
    {
        std::ifstream f(final_path, std::ios::binary);
        f.read(got.data(), static_cast<std::streamsize>(got.size()));
        ASSERT_EQ(f.gcount(), static_cast<std::streamsize>(kSize));
    }
    const std::string want = make_body(kSize);
    EXPECT_TRUE(got == want) << "成品内容应与源数据逐字节一致";

    // 诊断输出（非断言）：继续后的请求形态
    {
        const auto ranges = server.range_requests();
        std::size_t ranged = 0;
        for (const auto& r : ranges) {
            if (!r.empty()) ++ranged;
        }
        GTEST_LOG_(INFO) << "暂停点 downloaded=" << paused_at
                         << "，服务器共收到 GET " << ranges.size()
                         << " 个（带 Range " << ranged << " 个）";
    }
}

/// 多次反复：暂停→继续×3 → 完成 + 逐字节一致（用户验收口径「多次反复」）
TEST_F(PauseResumeE2E, RepeatedPauseResumeCyclesComplete) {
    const std::size_t kSize = 6 * 1024 * 1024;
    ASSERT_TRUE(server.start(make_body(kSize)));
    server.set_slow_send(4 * 1024, 6);

    engine = std::make_unique<DownloadEngine>(engine_config_one_slot());
    engine->register_handler(protocols::create_http_handler());

    DownloadOptions options;
    output_dir = temp_output_dir("repeat");
    options.output_directory = output_dir.string();
    options.output_filename = "repeated.bin";
    options.max_connections = 4;
    std::filesystem::create_directories(options.output_directory);

    auto task = engine->add_task(server.url("/repeated.bin"), options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine->start_task(task->id()));

    for (int cycle = 1; cycle <= 3; ++cycle) {
        ASSERT_TRUE(wait_downloaded_at_least(
            task.get(), static_cast<Bytes>(cycle) * 512 * 1024))
            << "第 " << cycle << " 轮等待进度失败";
        ASSERT_TRUE(engine->pause_task(task->id())) << "第 " << cycle << " 轮暂停失败";
        EXPECT_EQ(task->status(), TaskStatus::Paused);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ASSERT_TRUE(engine->resume_task(task->id()))
            << "第 " << cycle << " 轮继续失败";
        // resume → worker 出队启动是异步的（状态短暂停留 Pending 属
        // 正常），立即断言 Downloading 是竞速断言——轮询等待进入
        // 下载中（Failed 提前退出，交给终态断言报错）
        bool reached = false;
        for (int i = 0; i < 500 && !reached; ++i) {
            const auto st = task->status();
            if (st == TaskStatus::Downloading) {
                reached = true;
            } else if (st == TaskStatus::Failed) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_TRUE(reached) << "第 " << cycle << " 轮继续后应进入下载中";
    }

    EXPECT_TRUE(task->wait_for(std::chrono::seconds(90)));
    EXPECT_EQ(task->status(), TaskStatus::Completed)
        << "三次反复暂停→继续后任务应完成";

    const std::filesystem::path final_path = output_dir / "repeated.bin";
    ASSERT_TRUE(std::filesystem::exists(final_path));
    std::string got(kSize, '\0');
    {
        std::ifstream f(final_path, std::ios::binary);
        f.read(got.data(), static_cast<std::streamsize>(got.size()));
        ASSERT_EQ(f.gcount(), static_cast<std::streamsize>(kSize));
    }
    EXPECT_TRUE(got == make_body(kSize));
}
