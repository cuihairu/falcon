/**
 * @file download_engine_v2_resume_test.cpp
 * @brief V2 引擎断点续传端到端测试（.falcon.ctrl 控制文件 + If-Range 防护）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 覆盖的核心不变量：
 * - 失败/中断留下的临时文件 + 控制文件可支撑重加任务从断点继续
 *   （单连接与多分段两条路径），最终成品逐字节完整、无混合无重复
 * - 服务器内容变更（If-Range 失效）或 Range 被无视（回 200 全量）
 *   时放弃续传转全新下载——旧断点数据绝不接续新内容
 * - 门禁：resume_enabled=false / overwrite_existing=true / 控制文件
 *   损坏 / 临时文件缺失，均回退全新下载且不残留续传挂点
 *
 * 测试服务器分阶段编排：第一阶段"部分响应后粗暴断连"制造断点，
 * 第二阶段按请求特征（是否带 Range）应答 206/200，并记录收到的
 * Range 起点与 If-Range 值供断言。
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
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_resume_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int resume_test_getpid() { return _getpid(); }
using sock_len = int;
using recv_ssize = int;
#else
inline int resume_test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
using recv_ssize = ssize_t;
#endif

/// 从请求头里取指定头字段的值（name 须为小写；大小写不敏感匹配）
std::string header_value(const std::string& request, const std::string& name) {
    std::size_t pos = 0;
    while (pos < request.size()) {
        auto eol = request.find('\n', pos);
        if (eol == std::string::npos) {
            eol = request.size();
        }
        std::string_view line(request.data() + pos, eol - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.size() > name.size() &&
            line[name.size()] == ':') {
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) != name[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string value(line.substr(name.size() + 1));
                while (!value.empty() &&
                       (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(value.begin());
                }
                while (!value.empty() &&
                       (value.back() == ' ' || value.back() == '\t')) {
                    value.pop_back();
                }
                return value;
            }
        }
        pos = eol + 1;
    }
    return {};
}

/// 解析 Range 头（"bytes=4096-16383" 或开放末端 "bytes=4096-"），
/// has_end 为假表示开放末端（到文件尾）
bool parse_range_header(const std::string& value, Bytes& start,
                        Bytes& end, bool& has_end) {
    const auto pos = value.find("bytes=");
    if (pos == std::string::npos) {
        return false;
    }
    std::size_t i = pos + 6;
    while (i < value.size() && !std::isdigit(static_cast<unsigned char>(value[i]))) {
        ++i;
    }
    if (i >= value.size()) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        start = static_cast<Bytes>(std::stoull(value.substr(i), &consumed));
        if (consumed == 0) {
            return false;
        }
        i += consumed;
        has_end = false;
        end = 0;
        if (i < value.size() && value[i] == '-') {
            ++i;
            if (i < value.size() &&
                std::isdigit(static_cast<unsigned char>(value[i]))) {
                end = static_cast<Bytes>(std::stoull(value.substr(i), &consumed));
                if (consumed > 0) {
                    has_end = true;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * @brief 断点续传测试服务器
 *
 * 对每个请求按特征应答并记录观测值：
 * - 带 Range → 206 + Content-Range + 剩余字节（range_liar_ 时伪装成
 *   200 全量——模拟"宣称 Accept-Ranges 却无视 Range"的服务器）
 * - 不带 Range → 200 全量；partial_first_bytes_ > 0 时只发前 N 字节
 *   后立即断连（制造断点的第一触发）
 * - set_resource 可在中途替换响应体与 ETag（If-Range 失效场景）
 */
class ResumeTestServer {
public:
    ~ResumeTestServer() { stop(); }  // RAII：joinable 线程析构即 terminate

    bool start(std::string body, std::string etag, std::string last_modified) {
#ifdef _WIN32
        ensure_winsock_for_resume_test();
#endif
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body_ = std::move(body);
            etag_ = std::move(etag);
            last_modified_ = std::move(last_modified);
        }

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

    // ---- 行为开关（阶段之间调用，不与连接处理并发） ---------------

