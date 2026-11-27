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
// 本文件全部走 V1 curl 数据面。

#include "plugins/http/http_handler.hpp"

#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/exceptions.hpp>

#include <falcon/detail/injection.hpp>
#include "scripted_http_server.hpp"

#include <curl/curl.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

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
    const std::string seg0 = out + ".falcon.tmp.seg0";

    // 段 0 路径被空目录占用:修复前的 CI 实证缺陷——恢复检测循环对
    // 段路径 ifstream 打开目录同样成功(glibc fopen 目录不拒),
    // tellg() 返回目录 st_size(文件系统相关,CI /tmp 上可远大于段
    // 大小),命中"超尺寸段不可信"分支后 fs::remove 把空目录当损坏
    // 段文件删掉(rmdir 语义),下载照常完成——目录占用被静默"自愈",
    // 用户目录被下载器销毁。修复后目录必须保留并以下载失败收口
    std::error_code cd_ec;
    ASSERT_TRUE(fs::create_directories(seg0, cd_ec)) << cd_ec.message();

    const auto task = makeTask(218, server().url("/segdir.bin"), out, options);
    EXPECT_THROW(handler()->download(task, nullptr), FileIOException);

    // 占位目录必须原样保留(不得被任何"清段/删段"路径吞掉),成品不发布
    EXPECT_TRUE(fs::is_directory(seg0));
    EXPECT_FALSE(fs::exists(out));
}

// timeout_seconds 是停滞看门狗而非总时长硬上限(aria2 --timeout 同语义):
// 慢而健康的传输(1.6KB/s >> 1B/s 低速阈值)在总时长超过 timeout_seconds
// 时照常完成。旧实现把它映射 CURLOPT_TIMEOUT(整传输硬上限)时本用例
// 必红(2s 硬帽处 CURLE_OPERATION_TIMEDOUT -> NetworkException)——即
// 「HEAD 识别大小正常,30s 后必然失败」P0 的回归钉子(单连接路径)
TEST_F(HttpHandlerEdgesTest, SlowHealthyTransferSurvivesTimeoutSeconds) {
    const std::string content(4096, 's');
    FakeResponse resp;  // 无 Accept-Ranges -> supports_resume=false -> 单连接路径
    resp.body = content;
    server().set_response("/slowok.bin", resp);
    // 4096B / 16B 每 10ms ≈ 2.56s > timeout 2s(HEAD 不受慢发影响,服务器
    // 对 HEAD 恒全速应答,探测先快速通过)
    server().set_slow_body("/slowok.bin", 10'000, 16);

    DownloadOptions options;
    options.timeout_seconds = 2;
    options.max_retries = 0;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("slowok.bin");
    const auto task = makeTask(224, server().url("/slowok.bin"), out, options);

    const auto t0 = std::chrono::steady_clock::now();
    handler()->download(task, nullptr);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(out), content);
    // 传输总时长确已越过 timeout_seconds——区分新旧语义的决定性观测
    EXPECT_GE(elapsed_ms, 2000);
}

