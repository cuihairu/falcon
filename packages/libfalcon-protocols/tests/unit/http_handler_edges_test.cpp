// HTTP Handler 边缘路径测试（V1 curl 数据面）
//
// 覆盖率批次 G：http_handler.cpp gcov 80 miss 的真实缺口收敛。
// 自包含可编程 HTTP 测试服务器（记录请求 + 响应剧本 + 自动 Range
// 206 + 分块慢发），对 V1 handler 做回环测试：响应头解析
// （Content-Disposition 引号/无引号）、URL filename 推导、curl 选项
// 传播（自定义头/UA/Referer/Cookie/HTTP 认证挑战/代理）、断点续传
// （Range 断言）、Range 被无视 200 的临时文件重置防护、HTTP 错误重
// 试语义（4xx 立即抛 / 5xx 退避重试）、rename 失败、暂停中止、运
// 行时限速热应用与多段下载端到端。
//
// V2 分叉默认关（v2_http_enabled()=false），本文件全部走 V1 curl。

#include "plugins/http/http_handler.hpp"

#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace falcon;
namespace fs = std::filesystem;

//==============================================================================
// 可编程 HTTP 测试服务器
//==============================================================================

struct RecordedRequest {
    std::string method;
    std::string path;
    std::string range;  // Range 头原值（无则空）
    std::unordered_map<std::string, std::string> headers;  // 键小写
};

struct FakeResponse {
    int status = 200;
    std::string status_text = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool support_range = false;  // 请求带 Range 时自动 206 + 切片
    bool fail_nonzero_range = false;  // 起始 >0 的 Range 请求一律 500（段失败收口）
    bool no_length = false;  // 不发 Content-Length，body 以连接关闭为界（未知总长）
};

class HttpTestServer {
public:
    HttpTestServer() = default;
    ~HttpTestServer() { stop(); }
    HttpTestServer(const HttpTestServer&) = delete;
    HttpTestServer& operator=(const HttpTestServer&) = delete;

    void start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            std::fprintf(stderr, "HttpTestServer bind/listen failed\n");
            std::abort();
        }
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len);
        port_ = ntohs(bound.sin_port);
        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        ::shutdown(listen_fd_, SHUT_RDWR);  // Linux close() 不唤醒 accept
        ::close(listen_fd_);
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    int port() const { return port_; }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    // ---- 应答编程 ----

    void set_response(const std::string& path, const FakeResponse& resp) {
        std::lock_guard<std::mutex> lock(mutex_);
        responses_[path] = resp;
    }

    /// 按请求次序应答（末尾的应答无限重复）
    void set_script(const std::string& path, std::vector<FakeResponse> script) {
        std::lock_guard<std::mutex> lock(mutex_);
        scripts_[path] = std::move(script);
    }

    /// body 分块慢发（进度/暂停窗口）；range_start >= 0 时仅对
    /// 该起始偏移的 Range 请求慢发（多段差异化速度）
    void set_slow_body(const std::string& path, int chunk_delay_us,
                       size_t chunk_size = 32, long range_start = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        slow_[path] = SlowSpec{chunk_delay_us, chunk_size, range_start};
    }

    void clear_slow_body(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        slow_.erase(path);
    }

    /// 一次性中断：对匹配 Range（range_start<0 则任意请求）发出
    /// n_bytes 字节后直接断连（段短传 → 尺寸校验失败 → 重试续传）
    void set_abort_after(const std::string& path, size_t n_bytes,
                         long range_start = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_[path] = SlowSpec{0, n_bytes, range_start};
    }

    // ---- 记录 ----

    std::vector<RecordedRequest> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    size_t count_requests(const std::string& path, const std::string& method = {}) {
        size_t n = 0;
        for (const auto& r : requests()) {
            if (r.path == path && (method.empty() || r.method == method)) ++n;
        }
        return n;
    }

