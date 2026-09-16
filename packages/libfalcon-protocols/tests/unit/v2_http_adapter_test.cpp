/**
 * @file v2_http_adapter_test.cpp
 * @brief V2 HTTP 适配层测试——V1/V2 参数化等价对照 + 回退表 + 宿主生命周期
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 参数化组（WithParamInterface<bool>：false = V1 curl 数据面，
 * true = V2 引擎数据面）对同一组断言跑两遍，锁定桥接等价性：
 * - 下载完成 + 事件序列（on_file_info 先于 on_progress、终态回调）
 * - 暂停/恢复（V2 侧含停机排水 → 引擎重建 → 控制文件续传）
 * - 取消、404 错误传播、多分段
 * worker 收口路径忠实复刻 task_manager.cpp（catch → set_error +
 * set_status(Failed)），两侧事件序列逐一对齐。
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

#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/protocols/v2_engine_host.hpp>
#include "http_handler.hpp"
#include "v2_http_download_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;
using falcon::protocols::HttpHandler;

namespace {

#ifdef _WIN32
void ensure_winsock_for_adapter_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int adapter_test_getpid() { return _getpid(); }
using sock_len = int;
using recv_ssize = int;
#else
inline int adapter_test_getpid() { return static_cast<int>(::getpid()); }
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
            (std::string("falcon_v2_adapter_") + tag + "_" +
             std::to_string(adapter_test_getpid())))
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

/// 简单事件记录器（回调全部来自 downloader 线程，join 后读取）
class RecordingListener : public IEventListener {
public:
    void on_status_changed(TaskId, TaskStatus old_status,
                           TaskStatus new_status) override {
        events_.push_back(std::string("status:") + to_string(old_status) +
                          "->" + to_string(new_status));
    }
    void on_progress(const ProgressInfo& info) override {
        (void)info;
        events_.push_back("progress");
    }
    void on_error(TaskId, const std::string& message) override {
        events_.push_back("error:" + message);
    }
    void on_completed(TaskId, const std::string&) override {
        events_.push_back("completed");
    }
    void on_file_info(TaskId, const FileInfo&) override {
        events_.push_back("file_info");
    }

    const std::vector<std::string>& events() const { return events_; }

private:
    std::vector<std::string> events_;
};

/// 忠实复刻 TaskManager worker 的 download 收口（task_manager.cpp:834）
bool download_via_worker(HttpHandler& handler, const DownloadTask::Ptr& task,
                         IEventListener* listener) {
    try {
        handler.download(task, listener);
        return true;
    } catch (const std::exception& e) {
        task->set_error(e.what());
        task->set_status(TaskStatus::Failed);
        return false;
    }
}

/**
 * @brief 回环 HTTP 测试服务器
 *
 * HEAD → 200 + Content-Length + Accept-Ranges；
 * GET 无 Range → 200 全量；GET Range: bytes=A-B / A- → 206 区间。
 * /slow.bin 分块慢发（暂停窗口），其余路径快发；/missing 404。
 * 串行服务连接（每连接读请求头 → 应答 → 关闭）。
 */
class AdapterTestServer {
public:
    ~AdapterTestServer() { stop(); }

    bool start() {
#ifdef _WIN32
        ensure_winsock_for_adapter_test();
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        int reuse = 1;
        (void)setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            stop();
            return false;
        }

