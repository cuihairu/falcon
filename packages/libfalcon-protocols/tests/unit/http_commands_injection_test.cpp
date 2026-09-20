/**
 * @file http_commands_injection_test.cpp
 * @brief V2 HTTP 命令链故障注入测试（TLS 链创建/握手/发送 + socket/connect/
 *        解析硬失败 + /dev/full 磁盘满）
 * @author Falcon Team
 * @date 2026-09-19
 *
 * 回环上不可构造的失败（注入点确定性命中收口分支）：
 * - TLS 创建链五点（method/ctx/ssl null、set_fd、set1_host）必须发生在
 *   ClientHello 之前——服务器 handshakes()==0 即观测证据
 * - 握手首轮 WANT_WRITE 属进行中而非失败，重入续推后下载自然完成
 * - SSL_write 硬失败（真实握手完成后）任务 FAILED 干净收口
 * - socket() 创建失败 / connect 立即硬失败（回环上非阻塞 connect 恒报
 *   in-progress，真实立即失败本地网络环境不可构造）
 * - /dev/full 四个失败面（磁盘满绝不假报 COMPLETED）：容量触发的中途
 *   冲刷失败、直写 seekp 冲刷失败、收满后完成路径冲刷失败（receive
 *   后与首次 execute 两种完成形态）。测量备注：初始批次写失败分支
 *   结构性不可达——初始批次 ≤ 4KB 被 ofstream filebuf（8KB）吞入
 *   用户态缓冲，ENOSPC 到后续冲刷点才浮现
 * - 条件下载 × 重定向交点：跟随连接必须携带组级 If-Modified-Since
 *   （条件作用于最终资源；回环可构造、非注入路径）
 */

#include <gtest/gtest.h>
#include <falcon/detail/injection.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/resume_control.hpp>

#include "scripted_http_server.hpp"

#ifdef FALCON_ENABLE_OPENSSL
#include "tls_loopback_server.hpp"
#endif

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace falcon;

