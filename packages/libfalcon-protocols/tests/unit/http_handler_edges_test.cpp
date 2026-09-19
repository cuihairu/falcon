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

#include "scripted_http_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace falcon;
namespace fs = std::filesystem;

// 可编程 HTTP 测试服务器已抽取为共享基建（metalink 委托 e2e 同用）
using falcon::testscripts::FakeResponse;
using falcon::testscripts::RecordedRequest;
using HttpTestServer = falcon::testscripts::ScriptedHttpServer;

//==============================================================================
// 夹具与辅助
//==============================================================================

int uniqueSuffix() {
    static int counter = 0;
    // pid 参与:ctest 每用例独立进程并行跑,counter 各自从 0 起,仅靠
    // 时钟低 6 位截断跨进程可撞——同名 TempDir 会被并行进程的析构
    // remove_all 连树删掉,下载期间目录消失即静默改变被测行为
    // (CI Coverage 实证:并行进程删掉段 0 占位目录 → 分段全成功,
    // "目录占用必须失败"的用例反而下载成功)
#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif
    const auto tick = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return static_cast<int>((pid * 1000003 + tick) % 1000000000) + (counter++);
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

// 段文件已有部分数据（首次尝试中断残留）后再遇 200 撒谎：
// resize 截回续传起点（existing_size>0 分支），残段不膨胀成全量垃圾
TEST_F(HttpHandlerEdgesTest, SegmentPartialDataTruncatedOnLyingRetry) {
    const std::string content(64 * 1024, 'x');
    FakeResponse full;  // GET 一律 200 全量（Range 撒谎）
    full.body = content;
    FakeResponse head;
    head.headers = {{"Accept-Ranges", "bytes"}};  // 探测放行 → 分段
    head.body = content;

    server().set_head_response("/seglie2.bin", head);
    server().set_response("/seglie2.bin", full);
    // 一次性：对起始 16KB 的 Range 请求发 4KB 后硬断连 → 段文件残留 4KB
    server().set_abort_after("/seglie2.bin", 4096, 16384);

    DownloadOptions options;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;
    options.max_retries = 2;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("seglie2.bin");
    const auto task = makeTask(222, server().url("/seglie2.bin"), out, options);

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

// 段文件路径被目录占用:download_segment_curl 打开段文件失败 →
// 段下载函数返回 false → 重试耗尽 → 分段下载失败收口(FileIOException),
// 成品绝不发布
TEST_F(HttpHandlerEdgesTest, SegmentFileOccupiedByDirectoryFailsCleanly) {
    const std::string content(64 * 1024, 'd');
    FakeResponse head;
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;
    head.support_range = true;
    server().set_response("/segdir.bin", head);

    DownloadOptions options;
    options.max_connections = 4;
    options.min_segment_size = 16 * 1024;  // 64KB 文件触发分段
    options.max_retries = 1;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("segdir.bin");
    // 段 0 文件路径(<out>.falcon.tmp.seg0)被目录占用 → 段打开即失败
    fs::create_directories(out + ".falcon.tmp.seg0");

    const auto task = makeTask(218, server().url("/segdir.bin"), out, options);
    EXPECT_THROW(handler()->download(task, nullptr), FileIOException);
    EXPECT_FALSE(fs::exists(out));

    // 路径事实无条件落日志:分段失败于段 0 打开时零 GET(打开先于
    // curl),单连接成功为 1 次无 Range GET,分段全成功为 4 次带 Range
    // GET——"下载成功"的具体路径由请求序列直接判别
    std::string req_dump;
    for (const auto& r : server().requests()) {
        req_dump += r.method + " " + r.path +
                    (r.range.empty() ? "" : " Range=" + r.range) + "; ";
    }
    std::cout << "[诊断] 请求序列: " << (req_dump.empty() ? "(无请求)" : req_dump)
              << "| 成品存在: " << fs::exists(out) << std::endl;
}