        sockaddr_in bound{};
        sock_len len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound),
                          &len) != 0) {
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
    }

    int port() const { return port_; }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    int connections() const { return connections_.load(); }

    void set_body(std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        body_ = std::move(body);
    }

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
            int conn = ::accept(listen_fd_,
                                reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (conn < 0) {
                if (!running_) return;
                continue;
            }
            connections_.fetch_add(1);
            serve(conn);
        }
    }

    static void set_socket_timeout(int conn, int seconds) {
        timeval tv{};
        tv.tv_sec = seconds;
#ifdef _WIN32
        (void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&tv), sizeof(tv));
        (void)setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO,
                         reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        (void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    static bool send_all(int conn, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const int flags = 0;
#ifdef _WIN32
            const int n = ::send(conn, data + sent,
                                 static_cast<int>(size - sent), flags);
#else
            const ssize_t n = ::send(conn, data + sent, size - sent,
                                     flags | MSG_NOSIGNAL);
#endif
            if (n <= 0) return false;  // 客户端断开（暂停中止的服务器侧证据）
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    void serve(int conn) {
        set_socket_timeout(conn, 10);

        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64 * 1024) {
#ifdef _WIN32
            const int n = ::recv(conn, buf, sizeof(buf), 0);
#else
            const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
#endif
            if (n <= 0) {
                CLOSE_SOCKET(conn);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }

        const std::size_t line_end = request.find("\r\n");
        const std::string request_line = request.substr(0, line_end);
        const std::size_t sp1 = request_line.find(' ');
        const std::size_t sp2 =
            sp1 == std::string::npos ? std::string::npos
                                     : request_line.find(' ', sp1 + 1);
        const std::string method = request_line.substr(0, sp1);
        std::string path = request_line.substr(
            sp1 + 1, sp2 == std::string::npos ? std::string::npos
                                              : sp2 - sp1 - 1);
        const std::size_t query = path.find('?');
        if (query != std::string::npos) path.resize(query);

        std::string range_value;
        std::size_t pos = request.find("\r\n");
        while (pos != std::string::npos) {
            const std::size_t next = request.find("\r\n", pos + 2);
            const std::string line = request.substr(
                pos + 2, next == std::string::npos
                             ? std::string::npos
                             : next - pos - 2);
            std::string lower;
            lower.reserve(line.size());
            for (char c : line) {
                lower.push_back(
                    static_cast<char>(::tolower(static_cast<unsigned char>(c))));
            }
            if (lower.rfind("range:", 0) == 0) {
                range_value = line.substr(6);
            }
            if (line.empty()) break;
            pos = next;
        }

        std::string body;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body = body_;
        }

        if (path == "/missing" ||
            (path == "/bait" && method == "GET")) {
            // /bait：HEAD 放行、GET 404——探测通过后失败才发生在数据面
            // （V2 组 FAILED 路径而非 HEAD 探测层）
            static const char kNotFound[] = "HTTP/1.1 404 Not Found\r\n"
                                            "Content-Length: 0\r\n"
                                            "Connection: close\r\n\r\n";
            (void)send_all(conn, kNotFound, sizeof(kNotFound) - 1);
            CLOSE_SOCKET(conn);
            return;
        }

        const std::string common_headers =
            "Accept-Ranges: bytes\r\nConnection: close\r\n";

        if (method == "HEAD") {
            const std::string header =
                "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n" +
                common_headers +
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            (void)send_all(conn, header.data(), header.size());
            CLOSE_SOCKET(conn);
            return;
        }

        // GET：解析 Range（bytes=A-B / bytes=A-）
        std::size_t start = 0;
        std::size_t end = body.empty() ? 0 : body.size() - 1;
        bool partial = false;
        if (range_value.find("bytes=") != std::string::npos) {
            const std::size_t eq = range_value.find('=') + 1;
            const std::size_t dash = range_value.find('-', eq);
            if (dash != std::string::npos) {
                const std::string a = range_value.substr(eq, dash - eq);
                const std::string b = range_value.substr(dash + 1);
                start = static_cast<std::size_t>(std::stoull(a));
                if (!b.empty()) {
                    end = static_cast<std::size_t>(std::stoull(b));
                }
                if (start < body.size() && end < body.size() && start <= end) {
                    partial = true;
                }
            }
        }

        const std::string slice =
            body.substr(start, partial ? end - start + 1 : std::string::npos);

        std::string header;
        if (partial) {
            header = "HTTP/1.1 206 Partial Content\r\n"
                     "Content-Type: application/octet-stream\r\n" +
                     common_headers + "Content-Range: bytes " +
                     std::to_string(start) + "-" + std::to_string(end) + "/" +
                     std::to_string(body.size()) + "\r\nContent-Length: " +
                     std::to_string(slice.size()) + "\r\n\r\n";
        } else {
            header = "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/octet-stream\r\n" +
                     common_headers + "Content-Length: " +
                     std::to_string(slice.size()) + "\r\n\r\n";
        }
        if (!send_all(conn, header.data(), header.size())) {
            CLOSE_SOCKET(conn);
            return;
        }

        // /slow.bin 分块慢发（暂停窗口），其余路径一次发完
        if (path == "/slow.bin") {
            constexpr std::size_t kChunk = 8 * 1024;
            for (std::size_t off = 0; off < slice.size(); off += kChunk) {
                if (!send_all(conn, slice.data() + off,
                              std::min(kChunk, slice.size() - off))) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        } else {
            (void)send_all(conn, slice.data(), slice.size());
        }
        CLOSE_SOCKET(conn);
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> connections_{0};
    std::mutex mutex_;
    std::string body_;
    std::thread accept_thread_;
};

}  // namespace

// ============================================================================
// 参数化等价对照（false = V1 curl，true = V2 引擎）
// ============================================================================

namespace {

class V2HttpAdapterEquivalence : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (GetParam()) {
            V2EngineHost::instance().set_v2_http_enabled(true);
        }
    }
    void TearDown() override {
        V2EngineHost::instance().shutdown_and_join();
        V2EngineHost::instance().set_v2_http_enabled(false);
    }
    [[nodiscard]] bool v2() const { return GetParam(); }

    DownloadTask::Ptr make_task(TaskId id, const std::string& url,
                                const std::string& output_path,
                                const DownloadOptions& options) const {
        auto task = std::make_shared<DownloadTask>(id, url, options);
        task->set_output_path(output_path);
        return task;
    }
};