namespace {

std::string inj_temp_dir(const char* tag) {
#ifdef _WIN32
    const int pid = static_cast<int>(::_getpid());
#else
    const int pid = static_cast<int>(::getpid());
#endif
    return (std::filesystem::temp_directory_path() /
            (std::string("falcon_v2_inj_") + tag + "_" + std::to_string(pid)))
        .string();
}

std::string inj_read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// 无服务器失败（socket 创建/connect 硬失败在触达对端之前收口；
// with_injection=false 用于解析失败等无需注入的路径）
void run_no_server_fail(detail::InjectPoint point, const std::string& url,
                        const char* tag, bool with_injection = true) {
    const std::string dir = inj_temp_dir(tag);
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(url, options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    if (with_injection) {
        detail::ScopedInjection injection(point);
        testtls::TlsEngineRunner runner(engine);
        ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
        EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    } else {
        testtls::TlsEngineRunner runner(engine);
        ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
        EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }

    // 半成品不顶最终名
    EXPECT_TRUE(inj_read_file(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

#ifdef FALCON_ENABLE_OPENSSL

// TLS 链创建失败：任务 FAILED 且失败发生在 ClientHello 之前
void run_tls_creation_fail(detail::InjectPoint point, bool verify_ssl) {
    const std::string body = testtls::make_body(64 * 1024);

    const std::string dir = inj_temp_dir("tls");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    testtls::TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = verify_ssl;

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    {
        detail::ScopedInjection injection(point);
        testtls::TlsEngineRunner runner(engine);
        ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
        EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    // 创建失败必须发生在握手之前：服务器侧零次完整握手
    EXPECT_EQ(server.handshakes(), 0);
    EXPECT_TRUE(inj_read_file(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

struct TlsCreateFailCase {
    detail::InjectPoint point;
    bool verify_ssl;
};

class TlsCreationFailTest : public ::testing::TestWithParam<TlsCreateFailCase> {};

TEST_P(TlsCreationFailTest, FailsBeforeClientHello) {
    const TlsCreateFailCase& c = GetParam();
    run_tls_creation_fail(c.point, c.verify_ssl);
}

INSTANTIATE_TEST_SUITE_P(TlsChain, TlsCreationFailTest,
                         ::testing::ValuesIn(std::vector<TlsCreateFailCase>{
                             {detail::InjectPoint::TlsMethodFail, false},
                             {detail::InjectPoint::TlsCtxNewFail, false},
                             {detail::InjectPoint::TlsSslNewFail, false},
                             {detail::InjectPoint::TlsSetFdFail, false},
                             // set1_host 只在 verify_ssl=true 时调用
                             {detail::InjectPoint::TlsSetHostFail, true},
                         }));

#endif  // FALCON_ENABLE_OPENSSL

}  // namespace

// socket() 创建失败：任务 FAILED，引擎存活
TEST(DownloadEngineV2Injection, SocketCreateFailFailsTask) {
    run_no_server_fail(detail::InjectPoint::HttpSocketCreate,
                       "http://127.0.0.1:1/x.bin", "sockcreate");
}

// connect 立即硬失败（回环不可构造，注入确定性命中）：错误消息携带
// ENETUNREACH 语义，任务 FAILED
TEST(DownloadEngineV2Injection, ConnectHardFailFailsTask) {
    run_no_server_fail(detail::InjectPoint::HttpConnectHardFail,
                       "http://127.0.0.1:1/x.bin", "connfail");
}

// 域名解析失败（.invalid 保留 TLD 不可解析）：无需注入，任务 FAILED
TEST(DownloadEngineV2Injection, ResolveFailureFailsTask) {
    run_no_server_fail(detail::InjectPoint::HttpSocketCreate,
                       "http://falcon.invalid/x.bin", "resolvefail", false);
}

// 条件下载 + 302 重定向：条件作用于最终资源——跟随连接必须原样携带
// 组级 If-Modified-Since。目标 /b 请求携带非空 if-modified-since 头
// 即 handle_redirect 条件分支的执行铁证
TEST(DownloadEngineV2Injection, ConditionalGetRedirectCarriesIfModifiedSince) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(8 * 1024, 'c');
    testscripts::FakeResponse redirect;
    redirect.status = 302;
    redirect.status_text = "Found";
    redirect.headers = {{"Location", "/b.bin"}};
    server.set_response("/a.bin", redirect);
    testscripts::FakeResponse fresh;
    fresh.body = body;
    server.set_response("/b.bin", fresh);

    const std::string dir = inj_temp_dir("condredir");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();
    // 预置本地文件：conditional_get 据此在组 init 时生成组级
    // If-Modified-Since（取 mtime 的 HTTP 日期）
    {
        std::ofstream stale(out_path, std::ios::binary);
        stale << "stale-local-content";
    }

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.temp_extension = "";
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.conditional_get = true;  // 隐含覆盖授权，不触发已存在门禁
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(server.url("/a.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();
    server.stop();

    // 重定向目标收到 200 全量应答 → 本地被新内容替换
    EXPECT_EQ(inj_read_file(out_path), body);

    // 跟随请求（/b）必须携带组级 If-Modified-Since（初始请求 /a 携带
    // 属既有覆盖；此处钉住的是重定向跟随不掉条件头）
    bool saw_ims_on_target = false;
    int target_gets = 0;
    for (const auto& r : server.requests()) {
        if (r.path == "/b.bin" && r.method == "GET") {
            ++target_gets;
            const auto it = r.headers.find("if-modified-since");
            if (it != r.headers.end() && !it->second.empty()) {
                saw_ims_on_target = true;
            }
        }
    }
    ASSERT_GT(target_gets, 0) << "重定向跟随请求未到达服务器";
    EXPECT_TRUE(saw_ims_on_target)
        << "重定向跟随连接未携带 If-Modified-Since";

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 请求发送 WANT_WRITE（模拟内核发送缓冲满——回环上请求恒小于发送
// 缓冲，不可自然构造）：注入置位期间请求 send 恒被拦，「连接已建立
// 且请求未到达」即注入生效的服务器侧铁证；清注入后 socket 恒可写
// 唤醒重入，请求续推后下载自然完成（进行中语义非失败、无重连）
TEST(DownloadEngineV2Injection, PlainRequestSendWantWriteSuspendsThenRecovers) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(32 * 1024, 'w');
    testscripts::FakeResponse resp;
    resp.body = body;
    server.set_response("/f.bin", resp);

    const std::string dir = inj_temp_dir("sendww");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.temp_extension = "";
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    // 注入先于任务创建置位：初始连接的请求 send 必然被拦
    detail::set_injection(detail::InjectPoint::HttpSendWantWrite, true);
    const TaskId task_id = engine.add_download(server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    // 锚点：连接已到达（connect 完成）而请求被拦（GET 计数保持 0）
    ASSERT_TRUE(testtls::wait_for(
        [&] { return server.connections() >= 1; }, 10000));
    // 连接完成后 CONNECTING→send 在下一轮 poll 唤醒（毫秒级）发生；
    // 静置窗口保证首个 send 已被拦下，清注入不早于挂起
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(server.count_requests("/f.bin", "GET"), 0u)
        << "注入置位期间请求必须被拦下（WANT_WRITE 挂起中）";
    detail::set_injection(detail::InjectPoint::HttpSendWantWrite, false);

    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(inj_read_file(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 请求发送 WANT_WRITE 挂起后的事件注册失败（fd 上限/ENOMEM）：命令
// 不得带着 socket_wait_map_ 条目挂起等永远不会来的唤醒——注册失败
// 立即按发送失败收口。HttpSendWantWrite 是持续位：先放挂起前的
// WRITE 注册成功，锚定挂起稳定窗后再置 EventPollAddFail——持续位
// 让每次重入 send 仍报 EAGAIN，重新注册必然命中已置位的注入
TEST(DownloadEngineV2Injection, PlainSendWantWriteRegistrationFailsCleanly) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(32 * 1024, 'w');
    testscripts::FakeResponse resp;
    resp.body = body;
    server.set_response("/f.bin", resp);

    const std::string dir = inj_temp_dir("sendwwreg");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.timeout_seconds = 10;

    {
        detail::ScopedInjection send_ww(
            detail::InjectPoint::HttpSendWantWrite);
        const TaskId task_id =
            engine.add_download(server.url("/f.bin"), options);
        ASSERT_GT(task_id, 0u);
        auto* group = engine.request_group_man()->find_group(task_id);
        ASSERT_NE(group, nullptr);

        testtls::TlsEngineRunner runner(engine);
        // 锚点：连接已到达而请求被拦（GET 计数保持 0）——此时挂起前的
        // WRITE 注册已成功（AddFail 未置位）。持续位保持挂起：可写事件
        // 重入 send 再报 EAGAIN，随后的重新注册命中置位后的 AddFail
        ASSERT_TRUE(testtls::wait_for(
            [&] { return server.connections() >= 1; }, 10000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ASSERT_EQ(server.count_requests("/f.bin", "GET"), 0u)
            << "请求必须被拦下（WANT_WRITE 挂起中）";
        detail::ScopedInjection add_fail(
            detail::InjectPoint::EventPollAddFail);

        ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
        ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    EXPECT_TRUE(inj_read_file(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 响应头接收 EAGAIN 重注册失败（fd 上限/ENOMEM）：服务器延迟后只发
// 半截响应头再挂住——首个 READ 注册（AddFail 未置位）成功挂起等
// 前缀，前缀到达后 recv 得部分头仍不完整，重新注册命中置位后的
// EventPollAddFail，按响应头接收失败收口
TEST(DownloadEngineV2Injection, ResponseHeaderRecvRegistrationFailFailsCleanly) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(8 * 1024, 'h');
    testscripts::FakeResponse resp;
    resp.body = body;
    // 1.5s 后只发 "HTTP/1.1 200 OK\r\n" 前缀再挂住（等客户端断开）
    resp.defer_partial_ms = 1500;
    server.set_response("/f.bin", resp);

    const std::string dir = inj_temp_dir("hdrrecvreg");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.timeout_seconds = 10;

    const TaskId task_id = engine.add_download(server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    // 400ms 置位（远晚于首个 READ 注册的毫秒级窗口、远早于 1.5s 前缀）
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    detail::ScopedInjection add_fail(detail::InjectPoint::EventPollAddFail);

    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(inj_read_file(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 下载体接收 EAGAIN 重注册失败（fd 上限/ENOMEM）：慢发体持续滴流——
// 锚定 body 中途（GET 已达 + 静置）置位 EventPollAddFail，慢发的
// EAGAIN 间隙让下一次 recv 后的重新注册必然命中，按接收失败收口
TEST(DownloadEngineV2Injection, DownloadBodyRecvRegistrationFailFailsCleanly) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(64 * 1024, 'b');
    testscripts::FakeResponse resp;
    resp.body = body;
    server.set_response("/f.bin", resp);
    server.set_slow_body("/f.bin", 250, 32);  // ~128KB/s：64KB 约 0.5s

    const std::string dir = inj_temp_dir("bodyrecvreg");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.timeout_seconds = 10;

    const TaskId task_id = engine.add_download(server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    // 锚点：请求已到达（响应头阶段已过、body 命令已接管）+ 静置进
    // body 中途；慢发的持续 EAGAIN 让置位后的重新注册必然命中
    ASSERT_TRUE(testtls::wait_for(
        [&] { return server.count_requests("/f.bin", "GET") >= 1; }, 10000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    detail::ScopedInjection add_fail(detail::InjectPoint::EventPollAddFail);

    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(inj_read_file(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 多镜像恢复连接轮转到非主镜像必须剥离 If-Range（连接级重试链剥离
// 分支）：断点续传的 If-Range/ETag 归属主镜像，轮转承接的镜像与该
// ETag 无关——误带会让有效续传被整体拒绝。主镜像 A 对恢复连接
// 「收下请求后立即断连」（响应头阶段失败 → 连接级重试链），换源
// 落在 B：断言 A 的请求武装了 Range + If-Range（apply 路径铁证）、
// B 的请求带 Range 且无 If-Range（剥离分支执行铁证），206 续传完成
TEST(DownloadEngineV2Injection, ResumeRetryOnNonPrimaryMirrorStripsIfRange) {
    const std::string body(8 * 1024, 'r');
    testscripts::ScriptedHttpServer server;
    server.start();
    testscripts::FakeResponse resp;
    resp.body = body;
    resp.support_range = true;
    server.set_response("/g.bin", resp);
    server.set_fail_immediate("/f.bin");  // 主镜像：恢复连接一律断连

    const std::string dir = inj_temp_dir("mirr");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    // 手工构造断点现场：主镜像 = /f.bin、1KB 已落盘进度。半成品按
    // 默认 temp_extension 落在 .falcon.tmp（最终名不存在，覆盖门禁
    // 放行——恢复场景的正确形态）
    const std::string url_primary = server.url("/f.bin");
    ResumeControl control;
    control.url = url_primary;
    control.total = body.size();
    control.etag = "\"falcon-mirror-etag\"";
    control.segments = {{0, body.size(), 1024}};
    ASSERT_TRUE(save_resume_control(out_path + kResumeControlExtension,
                                    control));
    {
        std::ofstream temp(out_path + ".falcon.tmp", std::ios::binary);
        temp << body.substr(0, 1024);
    }

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 1;
    options.retry_delay_seconds = 0;

    const TaskId task_id = engine.add_download(
        std::vector<std::string>{url_primary, server.url("/g.bin")}, options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    // 主镜像响应头阶段断连 → 换源重试落在非主镜像（剥 If-Range）→
    // 206 续传完成
    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(inj_read_file(out_path), body);

    bool primary_had_if_range = false;
    bool primary_had_range = false;
    bool mirror_had_range = false;
    bool mirror_had_if_range = false;
    for (const auto& r : server.requests()) {
        if (r.method != "GET") continue;
        if (r.path == "/f.bin") {
            if (!r.range.empty()) primary_had_range = true;
            const auto it = r.headers.find("if-range");
            if (it != r.headers.end() && !it->second.empty())
                primary_had_if_range = true;
        }
        if (r.path == "/g.bin") {
            if (!r.range.empty()) mirror_had_range = true;
            const auto it = r.headers.find("if-range");
            if (it != r.headers.end() && !it->second.empty())
                mirror_had_if_range = true;
        }
    }
    EXPECT_TRUE(primary_had_range && primary_had_if_range)
        << "主镜像恢复连接必须武装 Range + If-Range（剥离前的 apply 铁证）";
    EXPECT_TRUE(mirror_had_range)
        << "轮转承接连接必须携带 Range（续传范围原样传递）";
    EXPECT_FALSE(mirror_had_if_range)
        << "非主镜像承接的恢复连接不应携带 If-Range";

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

#ifdef FALCON_ENABLE_OPENSSL

// 握手首轮 WANT_WRITE 属进行中而非失败：重入续推后下载自然完成
TEST(DownloadEngineV2Injection, TlsHandshakeWantWriteRecovers) {
    const std::string body = testtls::make_body(128 * 1024);

    const std::string dir = inj_temp_dir("wantwrite");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    testtls::TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = false;

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    {
        detail::ScopedInjection injection(
            detail::InjectPoint::TlsHandshakeWantWrite);
        testtls::TlsEngineRunner runner(engine);
        ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
        ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
        runner.shutdown_and_join();
    }
    server.stop();

    EXPECT_EQ(inj_read_file(out_path), body);
    EXPECT_EQ(server.handshakes(), 1);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 请求发送 SSL_write WANT_WRITE（进行中语义非失败）：握手真实完成
// 后请求写入被注入拦下（未发出任何字节），清注入后重入以真实
// SSL_write 续推，下载自然完成——恰一次握手钉住无重连
TEST(DownloadEngineV2Injection, TlsRequestSendWantWriteSuspendsThenRecovers) {
    const std::string body = testtls::make_body(64 * 1024);

    const std::string dir = inj_temp_dir("tlswrite");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    testtls::TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = false;

    // 注入先于任务创建置位：握手完成后的请求 SSL_write 必然被拦
    detail::set_injection(detail::InjectPoint::TlsRequestWriteWantWrite, true);
    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    // 锚点：服务器观测到完整握手（客户端收 Finished 后立刻落入请求
    // 发送）；静置窗口保证 SSL_write 已被拦下，清注入不早于挂起
    ASSERT_TRUE(testtls::wait_for(
        [&] { return server.handshakes() >= 1; }, 10000));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    detail::set_injection(detail::InjectPoint::TlsRequestWriteWantWrite, false);

    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(inj_read_file(out_path), body);
    EXPECT_EQ(server.handshakes(), 1);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 真实握手完成后的 SSL_write 硬失败：任务 FAILED 干净收口，服务器
// 观测到恰一次握手但从未收到完整请求
TEST(DownloadEngineV2Injection, TlsRequestWriteFailFailsCleanly) {
    const std::string body = testtls::make_body(32 * 1024);

    const std::string dir = inj_temp_dir("reqwrite");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    testtls::TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = false;

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    {
        detail::ScopedInjection injection(
            detail::InjectPoint::TlsRequestWriteFail);
        testtls::TlsEngineRunner runner(engine);
        ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
        EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    // 服务器侧计数与本用例的失败收口存在结构性竞速：客户端「TLS 握手
    // 成功」→ 注入失败 → FAILED 收口关连接，微秒级；服务器线程此刻
    // 还在 SSL_accept 里等客户端 Finished。
    //  - Linux/macOS：close 走 FIN，已收的 Finished 数据仍可读，
    //    SSL_accept 成功返回后计数 1（等待式收敛即可观测）；
    //  - Windows：TLS 1.3 服务器握手后发的 NewSessionTicket 躺在客户
    //    端接收缓冲未被读，closesocket 对接收缓冲非空的连接发 RST
    //    （Windows 特有语义），服务器 SSL_accept 被重置打断恒失败，
    //    计数恒 0——机制性结果非调度抖动（run 35484110282 实证 5s
    //    等待仍为 0）。
    // 注入点 TlsRequestWriteFail 位于握手完成后的 SSL_write，注入命
    // 中本身就是「握手已完成」的结构性证据，服务器侧计数只作观测补
    // 充：观察到则钉住恰一次，观察不到不再硬断言。
    if (testtls::wait_for([&] { return server.handshakes() >= 1; },
                          5000)) {
        EXPECT_EQ(server.handshakes(), 1);
    }
    EXPECT_TRUE(inj_read_file(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 请求发送 SSL_write WANT_WRITE 挂起后的事件注册失败：同明文版——
// 注册失败立即按发送失败收口。EventPollAddFail 不能提前置位（会拦下
// 握手自身的挂起注册），时序叠加：握手真实完成且 SSL_write 已被拦
// 挂起后置位——重入续推仍被拦（WantWrite 未清），挂起前的 WRITE
// 注册失败即收口
TEST(DownloadEngineV2Injection, TlsSendWantWriteRegistrationFailsCleanly) {
    const std::string body = testtls::make_body(64 * 1024);

    const std::string dir = inj_temp_dir("tlswritereg");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    testtls::TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = false;

    {
        detail::ScopedInjection send_ww(
            detail::InjectPoint::TlsRequestWriteWantWrite);
        const TaskId task_id = engine.add_download(
            server.url("localhost", "/f.bin"), options);
        ASSERT_GT(task_id, 0u);
        auto* group = engine.request_group_man()->find_group(task_id);
        ASSERT_NE(group, nullptr);

        testtls::TlsEngineRunner runner(engine);
        // 锚点：握手真实完成（SSL_write 才会执行）且请求已被拦挂起；
        // 此时置位 EventPollAddFail——WANT_WRITE 挂起是循环的，重入
        // 续推仍被拦，随后的事件注册必然失败
        ASSERT_TRUE(testtls::wait_for(
            [&] { return server.handshakes() >= 1; }, 10000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        detail::ScopedInjection add_fail(
            detail::InjectPoint::EventPollAddFail);

        ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
        ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    EXPECT_EQ(server.handshakes(), 1);
    EXPECT_TRUE(inj_read_file(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// TLS 握手推进后的事件注册失败（WANT_WRITE 注册）：服务器延迟握手
//（ServerHello 1.5s 后才来）让 SSL_connect 持续报 WANT_READ，
// TlsHandshakeWantWrite 把非完成返回改写为 WANT_WRITE（持续位）——
// fd 恒可写形成「注册 WRITE → 唤醒重入 → 伪造 WANT_WRITE」循环，
// 锚定循环稳定窗置位 EventPollAddFail 必然命中下一轮 WRITE 注册，
// 跳出 switch 按握手失败收口。全程 AddFail 会把 connect 阶段的注册
// 一并拦下（命中已覆盖的 586 收口），故必须延迟置位
TEST(DownloadEngineV2Injection, TlsHandshakeRegistrationFailFailsCleanly) {
    const std::string body = testtls::make_body(32 * 1024);

    const std::string dir = inj_temp_dir("tlsreg");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    testtls::TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);
    server.set_handshake_delay_ms(1500);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = false;
    options.timeout_seconds = 10;

    {
        detail::ScopedInjection hand_ww(
            detail::InjectPoint::TlsHandshakeWantWrite);
        const TaskId task_id = engine.add_download(
            server.url("localhost", "/f.bin"), options);
        ASSERT_GT(task_id, 0u);
        auto* group = engine.request_group_man()->find_group(task_id);
        ASSERT_NE(group, nullptr);

        testtls::TlsEngineRunner runner(engine);
        // 锚点：改写循环已稳定运转（首调 ClientHello 已真实写出，
        // ServerHello 被服务器延迟挡住，每轮伪造 WANT_WRITE）——
        // 静置窗口远大于一个 poll 周期
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        detail::ScopedInjection add_fail(
            detail::InjectPoint::EventPollAddFail);

        ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
        ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    // 客户端在服务器开始 SSL_accept 前已收口关闭：握手从未完成，
    // 服务器侧零握手（若意外推进完成则下载会继续，终态断言已红）
    EXPECT_EQ(server.handshakes(), 0);
    EXPECT_TRUE(inj_read_file(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

#endif  // FALCON_ENABLE_OPENSSL

#if !defined(_WIN32)

// /dev/full（写恒 ENOSPC）：缓冲写路径——中途冲刷失败必须按段错误
// 收口（磁盘满绝不假报 COMPLETED）
TEST(DownloadEngineV2Injection, DiskFullBufferedFlushFailsCleanly) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(64 * 1024, 'x');
    testscripts::FakeResponse resp;
    resp.body = body;
    server.set_response("/f.bin", resp);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.temp_extension = "";          // 直写最终名（/dev/full 本体）
    config.disk_cache_size = 16 * 1024;  // 小缓冲：中途冲刷即失败
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = "/dev/full";
    options.overwrite_existing = true;  // 门禁放行（设备文件恒"存在"）
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(
        server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();
}

// /dev/full：直写路径（无磁盘写缓冲）同样干净失败
TEST(DownloadEngineV2Injection, DiskFullDirectWriteFailsCleanly) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(64 * 1024, 'x');
    testscripts::FakeResponse resp;
    resp.body = body;
    server.set_response("/f.bin", resp);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.temp_extension = "";
    config.enable_disk_cache = false;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = "/dev/full";
    options.overwrite_existing = true;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(
        server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();
}

// /dev/full：直写路径首次落盘即失败——响应头单独到达（body 首块延迟
// 1ms），filebuf 为空时 >8KB 单次写绕过用户态缓冲直落 syscall，ENOSPC
// 在 write() 调用点当场暴露（seekp 成功、write 失败的分流）
TEST(DownloadEngineV2Injection, DiskFullDirectFirstWriteFailsCleanly) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(32 * 1024, 'w');
    testscripts::FakeResponse resp;
    resp.body = body;
    server.set_response("/f.bin", resp);
    // body 首块 16KB（> ofstream filebuf 的 8KB）且延迟 200ms 发送
    // （远大于引擎 poll 周期，确保头部到达时 body 尚未发出）：
    // 初始批次为空、filebuf 保持空，首次直写绕过用户态缓冲当场失败
    server.set_slow_body("/f.bin", 200000, 16 * 1024);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.temp_extension = "";
    config.enable_disk_cache = false;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = "/dev/full";
    options.overwrite_existing = true;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(
        server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();
}

// /dev/full：大缓冲 + 小 body——全部数据攒在内存缓冲、全程不触发容量
// 冲刷，收满后完成路径的 finish_output 冲刷失败必须按失败收口
// （磁盘满绝不假报 COMPLETED）
TEST(DownloadEngineV2Injection, DiskFullCompletionFlushFailsCleanly) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(8 * 1024, 'y');
    testscripts::FakeResponse resp;
    resp.body = body;
    server.set_response("/f.bin", resp);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.temp_extension = "";
    config.disk_cache_size = 1024 * 1024;  // 缓冲大于 body：无中途冲刷
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = "/dev/full";
    options.overwrite_existing = true;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(
        server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();
}

// /dev/full：头 + body 单次 send + 小 body——响应头与完整 body 同批
// 到达，首次 execute 即收满完成，完成路径的 finish_output 冲刷失败
// 必须按失败收口（磁盘满绝不假报 COMPLETED）
//
// 测量备注：初始批次写失败分支（"Failed to write initial body
// bytes"）经 /dev/full 实证结构性不可达——初始批次 ≤ 响应头接收缓冲
// （4KB），ofstream filebuf（8KB）将其全部吞入用户态缓冲、流状态
// 保持良好，ENOSPC 要到下一次 seekp 冲刷或 close() 时才浮现（由
// 2689/2644/2375 等后续失败点收口），初始写永远"成功"
TEST(DownloadEngineV2Injection, DiskFullFirstExecuteCompletionFlushFails) {
    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string body(2 * 1024, 'z');
    testscripts::FakeResponse resp;
    resp.body = body;
    resp.single_write = true;
    server.set_response("/f.bin", resp);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.temp_extension = "";
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = "/dev/full";
    options.overwrite_existing = true;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(
        server.url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    testtls::TlsEngineRunner runner(engine);
    ASSERT_TRUE(testtls::wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();
}

#endif  // !_WIN32