    /// 非 Range 请求只发前 N 字节后断连（0 = 正常全量）
    void set_partial_first_bytes(std::size_t n) { partial_first_bytes_ = n; }

    /// 带 Range 的请求无视 Range 回 200 全量
    void set_range_liar(bool v) { range_liar_ = v; }

    /// 206 的 Content-Range 起点写成超过 uint64 的数字(服务器侧数据
    /// 损坏形态)——驱动客户端解析的 stoull 溢出防御
    void set_overflow_content_range(bool v) { overflow_content_range_ = v; }

    /// 替换响应内容与 ETag（内容变更 → If-Range 失效）
    void set_resource(std::string body, std::string etag) {
        std::lock_guard<std::mutex> lock(mutex_);
        body_ = std::move(body);
        etag_ = std::move(etag);
    }

    // ---- 观测值 ----------------------------------------------------

    std::vector<Bytes> range_starts_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return range_starts_;
    }

    std::vector<std::string> if_range_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return if_range_values_;
    }

    int plain_gets() const { return plain_gets_.load(); }
    int range_gets() const { return range_gets_.load(); }

private:
    void accept_loop() {
        // poll 带超时轮询 running_：阻塞 accept 上直接 close(fd) 在
        // Linux 不保证唤醒（复用既有测试服务器模板）
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

        const std::string range_header = header_value(request, "range");
        Bytes range_start = 0;
        Bytes range_end = 0;
        bool has_end = false;
        const bool has_range = parse_range_header(range_header, range_start,
                                                  range_end, has_end);
        const std::string if_range = header_value(request, "if-range");

        std::string body;
        std::string etag;
        std::string last_modified;
        std::size_t partial = 0;
        bool liar = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body = body_;
            etag = etag_;
            last_modified = last_modified_;
            partial = partial_first_bytes_;
            liar = range_liar_;
            if (has_range) {
                range_starts_.push_back(range_start);
            }
            if (!if_range.empty()) {
                if_range_values_.push_back(if_range);
            }
        }
        if (has_range) {
            range_gets_.fetch_add(1);
        } else {
            plain_gets_.fetch_add(1);
        }

        const std::string common =
            "Content-Type: application/octet-stream\r\n"
            "Accept-Ranges: bytes\r\n"
            "ETag: " + etag + "\r\n"
            "Last-Modified: " + last_modified + "\r\n"
            "Connection: close\r\n";

        if (has_range && !liar && range_start < body.size()) {
            // 正规 206：按 Range 的 start-end 精确应答（有末端时绝不
            // 越过它——客户端按请求长度校验响应），Content-Range 标明
            // 实际区间
            Bytes serve_end = body.size() - 1;
            if (has_end && range_end < serve_end) {
                serve_end = range_end;
            }
            const std::string slice = body.substr(
                static_cast<std::size_t>(range_start),
                static_cast<std::size_t>(serve_end - range_start + 1));
            const std::string cr_start = overflow_content_range_
                ? std::string("99999999999999999999999")  // > uint64:stoull 溢出
                : std::to_string(range_start);
            std::string header =
                "HTTP/1.1 206 Partial Content\r\n" + common +
                "Content-Range: bytes " + cr_start + "-" +
                std::to_string(serve_end) + "/" +
                std::to_string(body.size()) + "\r\n"
                "Content-Length: " + std::to_string(slice.size()) + "\r\n\r\n";
            send_all(conn, header.data(), header.size());
            send_all(conn, slice.data(), slice.size());
            CLOSE_SOCKET(conn);
            return;
        }

        // 200 全量（无 Range / Range 撒谎 / If-Range 失效由客户端侧 If-Range
        // 请求头触发——服务器按 RFC 7233 回全量新内容）
        std::string header =
            "HTTP/1.1 200 OK\r\n" + common +
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        send_all(conn, header.data(), header.size());
        if (!has_range && partial > 0 && partial < body.size()) {
            // 部分响应后立即断连：制造断点的第一触发
            send_all(conn, body.data(), partial);
            CLOSE_SOCKET(conn);
            return;
        }
        send_all(conn, body.data(), body.size());
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

    mutable std::mutex mutex_;
    std::string body_;
    std::string etag_;
    std::string last_modified_;
    std::vector<Bytes> range_starts_;
    std::vector<std::string> if_range_values_;
    std::size_t partial_first_bytes_ = 0;
    bool range_liar_ = false;
    bool overflow_content_range_ = false;

    std::atomic<int> plain_gets_{0};
    std::atomic<int> range_gets_{0};

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

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
            (std::string("falcon_v2_resume_") + tag + "_" +
             std::to_string(resume_test_getpid())))
        .string();
}