TEST_P(V2HttpAdapterEquivalence, DownloadCompletesWithEventOrder) {
    const std::string body = make_body(128 * 1024);
    AdapterTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string dir = temp_dir_for("complete");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.progress_interval_ms = 50;

    HttpHandler handler;
    RecordingListener listener;
    auto task = make_task(1, server.url("/f.bin"), out_path, options);
    task->set_listener(&listener);  // set_file_info/set_status 的事件出口
    task->set_status(TaskStatus::Downloading);  // 模拟 TaskManager 调度

    EXPECT_TRUE(download_via_worker(handler, task, &listener));
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file_content(out_path), body);

    const auto& events = listener.events();
    const auto first_progress =
        std::find(events.begin(), events.end(), std::string("progress"));
    const auto first_fi = std::find(events.begin(), events.end(),
                                    std::string("file_info"));
    EXPECT_NE(first_fi, events.end()) << "on_file_info 必须发生";
    if (first_progress != events.end()) {
        EXPECT_TRUE(first_fi < first_progress)
            << "on_file_info 必须先于首个 on_progress";
    }
    EXPECT_NE(std::find(events.begin(), events.end(),
                        std::string("status:Downloading->Completed")),
              events.end());

    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

TEST_P(V2HttpAdapterEquivalence, NotFoundPropagatesAsFailure) {
    AdapterTestServer server;
    ASSERT_TRUE(server.start());

    const std::string dir = temp_dir_for("notfound");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    HttpHandler handler;
    RecordingListener listener;
    auto task = make_task(2, server.url("/missing"), out_path, options);
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    EXPECT_FALSE(download_via_worker(handler, task, &listener));
    EXPECT_EQ(task->status(), TaskStatus::Failed);
    EXPECT_FALSE(task->error_message().empty());
    EXPECT_TRUE(read_file_content(out_path).empty())
        << "失败后半成品不得顶最终名";

    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

TEST_P(V2HttpAdapterEquivalence, MultiSegmentDownloadConsistent) {
    const std::string body = make_body(64 * 1024);
    AdapterTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string dir = temp_dir_for("segments");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 4;
    options.max_retries = 0;
    options.min_segment_size = 1024;

    HttpHandler handler;
    RecordingListener listener;
    auto task = make_task(3, server.url("/f.bin"), out_path, options);
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    EXPECT_TRUE(download_via_worker(handler, task, &listener));
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file_content(out_path), body);
    EXPECT_GE(server.connections(), 1);

    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

TEST_P(V2HttpAdapterEquivalence, PauseResumeCycle) {
    const std::string body = make_body(512 * 1024);
    AdapterTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string dir = temp_dir_for("pause");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.progress_interval_ms = 50;

    HttpHandler handler;
    RecordingListener listener;
    auto task = make_task(4, server.url("/slow.bin"), out_path, options);
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread downloader(
        [&] { (void)download_via_worker(handler, task, &listener); });

    // 传输启动后暂停（服务器慢发留窗口）
    ASSERT_TRUE(wait_for([&] { return task->downloaded_bytes() > 0; }, 15000));
    handler.pause(task);
    downloader.join();

    EXPECT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(std::filesystem::exists(out_path))
        << "暂停后成品不得发布";

    // V2 侧：模拟进程重启——排水停机后引擎重建，恢复走控制文件断点
    if (v2()) {
        V2EngineHost::instance().shutdown_and_join();
    }

    task->set_status(TaskStatus::Downloading);  // resume 重新调度
    EXPECT_TRUE(download_via_worker(handler, task, &listener));
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file_content(out_path), body)
        << "恢复后成品必须逐字节一致（断点续传不损坏内容）";

    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

TEST_P(V2HttpAdapterEquivalence, CancelDuringTransfer) {
    const std::string body = make_body(512 * 1024);
    AdapterTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string dir = temp_dir_for("cancel");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    HttpHandler handler;
    RecordingListener listener;
    auto task = make_task(5, server.url("/slow.bin"), out_path, options);
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread downloader(
        [&] { (void)download_via_worker(handler, task, &listener); });

    ASSERT_TRUE(wait_for([&] { return task->downloaded_bytes() > 0; }, 15000));
    handler.cancel(task);
    downloader.join();

    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(out_path))
        << "取消后成品不得发布";

    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// HEAD 探测通过、GET 数据面 404：失败发生在引擎任务组内（区别于
// NotFoundPropagatesAsFailure 的 HEAD 层失败）——适配器轮询观察到组
// FAILED → sync_final_progress 后 throw 组错误消息（V1 worker catch
// 统一收口 Failed）
TEST_P(V2HttpAdapterEquivalence, MidDownloadFailurePropagatesGroupError) {
    const std::string body = make_body(64 * 1024);
    AdapterTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string dir = temp_dir_for("bait");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    HttpHandler handler;
    RecordingListener listener;
    auto task = make_task(6, server.url("/bait"), out_path, options);
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    EXPECT_FALSE(download_via_worker(handler, task, &listener));
    EXPECT_EQ(task->status(), TaskStatus::Failed);
    EXPECT_FALSE(task->error_message().empty());
    EXPECT_FALSE(std::filesystem::exists(out_path))
        << "失败后半成品不得顶最终名";

    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 同一 V2 引擎实例内的暂停→恢复（不经停机排水重建——区别于
// PauseResumeCycle 的 shutdown_and_join 路径）：恢复的 download() 里
// find_group 命中 PAUSED 组 → resume_task 续跑断点（而非重新注入）
TEST_P(V2HttpAdapterEquivalence, PauseResumeWithinSameEngine) {
    const std::string body = make_body(512 * 1024);
    AdapterTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string dir = temp_dir_for("sameengine");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.progress_interval_ms = 50;

    HttpHandler handler;
    RecordingListener listener;
    auto task = make_task(7, server.url("/slow.bin"), out_path, options);
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread downloader(
        [&] { (void)download_via_worker(handler, task, &listener); });

    ASSERT_TRUE(wait_for([&] { return task->downloaded_bytes() > 0; }, 15000));
    handler.pause(task);
    downloader.join();
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // 引擎与 PAUSED 组保持存活（无停机排水）：resume 在同实例内续跑
    task->set_status(TaskStatus::Downloading);  // resume 重新调度
    EXPECT_TRUE(download_via_worker(handler, task, &listener));
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file_content(out_path), body)
        << "同引擎续跑后成品必须逐字节一致";

    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// V2 引擎侧 pause_all（不经 V1 handler.pause——V1 任务状态仍是
// Downloading）：适配器轮询观察到组 PAUSED 分支 → V1 状态对齐 Paused
// 后挂起（run 正常返回，非异常收口）
TEST_P(V2HttpAdapterEquivalence, EnginePauseAllAlignsV1Status) {
    if (!v2()) {
        GTEST_SKIP() << "V1 curl 数据面无 V2 引擎任务组";
    }

    const std::string body = make_body(512 * 1024);
    AdapterTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string dir = temp_dir_for("enginepause");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.progress_interval_ms = 50;

    HttpHandler handler;
    RecordingListener listener;
    auto task = make_task(8, server.url("/slow.bin"), out_path, options);
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread downloader(
        [&] { (void)download_via_worker(handler, task, &listener); });

    ASSERT_TRUE(wait_for([&] { return task->downloaded_bytes() > 0; }, 15000));
    V2EngineHost::instance().engine()->pause_all();
    downloader.join();

    // 组 PAUSED 分支：V1 状态被适配器对齐为 Paused（run 正常返回）
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(std::filesystem::exists(out_path))
        << "暂停后成品不得发布";

    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

INSTANTIATE_TEST_SUITE_P(V1VsV2, V2HttpAdapterEquivalence,
                         ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "V2Engine" : "V1Curl";
                         });

// ============================================================================
// supports 回退表（curl 专属能力 → 回退 V1）
// ============================================================================

class V2HttpAdapterSupports : public ::testing::Test {
protected:
    void TearDown() override {
        V2EngineHost::instance().set_v2_http_enabled(false);
    }
};

TEST_F(V2HttpAdapterSupports, FallbackTable) {
    auto& host = V2EngineHost::instance();

    host.set_v2_http_enabled(false);
    EXPECT_FALSE(V2HttpDownloadAdapter::supports({})) << "开关关一律回退";

    host.set_v2_http_enabled(true);
    EXPECT_TRUE(V2HttpDownloadAdapter::supports({}));

    DownloadOptions options;
    options.proxy = "socks5://192.0.2.1:1080";
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options)) << "socks 代理回退";

    options = DownloadOptions{};
    options.proxy = "socks4://192.0.2.1:1080";
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options));

    options = DownloadOptions{};
    options.proxy = "https://192.0.2.1:8443";
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options)) << "HTTPS 代理回退";

    options = DownloadOptions{};
    options.proxy = "http://192.0.2.1:8080";
    EXPECT_TRUE(V2HttpDownloadAdapter::supports(options))
        << "明文 HTTP 代理 V2 已支持";

    options = DownloadOptions{};
    options.proxy = "http://alice:secret@192.0.2.1:8080";
    EXPECT_TRUE(V2HttpDownloadAdapter::supports(options))
        << "HTTP 代理带认证 V2 已支持";

    options = DownloadOptions{};
    options.proxy = "http://192.0.2.1:8080";
    options.proxy_type = "socks5";  // 类型覆盖指向 socks
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options));

    options = DownloadOptions{};
    options.cookie_file = "cookies.txt";
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options));

    options = DownloadOptions{};
    options.cookie_jar = "cookies.txt";
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options));

    options = DownloadOptions{};
    options.http_username = "user";
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options));

    options = DownloadOptions{};
    options.http_password = "pass";
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options));

    options = DownloadOptions{};
    options.referer = "http://example.com/";
    EXPECT_FALSE(V2HttpDownloadAdapter::supports(options))
        << "Referer（防盗链语义）V2 不发送，回退";
}

// ============================================================================
// V2EngineHost 生命周期
// ============================================================================

class V2EngineHostLifecycle : public ::testing::Test {
protected:
    void TearDown() override {
        V2EngineHost::instance().shutdown_and_join();
        V2EngineHost::instance().set_v2_http_enabled(false);
    }
};

TEST_F(V2EngineHostLifecycle, LazyStartConfigureAndRebuild) {
    auto& host = V2EngineHost::instance();
    host.shutdown_and_join();  // 干净起点（其他用例可能已启动）

    EXPECT_EQ(host.try_engine(), nullptr) << "未启动时不暴露引擎";

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    host.configure(config);

    auto first = host.engine();
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(first->config().wait_when_idle)
        << "宿主引擎强制常驻（wait_when_idle）";
    EXPECT_GE(first->config().max_concurrent_tasks, 64u)
        << "宿主并发槽位抬升，V1 TaskManager 权威排队";
    EXPECT_EQ(host.engine(), first) << "重复 engine() 返回同实例";

    host.shutdown_and_join();
    EXPECT_EQ(host.try_engine(), nullptr) << "停机后引擎实例销毁";

    auto second = host.engine();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second, first) << "halt 后的引擎不可复用，必须重建";
}

}  // namespace