private:
    static bool send_all(int fd, const std::string& text) {
        size_t sent = 0;
        while (sent < text.size()) {
            ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    void accept_loop() {
        while (running_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_) return;
                continue;
            }
            timeval tv{15, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::thread([this, fd] { handle_connection(fd); }).detach();
        }
    }

    void handle_connection(int fd) {
        std::string buffer;
        for (;;) {
            // 读一个请求（到空行；GET 无 body）
            size_t head_end;
            while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
                char chunk[4096];
                ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
                if (n <= 0) {
                    ::close(fd);
                    return;
                }
                buffer.append(chunk, static_cast<size_t>(n));
            }
            const std::string head = buffer.substr(0, head_end);
            buffer.erase(0, head_end + 4);

            RecordedRequest rec;
            std::istringstream stream(head);
            std::string line;
            std::getline(stream, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t sp1 = line.find(' ');
            const size_t sp2 = line.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                rec.method = line.substr(0, sp1);
                rec.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = line.substr(0, colon);
                for (auto& ch : key) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
                size_t vstart = colon + 1;
                while (vstart < line.size() && line[vstart] == ' ') ++vstart;
                std::string value = line.substr(vstart);
                if (key == "range") rec.range = value;
                rec.headers[key] = value;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(rec);
            }

            FakeResponse resp;
            bool slow = false;
            int slow_delay = 0;
            size_t slow_chunk = 32;
            size_t abort_after = 0;
            bool abort_match = false;
            // 查表键剥除 query（记录保留原样供断言）
            std::string route = rec.path;
            const size_t qpos = route.find('?');
            if (qpos != std::string::npos) route = route.substr(0, qpos);
            long request_range_start = -1;
            if (rec.range.rfind("bytes=", 0) == 0) {
                const std::string spec = rec.range.substr(6);
                const size_t dash = spec.find('-');
                if (dash != std::string::npos) {
                    request_range_start =
                        static_cast<long>(std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10));
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto script = scripts_.find(route);
                if (script != scripts_.end()) {
                    auto& seq = script->second;
                    if (rec.method == "HEAD") {
                        // HEAD 探测（get_file_info）恒用末位应答：
                        // 剧本只驱动 GET 的错误/重试次序，否则 500 剧本
                        // 会在 download() 顶部的 HEAD 处直接炸掉
                        resp = seq.back();
                    } else if (seq.size() > 1) {
                        resp = seq.front();
                        seq.erase(seq.begin());
                    } else {
                        resp = seq.front();  // 末位应答无限重复
                    }
                } else {
                    auto it = responses_.find(route);
                    resp = it != responses_.end() ? it->second : FakeResponse{404, "Not Found", {}, "missing", false};
                }
                auto d = slow_.find(route);
                if (d != slow_.end() &&
                    (d->second.range_start < 0 ||
                     d->second.range_start == request_range_start)) {
                    slow = true;
                    slow_delay = d->second.delay_us;
                    slow_chunk = d->second.chunk;
                }
                abort_after = 0;
                abort_match = false;
                auto a = abort_.find(route);
                if (a != abort_.end() &&
                    (a->second.range_start < 0 ||
                     a->second.range_start == request_range_start)) {
                    abort_after = a->second.chunk;  // 复用 chunk 存字节数
                    abort_match = true;
                    abort_.erase(a);  // 一次性
                }
            }

            // Range 自动处理：206 + 切片
            std::string body = resp.body;
            int status = resp.status;
            std::string status_text = resp.status_text;
            if (resp.fail_nonzero_range && !rec.range.empty() &&
                rec.range.rfind("bytes=", 0) == 0) {
                const std::string spec = rec.range.substr(6);
                const size_t dash = spec.find('-');
                if (dash != std::string::npos &&
                    std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10) > 0) {
                    status = 500;
                    status_text = "Internal Server Error";
                    body.clear();
                }
            }
            if (resp.support_range && !rec.range.empty() &&
                rec.range.rfind("bytes=", 0) == 0 && status == 200) {
                const std::string spec = rec.range.substr(6);
                const size_t dash = spec.find('-');
                if (dash != std::string::npos) {
                    const uint64_t start = std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10);
                    const uint64_t end = spec.substr(dash + 1).empty()
                                             ? body.size() - 1
                                             : std::min<uint64_t>(
                                                   std::strtoull(spec.substr(dash + 1).c_str(), nullptr, 10),
                                                   body.size() - 1);
                    if (start < body.size() && end >= start) {
                        body = body.substr(static_cast<size_t>(start),
                                           static_cast<size_t>(end - start + 1));
                        status = 206;
                        status_text = "Partial Content";
                        resp.headers.emplace_back(
                            "Content-Range",
                            "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                                std::to_string(resp.body.size()));
                    }
                }
            }

            std::string out = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n";
            bool has_len = false;
            for (const auto& [k, v] : resp.headers) {
                out += k + ": " + v + "\r\n";
                if (k == "Content-Length") has_len = true;
            }
            if (!has_len) {
                if (resp.no_length) {
                    out += "Connection: close\r\n";  // body 以 EOF 为界
                } else {
                    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                }
            }
            out += "\r\n";
            if (!send_all(fd, out)) break;
            if (rec.method != "HEAD") {
                if (abort_match && abort_after > 0 && abort_after < body.size()) {
                    // 发出部分数据后硬断连：客户端按 Content-Length 判短传
                    send_all(fd, body.substr(0, abort_after));
                    break;
                }
                if (slow) {
                    for (size_t pos = 0; pos < body.size(); pos += slow_chunk) {
                        std::this_thread::sleep_for(std::chrono::microseconds(slow_delay));
                        if (!send_all(fd, body.substr(pos, slow_chunk))) break;
                    }
                    if (slow && resp.no_length) break;  // EOF 定界：发完即关
                } else if (!body.empty()) {
                    send_all(fd, body);
                    if (resp.no_length) break;  // EOF 定界：发完即关
                }
            }
        }
        ::close(fd);
    }

    struct SlowSpec {
        int delay_us;
        size_t chunk;
        long range_start;  // -1 = 所有请求
    };

    std::mutex mutex_;
    std::vector<RecordedRequest> requests_;
    std::unordered_map<std::string, FakeResponse> responses_;
    std::unordered_map<std::string, std::vector<FakeResponse>> scripts_;
    std::unordered_map<std::string, SlowSpec> slow_;
    std::unordered_map<std::string, SlowSpec> abort_;

    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread accept_thread_;
};