/// 单连接下载选项（续传基础路径）
DownloadOptions single_connection_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;  // 聚焦续传链路：失败立即终态
    return options;
}

/// 跑一次下载直到任务组终态；返回是否在时限内观察到终态
bool run_until_terminal(DownloadEngineV2& engine, RequestGroup* group,
                        int timeout_seconds) {
    std::thread runner([&engine] { engine.run(); });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto st = group->status();
        if (st == RequestGroupStatus::COMPLETED || st == RequestGroupStatus::FAILED) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!finished) {
        engine.force_shutdown();
    }
    runner.join();
    return finished;
}

/// 断点续传两阶段流程：第一阶段部分响应后断连 → FAILED，断言断点
/// 已落盘（临时文件含前缀 + 控制文件存在）；返回输出路径
std::string run_failing_first_phase(ResumeTestServer& server,
                                    const std::string& out_path,
                                    const DownloadOptions& options) {
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    EXPECT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    EXPECT_NE(group, nullptr);
    EXPECT_TRUE(run_until_terminal(engine, group, 20));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    // 断点已固化：临时文件含已下载前缀，控制文件就位
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(out_path + kResumeControlExtension, ec))
        << "失败收口应写出控制文件";
    return out_path;
}

} // namespace

/// 单连接续传端到端：断点后重加任务 → 服务器收到带断点起点的 Range
/// + If-Range → 206 接续 → 成品逐字节一致，续传挂点全部清除
TEST(DownloadEngineV2Resume, SingleConnectionContinuesFromBreakpoint) {
    const std::string body = make_body(32 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(body, "\"etag-v1\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("single");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    auto options = single_connection_options(out_path);

    server.set_partial_first_bytes(4096);
    run_failing_first_phase(server, out_path, options);

    const std::string temp_prefix = read_file_content(out_path + ".falcon.tmp");
    ASSERT_EQ(temp_prefix.size(), 4096u);
    EXPECT_EQ(temp_prefix, body.substr(0, 4096));

    // 第二阶段：断点续传
    server.set_partial_first_bytes(0);
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(group->downloaded_bytes(), body.size());
    // 成品逐字节一致：断点处接写，不覆盖前缀也不重复内容
    EXPECT_EQ(read_file_content(out_path), body);

    // 服务器看到的续传请求：Range 起点 = 断点，If-Range = 上次 ETag
    const auto starts = server.range_starts_snapshot();
    ASSERT_FALSE(starts.empty());
    EXPECT_EQ(starts.back(), 4096u)
        << "续传请求应从断点 4096 处开始";
    const auto if_ranges = server.if_range_snapshot();
    ASSERT_FALSE(if_ranges.empty());
    EXPECT_EQ(if_ranges.back(), "\"etag-v1\"");

    // 续传使命完成：临时文件（已改名）与控制文件都不再存在
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(out_path + ".falcon.tmp", ec));
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension, ec));

    std::filesystem::remove_all(dir);
}

