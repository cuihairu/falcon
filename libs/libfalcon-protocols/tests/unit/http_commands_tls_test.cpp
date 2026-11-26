/**
 * @file http_commands_tls_test.cpp
 * @brief V2 引擎 HTTPS/TLS 端到端测试（异步握手 + 证书校验硬断连 + SNI）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 运行时自签证书回环覆盖的核心不变量：
 * - https:// 请求经异步 TLS 握手（非阻塞 socket 上 WANT_* 置
 *   TLS_HANDSHAKING 等 socket 事件重入续推）后完整下载，成品一致
 * - verify_ssl=true 对自签证书必须硬失败——回归此前的 WARN-only
 *   行为（校验失败告警后照常收数据，TLS 形同虚设）
 * - SNI 随握手发出（服务器侧观测），verify_ssl=true 且信任自签 CA
 *   （SSL_CERT_FILE）+ SAN 主机名匹配时校验通过下载成功
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
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#define CLOSE_SOCKET(fd) close(fd)
#define POLL(fd_ptr, count, timeout_ms) ::poll((fd_ptr), (count), (timeout_ms))
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>

#ifdef FALCON_ENABLE_OPENSSL
#include "tls_loopback_server.hpp"
#endif

#include <filesystem>
#include <string>

using namespace falcon;

#ifdef FALCON_ENABLE_OPENSSL

using namespace falcon::testtls;

namespace {

/**
 * @brief 非阻塞异步握手全链路：WANT_* 置 TLS_HANDSHAKING 等 socket 事件
 *        重入续推，完成后明文 HTTP 语义照常工作
 */
TEST(DownloadEngineV2Tls, HttpsAsyncHandshakeDownloadSucceeds) {
    const std::string body = make_body(128 * 1024);

    const std::string dir = temp_dir_for("handshake");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    TlsTestServer server;
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
    options.verify_ssl = false;  // 自签证书：跳过校验

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    EXPECT_EQ(server.handshakes(), 1);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// verify_ssl=true 对自签证书必须硬失败（回归 WARN-only：旧行为
/// 校验失败仅告警即继续收数据，成品照常落盘）
TEST(DownloadEngineV2Tls, VerifySslFailsHardOnSelfSigned) {
    const std::string body = make_body(32 * 1024);

    const std::string dir = temp_dir_for("verify");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    TlsTestServer server;
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
    options.verify_ssl = true;  // 自签证书不在信任锚内，必须拒绝

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "自签证书必须硬失败，不得 WARN 后继续";

    runner.shutdown_and_join();
    server.stop();

    // 半成品不顶最终名
    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// SNI 随握手发出 + 正向校验：信任自签 CA（SSL_CERT_FILE）且 SAN
/// 主机名匹配（SSL_set1_host 绑定 localhost）时校验通过、下载成功
TEST(DownloadEngineV2Tls, SniSentAndPositiveVerificationSucceeds) {
    const std::string body = make_body(64 * 1024);

    const std::string dir = temp_dir_for("sni");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    // 客户端信任锚 = 自签证书（默认验证路径会读取 SSL_CERT_FILE）
    ScopedEnvVar trusted_ca("SSL_CERT_FILE", cert_path);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = true;

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    // SNI 与证书 CN/SAN 同源（SSL_set_tlsext_host_name(host_)）
    EXPECT_EQ(server.sni(), "localhost");
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 双向 TLS（mTLS）：客户端出示证书（aria2 --certificate/--private-key
/// 同语义）——服务器要求客户端证书时握手成立、下载完成，服务器侧
/// 观测到客户端证书
TEST(DownloadEngineV2Tls, MutualTlsWithClientCertSucceeds) {
    const std::string body = make_body(64 * 1024);

    const std::string dir = temp_dir_for("mtls");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();
    const std::string client_key_path =
        (std::filesystem::path(dir) / "client_key.pem").string();
    const std::string client_cert_path =
        (std::filesystem::path(dir) / "client_cert.pem").string();
    ASSERT_TRUE(falcon_test_tls::generate_client_cert(client_key_path,
                                                      client_cert_path));

    TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path, /*dual_stack=*/false,
                             client_cert_path));
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
    options.verify_ssl = false;  // 本用例只验客户端证书路径
    options.client_certificate = client_cert_path;
    options.client_private_key = client_key_path;

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.handshakes(), 1);
    EXPECT_EQ(server.client_certs(), 1);
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 服务器要求客户端证书而客户端未出示：握手失败，任务干净 FAILED
///（SSL_accept 以 certificate required 告警告败，handshakes 不增长）
TEST(DownloadEngineV2Tls, MutualTlsWithoutClientCertFailsCleanly) {
    const std::string dir = temp_dir_for("mtls_nocert");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();
    const std::string client_key_path =
        (std::filesystem::path(dir) / "client_key.pem").string();
    const std::string client_cert_path =
        (std::filesystem::path(dir) / "client_cert.pem").string();
    ASSERT_TRUE(falcon_test_tls::generate_client_cert(client_key_path,
                                                      client_cert_path));

    TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path, /*dual_stack=*/false,
                             client_cert_path));
    server.set_body(make_body(1024));

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
    // 不带客户端证书

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    server.stop();

    // 客户端在 CertificateRequest 后未回应证书，服务器握手从未完成
    EXPECT_EQ(server.handshakes(), 0);
    EXPECT_EQ(server.client_certs(), 0);
    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 半配置（只有证书没有私钥）：本地加载失败，握手前干净收口——
/// 配置错误绝不退化成匿名连接
TEST(DownloadEngineV2Tls, ClientCertWithoutKeyFailsBeforeHandshake) {
    const std::string dir = temp_dir_for("mtls_half");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();
    const std::string client_key_path =
        (std::filesystem::path(dir) / "client_key.pem").string();
    const std::string client_cert_path =
        (std::filesystem::path(dir) / "client_cert.pem").string();
    ASSERT_TRUE(falcon_test_tls::generate_client_cert(client_key_path,
                                                      client_cert_path));

    TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path, /*dual_stack=*/false,
                             client_cert_path));
    server.set_body(make_body(1024));

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
    options.client_certificate = client_cert_path;
    // 故意缺 client_private_key

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    server.stop();

    // check_private_key 在握手开始前失败——ClientHello 未发出
    EXPECT_EQ(server.handshakes(), 0);
    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

} // namespace

#else  // !FALCON_ENABLE_OPENSSL

// 无 OpenSSL 构建：TLS 不可用，本文件无测试（避免空翻译单元告警）
namespace falcon_tls_placeholder {
inline int placeholder() { return 0; }
}  // namespace falcon_tls_placeholder

#endif  // FALCON_ENABLE_OPENSSL