//==============================================================================
// 夹具与辅助
//==============================================================================

int uniqueSuffix() {
    static int counter = 0;
    return static_cast<int>(
        std::chrono::steady_clock::now().time_since_epoch().count() % 1000000) +
        (counter++);
}

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("falcon_http_edges_" + std::to_string(uniqueSuffix()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }
    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    fs::path path_;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

DownloadTask::Ptr makeTask(int id, const std::string& url, const std::string& output_path,
                           const DownloadOptions& options = {}) {
    auto task = std::make_shared<DownloadTask>(id, url, options);
    task->set_output_path(output_path);
    return task;
}

/// 可编程限速 listener：按查询次数返回预设值序列（末值无限重复）
class ScriptedSpeedListener : public IEventListener {
public:
    explicit ScriptedSpeedListener(std::vector<BytesPerSecond> limits)
        : limits_(std::move(limits)) {}

    BytesPerSecond query_speed_limit(TaskId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t idx = calls_++;
        return idx < limits_.size() ? limits_[idx] : limits_.back();
    }

private:
    std::vector<BytesPerSecond> limits_;
    size_t calls_ = 0;
    std::mutex mutex_;
};

class HttpHandlerEdgesTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<HttpTestServer>();
        server_->start();
        handler_ = std::make_unique<protocols::HttpHandler>();
    }
    void TearDown() override {
        handler_.reset();
        server_->stop();
    }

    HttpTestServer& server() { return *server_; }
    protocols::HttpHandler* handler() { return handler_.get(); }

private:
    std::unique_ptr<HttpTestServer> server_;
    std::unique_ptr<protocols::HttpHandler> handler_;
};

//==============================================================================
// get_file_info：响应头解析与 filename 推导
//==============================================================================

TEST_F(HttpHandlerEdgesTest, GetFileInfoParsesMetadataAndQuotedFilename) {
    FakeResponse resp;
    resp.headers = {
        {"Content-Type", "application/octet-stream"},
        {"Accept-Ranges", "bytes"},
        {"Content-Disposition", "attachment; filename=\"report data.bin\"; size=9"},
    };
    resp.body = "0123456789";
    server().set_response("/file.bin", resp);

    const auto info = handler()->get_file_info(server().url("/file.bin"), {});
    EXPECT_EQ(info.url, server().url("/file.bin"));
    EXPECT_EQ(info.total_size, 10u);
    EXPECT_EQ(info.content_type, "application/octet-stream");
    EXPECT_TRUE(info.supports_resume);
    EXPECT_EQ(info.filename, "report data.bin");
}

TEST_F(HttpHandlerEdgesTest, GetFileInfoParsesUnquotedFilename) {
    FakeResponse resp;
    resp.headers = {
        {"Content-Disposition", "attachment; filename=plain.bin; foo=bar"},
    };
    server().set_response("/anything", resp);

    const auto info = handler()->get_file_info(server().url("/anything"), {});
    EXPECT_EQ(info.filename, "plain.bin");
}