/// 多分段续传：4 段并行下载中途断连 → 重加任务按控制文件重建分段
/// 计划，各段从各自断点（Range 起点）接续 → 成品逐字节一致
TEST(DownloadEngineV2Resume, MultiSegmentResumeRebuildsAllSegments) {
    const std::string body = make_body(64 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(body, "\"etag-multi\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("multi");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;  // 64KB → 4×16KB
    options.max_retries = 0;

    server.set_partial_first_bytes(8192);  // 初始连接（段 0）断在 8192
    run_failing_first_phase(server, out_path, options);

    const auto starts_after_phase1 = server.range_starts_snapshot();

    // 第二阶段：断点续传
    server.set_partial_first_bytes(0);
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    // 成品逐字节一致：各段在各自断点接写，段间无重叠无缝隙
    EXPECT_EQ(read_file_content(out_path), body);

    // 第二阶段新增的 Range 请求：至少包含段 0 的断点起点 8192
    //（其余段可能带各自断点或段起点）
    const auto starts_after_phase2 = server.range_starts_snapshot();
    ASSERT_GT(starts_after_phase2.size(), starts_after_phase1.size());
    bool saw_breakpoint = false;
    for (std::size_t i = starts_after_phase1.size();
         i < starts_after_phase2.size(); ++i) {
        if (starts_after_phase2[i] == 8192u) {
            saw_breakpoint = true;
        }
    }
    EXPECT_TRUE(saw_breakpoint) << "续传应携带段 0 断点起点 8192";

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension, ec));

    std::filesystem::remove_all(dir);
}

/// 内容变更防护：If-Range 失效（ETag 已变）→ 服务器按 RFC 7233 回
/// 200 全量新内容 → 客户端校验失败放弃续传 → 全新下载，成品是新内容
///（旧断点数据绝不接续新内容——混合损坏的不变量）
TEST(DownloadEngineV2Resume, ChangedContentAbandonsResumeAndDownloadsFresh) {
    const std::string old_body = make_body(32 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(old_body, "\"etag-v1\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("changed");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    auto options = single_connection_options(out_path);

    server.set_partial_first_bytes(4096);
    run_failing_first_phase(server, out_path, options);

    // 内容已变更：新 body + 新 ETag
    std::string new_body = make_body(30 * 1024);  // 长度也不同
    for (auto& ch : new_body) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    server.set_resource(new_body, "\"etag-v2\"");
    server.set_partial_first_bytes(0);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    // 成品必须是完整新内容——旧断点前缀绝不能混入
    EXPECT_EQ(read_file_content(out_path), new_body);

    // 续传请求带旧 ETag 的 If-Range；被 200 拒绝后重新发起无 Range 的
    // 全新 GET
    const auto if_ranges = server.if_range_snapshot();
    ASSERT_FALSE(if_ranges.empty());
    EXPECT_EQ(if_ranges.back(), "\"etag-v1\"");
    const auto starts = server.range_starts_snapshot();
    EXPECT_FALSE(starts.empty()) << "首次尝试应携带续传 Range";
    EXPECT_EQ(server.plain_gets() >= 2, true)
        << "放弃续传后应重新发起无 Range 的全新下载";

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension, ec));

    std::filesystem::remove_all(dir);
}

/// Range 撒谎防护：服务器无视 Range 回 200 全量（内容未变）→ 校验
/// 失败放弃续传 → 全新下载，成品完整且不重复不损坏
TEST(DownloadEngineV2Resume, RangeLiarAbandonsResumeWithoutCorruption) {
    const std::string body = make_body(32 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(body, "\"etag-v1\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("liar");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    auto options = single_connection_options(out_path);

    server.set_partial_first_bytes(4096);
    run_failing_first_phase(server, out_path, options);

    server.set_partial_first_bytes(0);
    server.set_range_liar(true);  // Range 请求一律 200 全量

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body)
        << "放弃续传后的全新下载必须产出完整一致的成品";

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension, ec));

    std::filesystem::remove_all(dir);
}

