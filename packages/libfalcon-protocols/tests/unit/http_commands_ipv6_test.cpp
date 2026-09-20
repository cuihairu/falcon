/**
 * @file http_commands_ipv6_test.cpp
 * @brief V2 引擎 IPv6 数据面端到端测试
 * @author Falcon Team
 * @date 2026-09-20
 *
 * 覆盖的核心不变量：
 * - [::1] 字面量 URL：方括号解析（RFC 3986 §3.2.2）→ AF_INET6 socket
 *   → 下载完成逐字节一致；线上 Host 头保留括号形态
 * - AAAA-only 主机名经 resolve_host 回落 IPv6 地址族（仅 POSIX 且
 *   解析结果纯 v6 时执行，其余环境 GTEST_SKIP）
 * - 多连接分段每段一条 socket，地址族一致，成品逐字节一致
 * - 重定向：v6 括号 authority 基准上的相对 Location 解析 + 重新连接
 * - 畸形括号 authority（无闭合 ]）干净失败，不崩溃
 * - TLS：IP 字面量直连（v6/v4）verify 按 IP SAN 匹配（此前
 *   SSL_set1_host 恒 hostname mismatch 的缺陷修复）；IP 直连不发 SNI
 *
 * 环境探测：dual-stack 监听建立失败（无 IPv6）的用例 GTEST_SKIP。
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>

#include "scripted_http_server.hpp"
#include "tls_loopback_server.hpp"

#include <filesystem>
#include <string>

namespace {

using falcon::testscripts::FakeResponse;
using falcon::testscripts::ScriptedHttpServer;
using falcon::testtls::TlsEngineRunner;
using falcon::testtls::wait_group_terminal;
using falcon::DownloadOptions;
using falcon::DownloadEngineV2;
using falcon::EngineConfigV2;
using falcon::RequestGroup;
using falcon::RequestGroupStatus;
using falcon::TaskId;

namespace fs = std::filesystem;

DownloadOptions v6_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    return options;
}

/// dual-stack 服务器 + 环境探测封装：返回 false 表示环境无 IPv6
bool start_dual_stack(ScriptedHttpServer& server) {
    return server.start_v6();
}

void expect_terminal(DownloadEngineV2& engine,
                     RequestGroup* group,
                     RequestGroupStatus expected) {
    ASSERT_NE(group, nullptr);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000))
        << "任务未在时限内到达终态";
    EXPECT_EQ(group->status(), expected);
}

/// [::1] 字面量下载：括号解析 + AF_INET6 socket + Host 头带括号
TEST(DownloadEngineV2Ipv6, LiteralBracketDownloadCompletes) {
    ScriptedHttpServer server;
    ASSERT_TRUE(start_dual_stack(server))
        << "环境无 IPv6，跳过";
    const std::string body = falcon::testtls::make_body(256 * 1024);
    FakeResponse resp;
    resp.body = body;
    server.set_response("/f.bin", resp);

    const std::string dir = falcon::testtls::temp_dir_for("v6_literal");
    fs::create_directories(dir);
    const std::string out_path = (fs::path(dir) / "f.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    falcon::DownloadEngineV2 engine(config);

    const falcon::TaskId task_id =
        engine.add_download(server.url_v6("/f.bin"), v6_options(out_path));
    ASSERT_GT(task_id, 0u);

    TlsEngineRunner runner(engine);
    expect_terminal(
        engine, engine.request_group_man()->find_group(task_id),
        falcon::RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();

    EXPECT_EQ(falcon::testtls::read_file_content(out_path), body);

    // 线上 Host 头：IPv6 字面量按 RFC 3986 保留方括号
    const auto reqs = server.requests();
    ASSERT_FALSE(reqs.empty());
    const auto host_it = reqs[0].headers.find("host");
    ASSERT_NE(host_it, reqs[0].headers.end());
    EXPECT_EQ(host_it->second, "[::1]");

    server.stop();
    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
}

/// 双栈服务器上 v4 URL 照常工作（dual-stack 改动不破坏既有路径）
TEST(DownloadEngineV2Ipv6, V4StillWorksOnDualStackServer) {
    ScriptedHttpServer server;
    ASSERT_TRUE(start_dual_stack(server)) << "环境无 IPv6，跳过";
    const std::string body = falcon::testtls::make_body(64 * 1024);
    FakeResponse resp;
    resp.body = body;
    server.set_response("/g.bin", resp);

    const std::string dir = falcon::testtls::temp_dir_for("v6_dual_v4");
    fs::create_directories(dir);
    const std::string out_path = (fs::path(dir) / "g.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    falcon::DownloadEngineV2 engine(config);

    const falcon::TaskId task_id =
        engine.add_download(server.url("/g.bin"), v6_options(out_path));
    ASSERT_GT(task_id, 0u);

    TlsEngineRunner runner(engine);
    expect_terminal(
        engine, engine.request_group_man()->find_group(task_id),
        falcon::RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();

    EXPECT_EQ(falcon::testtls::read_file_content(out_path), body);
    EXPECT_EQ(server.count_requests("/g.bin"), 1u);

    server.stop();
    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
}

#ifndef _WIN32
/// AAAA-only 主机名：resolve_host 无 IPv4 可选时回落 IPv6 地址族。
/// 仅当 ip6-localhost 解析结果纯 v6（无 A 记录）时执行——存在 A 记录
/// 的环境会命中"优先 IPv4"分支，测不到 v6 解析路径
TEST(DownloadEngineV2Ipv6, HostnameResolutionFallsBackToV6) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo("ip6-localhost", nullptr, &hints, &res) != 0) {
        GTEST_SKIP() << "ip6-localhost 不可解析";
    }
    bool has_v6 = false;
    bool has_v4 = false;
    for (auto* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET6) has_v6 = true;
        if (p->ai_family == AF_INET) has_v4 = true;
    }
    freeaddrinfo(res);
    if (!has_v6 || has_v4) {
        GTEST_SKIP() << "ip6-localhost 非 AAAA-only，无法确定走 v6 分支";
    }

    ScriptedHttpServer server;
    ASSERT_TRUE(start_dual_stack(server)) << "环境无 IPv6，跳过";
    const std::string body = falcon::testtls::make_body(48 * 1024);
    FakeResponse resp;
    resp.body = body;
    server.set_response("/h.bin", resp);

    const std::string dir = falcon::testtls::temp_dir_for("v6_hostname");
    fs::create_directories(dir);
    const std::string out_path = (fs::path(dir) / "h.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    falcon::DownloadEngineV2 engine(config);

    const falcon::TaskId task_id = engine.add_download(
        "http://ip6-localhost:" + std::to_string(server.port()) + "/h.bin",
        v6_options(out_path));
    ASSERT_GT(task_id, 0u);

    TlsEngineRunner runner(engine);
    expect_terminal(
        engine, engine.request_group_man()->find_group(task_id),
        falcon::RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();

    EXPECT_EQ(falcon::testtls::read_file_content(out_path), body);

    server.stop();
    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
}
#endif  // !_WIN32

/// 多连接分段经 IPv6：每段一条 AF_INET6 socket，成品逐字节一致
TEST(DownloadEngineV2Ipv6, MultiSegmentV6Download) {
    ScriptedHttpServer server;
    ASSERT_TRUE(start_dual_stack(server)) << "环境无 IPv6，跳过";
    const std::string body = falcon::testtls::make_body(4 * 1024 * 1024);
    FakeResponse resp;
    resp.body = body;
    resp.support_range = true;
    // 多段判定由 GET 响应显式 Accept-Ranges 头驱动
    resp.headers = {{"Accept-Ranges", "bytes"}};
    server.set_response("/big.bin", resp);

    const std::string dir = falcon::testtls::temp_dir_for("v6_multi");
    fs::create_directories(dir);
    const std::string out_path = (fs::path(dir) / "big.bin").string();

    DownloadOptions options = v6_options(out_path);
    options.max_connections = 4;
    options.min_segment_size = 256 * 1024;

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    falcon::DownloadEngineV2 engine(config);

    const falcon::TaskId task_id =
        engine.add_download(server.url_v6("/big.bin"), options);
    ASSERT_GT(task_id, 0u);

    TlsEngineRunner runner(engine);
    expect_terminal(
        engine, engine.request_group_man()->find_group(task_id),
        falcon::RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();

    EXPECT_EQ(falcon::testtls::read_file_content(out_path), body);
    // 4 段计划 + 1 次首连判定（HEAD 无，GET 初始连接 + 段连接）
    EXPECT_GE(server.requests().size(), 4u);

    server.stop();
    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
}

/// v6 括号 authority 基准上的相对 Location：解析 + 重新连接都走
/// 括号形态（resolve_redirect_location 生成的 URL 再进构造函数）
TEST(DownloadEngineV2Ipv6, RedirectRelativeOnV6Authority) {
    ScriptedHttpServer server;
    ASSERT_TRUE(start_dual_stack(server)) << "环境无 IPv6，跳过";
    const std::string body = falcon::testtls::make_body(32 * 1024);
    FakeResponse redirect_resp;
    redirect_resp.status = 302;
    redirect_resp.status_text = "Found";
    redirect_resp.headers = {{"Location", "../up/f.bin"}};
    // 相对 Location "../up/f.bin" 基于请求目录 /a/ 解析：上跳归一化
    // 后为 /up/f.bin（RFC 3986 §5.2.4）
    server.set_response("/a/rel.bin", redirect_resp);
    FakeResponse final_resp;
    final_resp.body = body;
    server.set_response("/up/f.bin", final_resp);

    const std::string dir = falcon::testtls::temp_dir_for("v6_redirect");
    fs::create_directories(dir);
    const std::string out_path = (fs::path(dir) / "f.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    falcon::DownloadEngineV2 engine(config);

    const falcon::TaskId task_id = engine.add_download(
        server.url_v6("/a/rel.bin"), v6_options(out_path));
    ASSERT_GT(task_id, 0u);

    TlsEngineRunner runner(engine);
    expect_terminal(
        engine, engine.request_group_man()->find_group(task_id),
        falcon::RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();

    EXPECT_EQ(falcon::testtls::read_file_content(out_path), body);
    EXPECT_EQ(server.count_requests("/a/rel.bin"), 1u);
    EXPECT_EQ(server.count_requests("/up/f.bin"), 1u);

    server.stop();
    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
}

/// 畸形括号 authority（无闭合 ]）：不崩溃，任务按失败干净收口
TEST(DownloadEngineV2Ipv6, MalformedBracketAuthorityFailsCleanly) {
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    falcon::DownloadEngineV2 engine(config);

    const std::string dir = falcon::testtls::temp_dir_for("v6_malformed");
    fs::create_directories(dir);
    const std::string out_path = (fs::path(dir) / "x.bin").string();

    const falcon::TaskId task_id = engine.add_download(
        "http://[::1/no-close.bin", v6_options(out_path));
    ASSERT_GT(task_id, 0u);

    TlsEngineRunner runner(engine);
    expect_terminal(
        engine, engine.request_group_man()->find_group(task_id),
        falcon::RequestGroupStatus::FAILED);
    runner.shutdown_and_join();

    // 无成品落盘
    EXPECT_FALSE(fs::exists(out_path));

    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
}

#ifdef FALCON_ENABLE_OPENSSL
/// IPv6 字面量 TLS 直连：verify 按 IP SAN 匹配（set1_ip_asc 修复此前
/// SSL_set1_host 恒 hostname mismatch 的缺陷）；IP 直连不发 SNI
TEST(DownloadEngineV2Ipv6, TlsV6IpLiteralVerifiesViaIpSan) {
    const std::string dir = falcon::testtls::temp_dir_for("v6_tls");
    fs::create_directories(dir);
    const auto key_path = (fs::path(dir) / "k.pem").string();
    const auto cert_path = (fs::path(dir) / "c.pem").string();

    falcon::testtls::TlsTestServer tls;
    ASSERT_TRUE(tls.start(key_path, cert_path, /*dual_stack=*/true))
        << "环境无 IPv6，跳过";
    const std::string body = falcon::testtls::make_body(96 * 1024);
    tls.set_body(body);

    DownloadOptions options = v6_options((fs::path(dir) / "t.bin").string());
    options.verify_ssl = true;

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    falcon::DownloadEngineV2 engine(config);

    const falcon::TaskId task_id = engine.add_download(
        tls.url("[::1]", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);

    // 信任自签证书（SAN 含 IP:::1）
    falcon::testtls::ScopedEnvVar cert_env("SSL_CERT_FILE", cert_path);

    TlsEngineRunner runner(engine);
    expect_terminal(
        engine, engine.request_group_man()->find_group(task_id),
        falcon::RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();

    EXPECT_EQ(
        falcon::testtls::read_file_content(options.output_filename), body);
    // RFC 6066：IP 字面量不得进 SNI HostName
    EXPECT_EQ(tls.sni(), "");

    tls.stop();
    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
}

/// IPv4 字面量 TLS 直连：同一缺陷的 v4 侧回归钉子（修复前
/// verify_ssl=true 恒 FAILED），同样不发 SNI
TEST(DownloadEngineV2Ipv6, TlsV4IpLiteralVerifiesViaIpSan) {
    const std::string dir = falcon::testtls::temp_dir_for("v4_tls_ip");
    fs::create_directories(dir);
    const auto key_path = (fs::path(dir) / "k.pem").string();
    const auto cert_path = (fs::path(dir) / "c.pem").string();

    falcon::testtls::TlsTestServer tls;
    ASSERT_TRUE(tls.start(key_path, cert_path));
    const std::string body = falcon::testtls::make_body(64 * 1024);
    tls.set_body(body);

    DownloadOptions options = v6_options((fs::path(dir) / "t.bin").string());
    options.verify_ssl = true;

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    falcon::DownloadEngineV2 engine(config);

    const falcon::TaskId task_id = engine.add_download(
        tls.url("127.0.0.1", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);

    falcon::testtls::ScopedEnvVar cert_env("SSL_CERT_FILE", cert_path);

    TlsEngineRunner runner(engine);
    expect_terminal(
        engine, engine.request_group_man()->find_group(task_id),
        falcon::RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();

    EXPECT_EQ(
        falcon::testtls::read_file_content(options.output_filename), body);
    EXPECT_EQ(tls.sni(), "");

    tls.stop();
    std::error_code rm_ec;
    fs::remove_all(dir, rm_ec);
}
#endif  // FALCON_ENABLE_OPENSSL

} // namespace