TEST_F(HttpHandlerEdgesTest, GetFileInfoDerivesFilenameFromUrl) {
    // 无 Content-Disposition：从 URL basename 推导并剥除 query
    FakeResponse resp;
    server().set_response("/path/to/archive.tar.gz", resp);
    EXPECT_EQ(handler()->get_file_info(server().url("/path/to/archive.tar.gz?token=x"), {}).filename,
              "archive.tar.gz");

    // 无 basename（URL 以 / 结尾）：默认 "download"
    server().set_response("/", resp);
    EXPECT_EQ(handler()->get_file_info(server().url("/"), {}).filename, "download");
}

TEST_F(HttpHandlerEdgesTest, GetFileInfoConnectionRefusedThrows) {
    HttpTestServer dead;
    dead.start();
    const int dead_port = dead.port();
    dead.stop();  // 端口已释放，connect 立即被拒

    EXPECT_THROW(handler()->get_file_info("http://127.0.0.1:" + std::to_string(dead_port) + "/f", {}),
                 NetworkException);
}

//==============================================================================
// curl 选项传播（服务器侧观测）
//==============================================================================

TEST_F(HttpHandlerEdgesTest, OptionsPropagateToRequests) {
    TempDir dir;
    // 一条真实 cookie：激活的 cookie 引擎会在请求里携带它，
    // 并在 handle 清理时把会话 cookie 写回 COOKIEJAR
    writeFile(dir.file("cookies.txt"),
              "# Netscape HTTP Cookie File\n"
              "127.0.0.1\tFALSE\t/\tFALSE\t0\ttestcookie\ttestvalue\n");

    FakeResponse resp;
    resp.body = "opts";
    server().set_response("/opt", resp);

    DownloadOptions options;
    options.user_agent = "Falcon-Edge/1.0";
    options.referer = "https://referrer.example.com/page";
    options.cookie_file = dir.file("cookies.txt");
    options.cookie_jar = dir.file("jar.txt");
    options.verify_ssl = false;  // 明文回环下无操作，仅覆盖设置分支
    options.headers = {{"X-Falcon-Test", "edge-payload"}};

    const auto task = makeTask(201, server().url("/opt"), dir.file("out.bin"), options);
    handler()->download(task, nullptr);
    ASSERT_EQ(task->status(), TaskStatus::Completed);

    const auto reqs = server().requests();
    ASSERT_GE(reqs.size(), 1u);
    const auto& r = reqs.front();
    EXPECT_EQ(r.headers.at("user-agent"), "Falcon-Edge/1.0");
    EXPECT_EQ(r.headers.at("referer"), "https://referrer.example.com/page");
    EXPECT_EQ(r.headers.at("x-falcon-test"), "edge-payload");
    ASSERT_TRUE(r.headers.count("cookie") > 0);  // COOKIEFILE 激活 cookie 引擎
    EXPECT_NE(r.headers.at("cookie").find("testcookie=testvalue"), std::string::npos);
    EXPECT_TRUE(fs::exists(dir.file("jar.txt")));  // COOKIEJAR 会话写出
}

TEST_F(HttpHandlerEdgesTest, HttpAuthSendsCredentialsAfterChallenge) {
    FakeResponse unauthorized;
    unauthorized.status = 401;
    unauthorized.status_text = "Unauthorized";
    unauthorized.headers = {{"WWW-Authenticate", "Basic realm=\"edge\""}};
    FakeResponse ok;
    ok.body = "secret-area";
    server().set_script("/auth", {unauthorized, ok});

    DownloadOptions options;
    options.http_username = "user";
    options.http_password = "pass";  // RFC 4648 向量：user:pass -> dXNlcjpwYXNz
    options.max_retries = 0;

    TempDir dir;
    const auto task = makeTask(202, server().url("/auth"), dir.file("out.bin"), options);
    handler()->download(task, nullptr);
    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(dir.file("out.bin")), "secret-area");

    const auto reqs = server().requests();
    ASSERT_EQ(reqs.size(), 3u);  // HEAD 探测 + 挑战前的 GET + 带凭据重放的 GET
    EXPECT_EQ(reqs[0].method, "HEAD");
    EXPECT_EQ(reqs[1].headers.count("authorization"), 0u);   // 首次挑战前不带
    EXPECT_EQ(reqs[2].headers.at("authorization"), "Basic dXNlcjpwYXNz");
}

