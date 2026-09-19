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

    EXPECT_EQ(server.handshakes(), 1);
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