// 停滞中止面:连接建立、请求发出后服务器零字节挂死——LOW_SPEED 1B/s
// 看门狗恰在 timeout_seconds 处中止(CURLE_OPERATION_TIMEDOUT ->
// NetworkException)。partial_prefix 置空是刻意的:默认前缀自带 17 字节
// 状态行(17B/s > 1B/s),curl 速度采样下中止会被推迟,零字节才能确定性
// 触发。HEAD 走 set_head_response 全速应答先行通过(黑名单/立即失败等
// 剧本与方法无关会先杀掉 HEAD 探测,不可用于本用例)
TEST_F(HttpHandlerEdgesTest, StalledTransferAbortsAtTimeoutSeconds) {
    FakeResponse ok_head;  // HEAD 探测必须先通过(全速 200 + Content-Length)
    ok_head.body = std::string(4096, 't');
    server().set_head_response("/stall.bin", ok_head);

    FakeResponse stalled;  // GET:延迟 100ms 后零字节挂死(recv 阻塞到客户端断开)
    stalled.body = std::string(4096, 't');
    stalled.defer_partial_ms = 100;
    stalled.partial_prefix = "";
    server().set_response("/stall.bin", stalled);

    DownloadOptions options;
    options.timeout_seconds = 1;
    options.max_retries = 0;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("stall.bin");
    const auto task = makeTask(225, server().url("/stall.bin"), out, options);

    const auto t0 = std::chrono::steady_clock::now();
    bool threw_network = false;
    std::string diag_what;
    try {
        handler()->download(task, nullptr);
    } catch (const NetworkException& e) {
        threw_network = true;
        diag_what = e.what();
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();

    // NAIL-DIAG Windows 确定性 ~140ms 红面取证(两个 CI 回合 138/143ms
    // 同断言同形态;LOW_SPEED 看门狗 >=1s 数学上不可能在此收口,abort
    // 来源待定)。打印 curl 错误消息与服务器观测请求序(GET 是否到达/
    // 是否带 Range)后照常断言——用例红时日志自动携带决定性证据。
    std::cerr << "[stall-diag] elapsed_ms=" << elapsed_ms
              << " network_exception=" << threw_network
              << " what=\"" << diag_what << "\" requests=[";
    for (const auto& req : server().requests()) {
        std::cerr << "{" << req.method << " " << req.path << " range=\""
                  << req.range << "\"}";
    }
    std::cerr << "] events=[";
    for (const auto& ev : server().conn_events()) {
        std::cerr << "{" << ev.what << " conn=" << ev.conn << " ms=" << ev.ms
                  << " n=" << ev.n << " err=" << ev.err << "}";
    }
    std::cerr << "]\n";

    EXPECT_TRUE(threw_network);
    // 看门狗确实在 timeout_seconds 量级收口(既非立即失败=连接层问题,
    // 也非重试退避拖长——max_retries=0 至多一次尝试)
    EXPECT_GE(elapsed_ms, 900);
    EXPECT_LT(elapsed_ms, 10'000);
    // 半成品绝不顶着最终名发布
    EXPECT_FALSE(fs::exists(out));
}

// 段路径同语义钉死(download_segment_curl 站点):分段连接各自的慢而健康
// 传输不受 timeout_seconds 总时长惩罚。旧 CURLOPT_TIMEOUT 映射下每段恰
// 在 2s 硬帽处被杀 -> 段下载失败 -> FileIOException;SegmentDownloader
// 自身不消费 timeout_seconds(仅 download_segment_curl 的 curl 选项),
// 本用例唯一观测点就是段连接的 curl 超时语义
TEST_F(HttpHandlerEdgesTest, SlowSegmentedTransferSurvivesTimeoutSeconds) {
    const std::string content(8192, 'g');
    FakeResponse head;
    head.headers = {{"Accept-Ranges", "bytes"}};
    head.body = content;
    head.support_range = true;
    server().set_response("/segslow.bin", head);
    // 2 段 x 4096B / 16B 每 10ms ≈ 每段 2.56s > timeout 2s(段连接各自
    // 慢发,总墙钟 ≈ 最慢段)
    server().set_slow_body("/segslow.bin", 10'000, 16);

    DownloadOptions options;
    options.max_connections = 2;
    options.min_segment_size = 1024;  // 8192B 文件分 2 段
    options.timeout_seconds = 2;
    options.max_retries = 0;
    options.retry_delay_seconds = 0;

    TempDir dir;
    const std::string out = dir.file("segslow.bin");
    const auto task = makeTask(226, server().url("/segslow.bin"), out, options);

    const auto t0 = std::chrono::steady_clock::now();
    handler()->download(task, nullptr);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(readFile(out), content);
    EXPECT_GE(elapsed_ms, 2000);
}

#if defined(FALCON_FAILURE_INJECTION)

// ============================================================================
// 故障注入：curl 句柄创建失败（OOM 防御路径，正常执行不可达）
// ============================================================================

TEST_F(HttpHandlerEdgesTest, GetFileInfoThrowsOnCurlInitFailure) {
    falcon::detail::ScopedInjection guard(
        falcon::detail::InjectPoint::CurlEasyInit);
    EXPECT_THROW(handler()->get_file_info("http://127.0.0.1:1/x.bin", {}),
                 falcon::NetworkException);
}

TEST_F(HttpHandlerEdgesTest, DownloadThrowsOnCurlInitFailure) {
    TempDir dir;
    const std::string out = dir.file("inj.bin");
    const auto task = makeTask(901, "http://127.0.0.1:1/inj.bin", out, {});
    falcon::detail::ScopedInjection guard(
        falcon::detail::InjectPoint::CurlEasyInit);
    EXPECT_THROW(handler()->download(task, nullptr), falcon::NetworkException);
}

// 下载主循环的 curl init 失败：HEAD（get_file_info）必须先成功——注入
// 全程挂会先命中 HEAD 阶段的 init（上一用例），到不了主循环。锚
// on_file_info 回调（task->set_file_info 在 download 线程内同步触发，
// 位于主循环 curl_easy_init 之前）在回调内置位注入，零竞速命中主循环
// 的 throw；置位/清理均手动（回调栈上 RAII 会在返回时即恢复）
class LateInitInjectionArmer : public IEventListener {
public:
    void on_file_info(TaskId, const FileInfo&) override {
        falcon::detail::set_injection(
            falcon::detail::InjectPoint::CurlEasyInit, true);
    }
};

TEST_F(HttpHandlerEdgesTest, DownloadLoopCurlInitFailsAfterHead) {
    TempDir dir;
    FakeResponse resp;
    resp.body = "late-init";
    server().set_response("/late-init.bin", resp);

    const auto task = makeTask(903, server().url("/late-init.bin"),
                               dir.file("late.bin"), {});
    LateInitInjectionArmer armer;
    task->set_listener(&armer);

    bool threw = false;
    try {
        handler()->download(task, nullptr);
    } catch (const falcon::NetworkException& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("Failed to initialize CURL"),
                  std::string::npos);
    }
    // 先清理再断言（EXPECT 失败不中断执行，清理必须恒达）
    falcon::detail::set_injection(
        falcon::detail::InjectPoint::CurlEasyInit, false);
    EXPECT_TRUE(threw);
    // HEAD 已成功、主循环 init 即失败：不产生任何输出文件
    EXPECT_FALSE(fs::exists(dir.file("late.bin")));
}

#endif  // FALCON_FAILURE_INJECTION

#if defined(FALCON_ENABLE_OPENSSL)
#include "tls_loopback_server.hpp"

// V1 curl 数据面的双向 TLS：客户端证书选项（aria2 --certificate/
// --private-key 同语义）经 CURLOPT_SSLCERT/SSLKEY 传播——服务器要求
// 客户端证书（FAIL_IF_NO_PEER_CERT），选项未接上即握手被拒，任务
// 不可能 Completed，故 Completed 本身就是选项生效的铁证
TEST_F(HttpHandlerEdgesTest, MutualTlsClientCertPropagatesToCurl) {
    // curl 的 PEM 客户端证书（CURLOPT_SSLCERT/SSLKEY + PEM 类型）只在
    // OpenSSL 系 TLS 后端有效；非 OpenSSL 后端按能力限制跳过并留待
    // 各自证书格式支持再放开：
    // - Windows vcpkg curl 默认 Schannel 后端不认 PEM 文件，加载直接报
    //   CURLE_SSL_CERTPROBLEM（"Problem with the local SSL certificate"，
    //   run 35488059307 实证）。
    // - macOS CI 链接系统 curl（SecureTransport 后端），PEM 客户端证书
    //   经弃用的 SecItemImport 路径导入新 runner 镜像上在握手期失败，
    //   报 CURLE_SSL_CONNECT_ERROR（"SSL connect error"，runs
    //   42372750556 / 42516711927 双轮复现）；这是后端能力限制而非接线
    //   缺陷。
    // 跳过前打印后端标识，future 红面在 ctest -V 输出中可直接辨认所链
    // 后端。
    const curl_version_info_data* vinfo =
        curl_version_info(CURLVERSION_NOW);
    const char* backend =
        (vinfo && vinfo->ssl_version) ? vinfo->ssl_version : "";
    if (std::strstr(backend, "Schannel") != nullptr ||
        std::strstr(backend, "SecureTransport") != nullptr) {
        std::printf("curl TLS backend: %s\n", backend);
        GTEST_SKIP() << "curl " << backend
                     << " 后端不支持 PEM 客户端证书";
    }
    namespace fs = std::filesystem;
    TempDir dir;
    const std::string key_path = dir.file("key.pem");
    const std::string cert_path = dir.file("cert.pem");
    const std::string client_key_path = dir.file("client_key.pem");
    const std::string client_cert_path = dir.file("client_cert.pem");
    ASSERT_TRUE(falcon_test_tls::generate_client_cert(client_key_path,
                                                      client_cert_path));

    falcon::testtls::TlsTestServer tls;
    ASSERT_TRUE(tls.start(key_path, cert_path, /*dual_stack=*/false,
                          client_cert_path));
    const std::string body = "v1-mtls-payload";
    tls.set_body(body);

    DownloadOptions options;
    options.verify_ssl = false;  // 自签服务器证书不在本用例覆盖面
    options.client_certificate = client_cert_path;
    options.client_private_key = client_key_path;

    const auto task = makeTask(301, tls.url("localhost", "/f.bin"),
                               dir.file("out.bin"), options);
    handler()->download(task, nullptr);
    ASSERT_EQ(task->status(), TaskStatus::Completed);

    // download() 顶部 HEAD 探测 + 实际 GET 各一次握手，均出示证书
    EXPECT_GE(tls.client_certs(), 1);
    EXPECT_EQ(readFile(dir.file("out.bin")), body);

    tls.stop();
}

#endif  // FALCON_ENABLE_OPENSSL