TEST_F(HttpHandlerEdgesTest, UnreachableProxyFailsFast) {
    HttpTestServer proxy_gone;
    proxy_gone.start();
    const int dead_port = proxy_gone.port();
    proxy_gone.stop();

    FakeResponse resp;
    resp.body = "direct-hit";
    server().set_response("/opt", resp);

    DownloadOptions options;
    options.proxy = "http://127.0.0.1:" + std::to_string(dead_port);
    options.proxy_username = "u";
    options.proxy_password = "p";  // 代理凭据设置分支
    options.max_retries = 0;

    TempDir dir;
    const auto task = makeTask(203, server().url("/opt"), dir.file("out.bin"), options);
    // 代理设置生效则必败；若设置未生效 curl 直连成功使本用例变红
    EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    EXPECT_EQ(server().count_requests("/opt"), 0u);  // 流量从未直连到达
}

//==============================================================================
// 下载主路径：进度、续传、Range 防护、重试语义
//==============================================================================

TEST_F(HttpHandlerEdgesTest, DownloadReportsProgressDuringSlowTransfer) {
    FakeResponse resp;
    resp.body = std::string(2048, 'p');
    server().set_response("/slow", resp);
    server().set_slow_body("/slow", 8'000, 32);  // ≈512ms，越过 200ms 节流窗

    TempDir dir;
    const auto task = makeTask(204, server().url("/slow"), dir.file("out.bin"));
    handler()->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(dir.file("out.bin")), std::string(2048, 'p'));
    EXPECT_GT(task->downloaded_bytes(), 0u);  // 进度记账真实发生
    EXPECT_EQ(task->total_bytes(), 2048u);
}

TEST_F(HttpHandlerEdgesTest, DynamicSpeedLimitHotAppliedMidTransfer) {
    FakeResponse resp;
    resp.body = std::string(4096, 's');
    server().set_response("/dyn", resp);
    server().set_slow_body("/dyn", 5'000, 32);  // ≈640ms，多次进度窗口

    // 首窗无限制，次窗起限速——触发热应用分支（want != applied）
    ScriptedSpeedListener listener({0, 32 * 1024});

    TempDir dir;
    const auto task = makeTask(205, server().url("/dyn"), dir.file("out.bin"));
    handler()->download(task, &listener);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(dir.file("out.bin")), std::string(4096, 's'));
}

TEST_F(HttpHandlerEdgesTest, DownloadResumeSendsRangeAndAppends) {
    FakeResponse resp;
    resp.support_range = true;
    resp.headers = {{"Accept-Ranges", "bytes"}};
    resp.body = "AABBCCDD";
    server().set_response("/resume.bin", resp);

    TempDir dir;
    const std::string out = dir.file("resume.bin");
    writeFile(out + ".falcon.tmp", "AA");  // 已落盘 2 字节

    const auto task = makeTask(206, server().url("/resume.bin"), out);
    handler()->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(out), "AABBCCDD");
    EXPECT_FALSE(fs::exists(out + ".falcon.tmp"));

    const auto reqs = server().requests();
    ASSERT_GE(reqs.size(), 1u);
    EXPECT_EQ(reqs.back().range, "bytes=2-");  // 断点续传偏移到达服务器
}

TEST_F(HttpHandlerEdgesTest, DownloadRangeLyingServerNeverProducesCorruptOutput) {
    FakeResponse resp;
    resp.body = "AABBCCDD";  // 无 Accept-Ranges：Range 被无视回 200 全量
    server().set_response("/liar.bin", resp);

    DownloadOptions options;
    options.max_retries = 1;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("liar.bin");
    writeFile(out + ".falcon.tmp", "AA");  // 断点 2 字节

    const auto task = makeTask(207, server().url("/liar.bin"), out, options);
    // 现代 curl 自带 resume 守卫：续传请求被以 200 应答即 CURLE_RANGE_ERROR，
    // 重试耗尽后按失败收口——绝不把 200 全量追加进断点产出损坏成品
    // （handler 内另有 resize 清空的纵深防御分支，本 curl 下守卫先拒）
    EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    EXPECT_FALSE(fs::exists(out));  // 成品从未发布
}