/// resume_enabled=false：已有断点也不续传，从头全新下载（对照用例）
TEST(DownloadEngineV2Resume, ResumeDisabledIgnoresBreakpoint) {
    const std::string body = make_body(32 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(body, "\"etag-v1\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("disabled");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    auto options = single_connection_options(out_path);

    server.set_partial_first_bytes(4096);
    run_failing_first_phase(server, out_path, options);

    const auto starts_after_phase1 = server.range_starts_snapshot();

    server.set_partial_first_bytes(0);
    options.resume_enabled = false;  // 显式关闭续传

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body);

    // 第二阶段零 Range 请求：请求从未携带续传语义
    const auto starts_after_phase2 = server.range_starts_snapshot();
    EXPECT_EQ(starts_after_phase2.size(), starts_after_phase1.size())
        << "resume_enabled=false 不应发出任何 Range 请求";

    std::filesystem::remove_all(dir);
}

/// 控制文件损坏：无法解析即放弃续传，全新下载不残留坏文件
TEST(DownloadEngineV2Resume, CorruptControlFileFallsBackToFresh) {
    const std::string body = make_body(32 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(body, "\"etag-v1\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("corrupt");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    auto options = single_connection_options(out_path);

    server.set_partial_first_bytes(4096);
    run_failing_first_phase(server, out_path, options);

    // 破坏控制文件
    {
        std::ofstream bad(out_path + kResumeControlExtension,
                          std::ios::binary | std::ios::trunc);
        bad << "not a falcon control file\n";
    }

    server.set_partial_first_bytes(0);
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension, ec));

    std::filesystem::remove_all(dir);
}

/// 临时文件缺失：控制文件记录的断点无处落笔，校验拒绝后续传 → 全新
TEST(DownloadEngineV2Resume, MissingTempFileInvalidatesResume) {
    const std::string body = make_body(32 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(body, "\"etag-v1\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("notemp");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    auto options = single_connection_options(out_path);

    server.set_partial_first_bytes(4096);
    run_failing_first_phase(server, out_path, options);

    // 删除临时文件：断点数据不存在，续传基础崩塌
    std::error_code ec;
    std::filesystem::remove(out_path + ".falcon.tmp", ec);

    const auto starts_after_phase1 = server.range_starts_snapshot();

    server.set_partial_first_bytes(0);
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body);

    // 临时文件缺失 → 校验拒绝 → 不携带 Range 的全新下载
    const auto starts_after_phase2 = server.range_starts_snapshot();
    EXPECT_EQ(starts_after_phase2.size(), starts_after_phase1.size());

    std::filesystem::remove_all(dir);
}

/// overwrite_existing=true：显式授权覆盖即明确要求重下，忽略已有断点
TEST(DownloadEngineV2Resume, OverwriteExistingIgnoresResume) {
    const std::string body = make_body(32 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(body, "\"etag-v1\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("overwrite");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    auto options = single_connection_options(out_path);

    server.set_partial_first_bytes(4096);
    run_failing_first_phase(server, out_path, options);

    const auto starts_after_phase1 = server.range_starts_snapshot();

    server.set_partial_first_bytes(0);
    options.overwrite_existing = true;  // 显式覆盖 = 重下

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body);

    const auto starts_after_phase2 = server.range_starts_snapshot();
    EXPECT_EQ(starts_after_phase2.size(), starts_after_phase1.size())
        << "overwrite_existing=true 不应发出任何 Range 请求";

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension, ec));

    std::filesystem::remove_all(dir);
}

/// 控制文件 save/load 往返 + 严格校验（损坏/越界/不覆盖计划一律拒绝）
TEST(ResumeControlFile, SaveLoadRoundTripAndValidation) {
    const std::string dir = temp_dir_for("ctrl");
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/file.bin.falcon.ctrl";

    ResumeControl control;
    control.url = "http://example.com/file.bin?token=abc=xyz";
    control.total = 1000;
    control.etag = "\"abc123\"";
    control.last_modified = "Wed, 10 Sep 2026 08:00:00 GMT";
    control.segments = {
        ResumeSegment{0, 400, 400},      // 已完成
        ResumeSegment{400, 600, 250},    // 进行中
    };

    EXPECT_TRUE(save_resume_control(path, control));

    ResumeControl loaded;
    EXPECT_TRUE(load_resume_control(path, loaded));
    EXPECT_EQ(loaded.url, control.url);   // 查询串中的 '=' 不破坏解析
    EXPECT_EQ(loaded.total, control.total);
    EXPECT_EQ(loaded.etag, control.etag);
    EXPECT_EQ(loaded.last_modified, control.last_modified);
    ASSERT_EQ(loaded.segments.size(), control.segments.size());
    for (std::size_t i = 0; i < control.segments.size(); ++i) {
        EXPECT_EQ(loaded.segments[i].offset, control.segments[i].offset);
        EXPECT_EQ(loaded.segments[i].length, control.segments[i].length);
        EXPECT_EQ(loaded.segments[i].downloaded, control.segments[i].downloaded);
    }

    // If-Range 取值：ETag 优先，缺省回落 Last-Modified
    EXPECT_EQ(resume_if_range_value(control), "\"abc123\"");
    ResumeControl no_etag = control;
    no_etag.etag.clear();
    EXPECT_EQ(resume_if_range_value(no_etag), control.last_modified);

    // 魔数不对 → 拒绝
    {
        std::ofstream bad(path, std::ios::binary | std::ios::trunc);
        bad << "totally-different-format\n";
    }
    EXPECT_FALSE(load_resume_control(path, loaded));

    // 段计划未覆盖全文件 → 拒绝
    control.segments = {ResumeSegment{0, 400, 100}};
    ASSERT_TRUE(save_resume_control(path, control));
    EXPECT_FALSE(load_resume_control(path, loaded));

    // 进度越段界 → 拒绝
    control.segments = {ResumeSegment{0, 400, 500},
                        ResumeSegment{400, 600, 0}};
    ASSERT_TRUE(save_resume_control(path, control));
    EXPECT_FALSE(load_resume_control(path, loaded));

    // 段序号不连续 → 拒绝
    {
        std::ofstream bad(path, std::ios::binary | std::ios::trunc);
        bad << "falcon-resume-v1\n"
            << "url=http://example.com/f\n"
            << "total=1000\n"
            << "etag=\n"
            << "last_modified=\n"
            << "segments=2\n"
            << "seg=0 0 400 100\n"
            << "seg=5 400 600 0\n";
    }
    EXPECT_FALSE(load_resume_control(path, loaded));

    // 正常文件删除后加载失败（文件不存在 = 首次下载语义）
    remove_resume_control(path);
    EXPECT_FALSE(load_resume_control(path, loaded));

    std::filesystem::remove_all(dir);
}

//==============================================================================
// 覆盖率批次 Y:Content-Range 起点溢出防御
//==============================================================================

// 206 响应的 Content-Range 起点是超过 uint64 的数字(服务器侧损坏
// 形态):解析的 stoull 溢出必须按校验失败收口——放弃续传转全新
// 下载,断点数据绝不接续起点无法确认的响应
TEST(DownloadEngineV2Resume, OverflowContentRangeAbandonsResume) {
    const std::string body = make_body(32 * 1024);
    ResumeTestServer server;
    ASSERT_TRUE(server.start(body, "\"etag-ovf\"", "Wed, 10 Sep 2026 08:00:00 GMT"));

    const std::string dir = temp_dir_for("overflow_cr");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/file.bin";
    auto options = single_connection_options(out_path);

    server.set_partial_first_bytes(4096);
    run_failing_first_phase(server, out_path, options);
    ASSERT_EQ(read_file_content(out_path + ".falcon.tmp").size(), 4096u);

    // 第二阶段:续传请求得到 206,但 Content-Range 起点溢出 → 校验失败
    server.set_partial_first_bytes(0);
    server.set_overflow_content_range(true);
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/file.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));

    // abandon 后转全新下载:任务完成且成品逐字节一致(校验失败若被
    // 当作通过,续传起点错位会在这里现形为内容损坏)
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body);

    // 服务器观测:一次带 Range 的续传请求 + abandon 后的无 Range 全新 GET
    const auto starts = server.range_starts_snapshot();
    ASSERT_FALSE(starts.empty());
    EXPECT_EQ(starts.back(), 4096u);
    EXPECT_GE(server.plain_gets(), 2)
        << "溢出 Content-Range 应触发放弃续传并重新发起全新下载";

    std::filesystem::remove_all(dir);
}