TEST_F(HttpHandlerEdgesTest, DownloadHttpErrorRetrySemantics) {
    // 5xx：退避重试后成功
    FakeResponse boom;
    boom.status = 500;
    boom.status_text = "Internal Server Error";
    FakeResponse ok;
    ok.body = "finally";
    server().set_script("/flaky", {boom, boom, ok});

    DownloadOptions options;
    options.retry_delay_seconds = 0;
    TempDir dir;
    const auto task = makeTask(208, server().url("/flaky"), dir.file("flaky.bin"), options);
    handler()->download(task, nullptr);
    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(dir.file("flaky.bin")), "finally");
    EXPECT_EQ(server().count_requests("/flaky", "GET"), 3u);  // 恰好两次 5xx 重试

    // 4xx：立即抛出，不重试（HEAD 探测用剧本末位 200 放行，
    // 404 只发给首个 GET——download_single 的 4xx 立即抛分支）
    FakeResponse ok_gone;
    ok_gone.body = "gone-later";
    FakeResponse not_found;
    not_found.status = 404;
    not_found.status_text = "Not Found";
    server().set_script("/gone", {not_found, ok_gone});
    const auto task2 = makeTask(209, server().url("/gone"), dir.file("gone.bin"), options);
    EXPECT_THROW(handler()->download(task2, nullptr), NetworkException);
    EXPECT_EQ(server().count_requests("/gone", "GET"), 1u);
}

TEST_F(HttpHandlerEdgesTest, DownloadThrowsWhenRenameFails) {
    FakeResponse resp;
    resp.body = "data";
    server().set_response("/occupied", resp);

    TempDir dir;
    const std::string out = dir.file("occupied");
    fs::create_directories(out);  // 成品路径被目录占用
    const auto task = makeTask(210, server().url("/occupied"), out);

    EXPECT_THROW(handler()->download(task, nullptr), FileIOException);
    EXPECT_EQ(task->status(), TaskStatus::Pending);  // 绝不假报完成
    EXPECT_TRUE(fs::exists(out + ".falcon.tmp"));
}

TEST_F(HttpHandlerEdgesTest, DownloadFailsFastOnUnopenableOutput) {
    FakeResponse resp;
    resp.body = "data";
    server().set_response("/file", resp);

    TempDir dir;
    const std::string out = (dir.path() / "missing_sub" / "out.bin").string();
    const auto task = makeTask(211, server().url("/file"), out);
    EXPECT_THROW(handler()->download(task, nullptr), FileIOException);
}

//==============================================================================
// 暂停 / 取消 / 恢复生命周期
//==============================================================================

TEST_F(HttpHandlerEdgesTest, DownloadPauseAbortsMidFlight) {
    FakeResponse resp;
    resp.body = std::string(8192, 'x');
    server().set_response("/pausable", resp);
    server().set_slow_body("/pausable", 10'000, 32);  // ≈2.6s 窗口

    TempDir dir;
    const std::string out = dir.file("paused.bin");
    const auto task = makeTask(212, server().url("/pausable"), out);

    std::thread pauser([&task] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        task->set_status(TaskStatus::Paused);
    });
    handler()->download(task, nullptr);
    pauser.join();

    // progress_callback 见 Paused 返回 1 → CURLE_ABORTED_BY_CALLBACK →
    // 静默返回：不 rename、不抛出，临时文件保留为断点
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(fs::exists(out));
    EXPECT_TRUE(fs::exists(out + ".falcon.tmp"));
}

TEST_F(HttpHandlerEdgesTest, ResumeReentersDownloadAfterPause) {
    FakeResponse resp;
    resp.body = "resumable";
    server().set_response("/resume-again", resp);

    TempDir dir;
    const auto task = makeTask(213, server().url("/resume-again"), dir.file("r.bin"));
    handler()->pause(task);
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // 恢复前置位是调用方（TaskManager::resume_task → start_task）职责：
    // HttpHandler::resume 只重跑 download()，Paused 守卫对未复位状态
    // 直接返回
    task->set_status(TaskStatus::Downloading);
    handler()->resume(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(dir.file("r.bin")), "resumable");
}

TEST_F(HttpHandlerEdgesTest, CancelledTaskSkipsAndNullTaskIsIgnored) {
    FakeResponse resp;
    resp.body = "never";
    server().set_response("/cancelled", resp);

    TempDir dir;
    const auto task = makeTask(214, server().url("/cancelled"), dir.file("c.bin"));
    handler()->cancel(task);
    handler()->download(task, nullptr);  // 循环顶 Cancelled → 立即返回
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
    EXPECT_FALSE(fs::exists(dir.file("c.bin")));

    handler()->pause(nullptr);
    handler()->resume(nullptr, nullptr);
    handler()->cancel(nullptr);
    SUCCEED();
}

//==============================================================================
// 多段下载
//==============================================================================

TEST_F(HttpHandlerEdgesTest, SegmentedDownloadEndToEndWithSpeedLimit) {
    const std::string content(64 * 1024, 'g');
    FakeResponse head;
    head.status = 200;
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;
    head.support_range = true;
    server().set_response("/big.bin", head);

    DownloadOptions options;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;  // 64KB 文件触发分段
    options.retry_delay_seconds = 0;
    // listener 侧任务限速非零 → 分段路径的按连接均摊分支
    ScriptedSpeedListener listener({64 * 1024});

    TempDir dir;
    const auto task = makeTask(215, server().url("/big.bin"), dir.file("big.bin"), options);
    handler()->download(task, &listener);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(dir.file("big.bin")), content);  // 分段拼接逐字节一致
    EXPECT_GE(server().count_requests("/big.bin"), 2u);  // HEAD + 多段 GET
}

TEST_F(HttpHandlerEdgesTest, SegmentedDownloadFailsCleanlyOnSegmentHttpError) {
    const std::string content(64 * 1024, 'e');
    FakeResponse head;
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;
    head.support_range = true;
    head.fail_nonzero_range = true;  // 段 0 之外的 Range 一律 500

    DownloadOptions options;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;
    options.max_retries = 1;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("segfail.bin");
    const auto task = makeTask(216, server().url("/segfail.bin"), out, options);
    server().set_response("/segfail.bin", head);

    // 段重试耗尽 → 分段下载失败收口：FileIOException，成品绝不发布
    EXPECT_THROW(handler()->download(task, nullptr), FileIOException);
    EXPECT_FALSE(fs::exists(out));
}

TEST_F(HttpHandlerEdgesTest, SegmentedPauseCancelsActiveSegments) {
    const std::string content(64 * 1024, 'z');
    FakeResponse head;
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;
    head.support_range = true;
    server().set_response("/segpause.bin", head);
    server().set_slow_body("/segpause.bin", 10'000, 32);  // ≈20s 窗口

    DownloadOptions options;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;

    TempDir dir;
    const std::string out = dir.file("segpause.bin");
    const auto task = makeTask(217, server().url("/segpause.bin"), out, options);

    std::thread pauser([this, &task] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        server();  // 保持服务器存活至暂停完成
        task->set_status(TaskStatus::Paused);
        handler()->pause(task);  // 命中 active_segmented_downloads_ → 取消各段
    });
    handler()->download(task, nullptr);
    pauser.join();

    // 段被取消 → download 静默返回：不发布成品、状态保持 Paused
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(fs::exists(out));
}

TEST_F(HttpHandlerEdgesTest, PauseDuringRetryBackoffAbortsQuietly) {
    FakeResponse boom;
    boom.status = 500;
    boom.status_text = "Internal Server Error";
    FakeResponse ok;
    ok.body = "never-reached";
    server().set_script("/backoff-pause", {boom, ok});

    DownloadOptions options;
    options.retry_delay_seconds = 2;  // 退避睡眠窗口留给暂停线程

    TempDir dir;
    const std::string out = dir.file("bp.bin");
    const auto task = makeTask(218, server().url("/backoff-pause"), out, options);

    std::thread pauser([&task] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        task->set_status(TaskStatus::Paused);
    });
    handler()->download(task, nullptr);
    pauser.join();

    // 重试间隙的 Paused 检查：静默返回，不进入下一轮、不发布成品
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(fs::exists(out));
}

TEST_F(HttpHandlerEdgesTest, SegmentedCancelCancelsActiveSegments) {
    const std::string content(64 * 1024, 'q');
    FakeResponse head;
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;
    head.support_range = true;
    server().set_response("/segcancel.bin", head);
    server().set_slow_body("/segcancel.bin", 10'000, 32);  // ≈20s 窗口

    DownloadOptions options;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;

    TempDir dir;
    const std::string out = dir.file("segcancel.bin");
    const auto task = makeTask(219, server().url("/segcancel.bin"), out, options);

    std::thread canceller([this, &task] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // 分段路径的段只看 downloader 的 cancelled 标志（不看 task 状态），
        // 必须经 handler 转发 downloader->cancel()
        task->set_status(TaskStatus::Cancelled);
        handler()->cancel(task);
    });
    handler()->cancel(task);  // 下载前：注册表未命中，仅置状态
    handler()->download(task, nullptr);
    canceller.join();
    handler()->cancel(task);  // 下载后重入 cancel：无残留注册表项，幂等

    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
    EXPECT_FALSE(fs::exists(out));
}

TEST_F(HttpHandlerEdgesTest, SegmentedResumeAfterPauseCompletesFromSegmentFiles) {
    const std::string content(64 * 1024, 'r');
    FakeResponse head;
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;
    head.support_range = true;
    server().set_response("/segresume.bin", head);
    // 2 连接 → 段 0 [0,32K) 全速、段 1 [32K,...) 慢发：暂停时段 0 已
    // 完成落盘（ofstream 关闭才可见 file_size，进行中段的缓冲数据
    // 观测不到——断点数据必须来自已完成段）
    server().set_slow_body("/segresume.bin", 10'000, 32, 32 * 1024);

    DownloadOptions options;
    options.max_connections = 2;
    options.min_segment_size = 16 * 1024;

    TempDir dir;
    const std::string out = dir.file("segresume.bin");
    const auto task = makeTask(220, server().url("/segresume.bin"), out, options);

    std::thread pauser([this, &task] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        task->set_status(TaskStatus::Paused);
        handler()->pause(task);  // 转发取消慢发的段 1
    });
    handler()->download(task, nullptr);
    pauser.join();
    ASSERT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(fs::exists(out));

    // 恢复前置位（TaskManager 语义）→ resume 重入分段路径：
    // downloader 析构即清理段文件（设计如此：handler 层暂停不保留
    // 段断点），重新全量分段下载，成品仍必须逐字节一致
    task->set_status(TaskStatus::Downloading);
    server().clear_slow_body("/segresume.bin");
    handler()->resume(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(out), content);
    EXPECT_FALSE(fs::exists(out + ".falcon.tmp.seg0"));
}

TEST_F(HttpHandlerEdgesTest, SegmentRetryResumesFromPartialSegmentData) {
    const std::string content(64 * 1024, 'b');
    FakeResponse head;
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;
    head.support_range = true;
    server().set_response("/segabort.bin", head);
    // 段 0 的首次 Range 请求发出 4KB 后硬断连：短传 → 精确尺寸校验
    // 失败 → worker 以段文件已有尺寸计算续传起点重试 → 重试连接的
    // 范围数据以 app 模式补齐（绝不从头截断重来）
    server().set_abort_after("/segabort.bin", 4096, 0);

    DownloadOptions options;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("segabort.bin");
    const auto task = makeTask(222, server().url("/segabort.bin"), out, options);
    handler()->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(out), content);  // 拼接逐字节一致，无洞无重叠
}

TEST_F(HttpHandlerEdgesTest, SegmentedDownloadFailsOnRangeLyingServer) {
    const std::string content(64 * 1024, 'l');
    FakeResponse head;
    // HEAD 宣称 Accept-Ranges（探测通过 → 走分段），GET 一律 200 全量
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;

    DownloadOptions options;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;
    options.max_retries = 2;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("seglie.bin");
    const auto task = makeTask(221, server().url("/seglie.bin"), out, options);
    server().set_response("/seglie.bin", head);

    // 非零起始段被以 200 应答：截回本次续传起点按失败收尾，重试耗尽后
    // 整体失败收口——绝不让 200 全量数据进 merge（V1 段完整性闭环）
    EXPECT_THROW(handler()->download(task, nullptr), FileIOException);
    EXPECT_FALSE(fs::exists(out));
}

TEST_F(HttpHandlerEdgesTest, DownloadWithoutContentLengthCompletesWithUnknownTotal) {
    FakeResponse resp;
    resp.body = std::string(2048, 'n');
    resp.no_length = true;  // HEAD 无 Content-Length：总长未知（0）
    server().set_response("/nolen", resp);
    // 慢发越过 200ms 进度节流窗：进度记账在 dltotal==0 下照常发生
    server().set_slow_body("/nolen", 8'000, 32);

    TempDir dir;
    const auto task = makeTask(223, server().url("/nolen"), dir.file("nolen.bin"));
    handler()->download(task, nullptr);

    // 总长未知不阻碍下载：EOF 即完成，成品逐字节一致，total 记 0
    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(dir.file("nolen.bin")), std::string(2048, 'n'));
    EXPECT_EQ(task->total_bytes(), 0u);
    EXPECT_GT(task->downloaded_bytes(), 0u);
}

} // namespace
