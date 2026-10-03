// WebDAV Handler 门禁测试
//
// 数据面是 libcurl → 真实 HTTP，回环可全链测：接口元数据、URL 解析
// 全变体（纯函数：percent-decode 只作用 userinfo、path 保持 encoded、
// IPv6、错误路径）、registry 注册（强符号拉取）、回环 e2e（请求行/
// Authorization/Range 到达断言）、续传与 Range 撒谎防护、暂停中止、
// 4xx/连接拒绝/rename 失败收口、davs TLS 干净失败、timeout_seconds
// 停滞看门狗双向钉子（慢而健康总时长 > timeout 照常完成 / 静默服务
// 器 timeout 量级中止——B6 P0 语义，绝不映射 CURLOPT_TIMEOUT 总帽）。
// 真实服务走查（Radicale/Nextcloud 类服务器 PROPFIND 列目录）由
// stdlib 参考服务器手动验收承担，不在自动化测试面。

#include "plugins/webdav/webdav_handler.hpp"
#include <falcon/detail/injection.hpp>
#include <falcon/protocol_registry.hpp>
#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/exceptions.hpp>

#include "scripted_http_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace falcon;
using namespace falcon::protocols;
using namespace falcon::testscripts;
namespace fs = std::filesystem;

class WebdavHandlerTest : public ::testing::Test {
protected:
    void SetUp() override { handler_ = std::make_unique<WebdavHandler>(); }

    WebdavHandler* handler() { return handler_.get(); }

    static DownloadOptions withUser(const std::string& user,
                                    const std::string& pass = {}) {
        DownloadOptions options;
        options.http_username = user;
        options.http_password = pass;
        return options;
    }

private:
    std::unique_ptr<WebdavHandler> handler_;
};

class DavTempDir {
public:
    DavTempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("falcon_dav_test_" +
                 std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count() %
                     1000000) +
                 std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~DavTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    DavTempDir(const DavTempDir&) = delete;
    DavTempDir& operator=(const DavTempDir&) = delete;
    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

private:
    fs::path path_;
};

//==============================================================================
// 协议识别与元数据
//==============================================================================

TEST_F(WebdavHandlerTest, ProtocolMetadata) {
    EXPECT_EQ(handler()->protocol_name(), "webdav");
    const auto schemes = handler()->supported_schemes();
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "dav") !=
                schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "davs") !=
                schemes.end());
    EXPECT_EQ(handler()->priority(), 50);
    EXPECT_TRUE(handler()->supports_resume());
}

TEST_F(WebdavHandlerTest, CanHandleRecognizesDavSchemesOnly) {
    EXPECT_TRUE(handler()->can_handle("dav://example.com/dav/file.txt"));
    EXPECT_TRUE(handler()->can_handle(
        "davs://example.com/remote.php/webdav/a.bin"));
    EXPECT_FALSE(handler()->can_handle("http://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("ftp://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("sftp://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("dav:example.com"));
    EXPECT_FALSE(handler()->can_handle(""));
}

TEST_F(WebdavHandlerTest, ProtocolRegistryLoadsWebdavHandler) {
    // 引用 describe_builtin_protocols 把 builtin_protocol_handlers.cpp.o
    // 强符号拉进链接（FTP 套件的 weak stub 教训——不引用则 registry 为空）
    const auto described = falcon::describe_builtin_protocols();
    const auto it = std::find_if(
        described.begin(), described.end(),
        [](const falcon::BuiltinProtocolInfo& info) {
            return info.protocol == "webdav";
        });
    ASSERT_NE(it, described.end());
#if defined(FALCON_ENABLE_WEBDAV_PLUGIN)
    EXPECT_TRUE(it->compiled_in);
    EXPECT_TRUE(it->registry_integrated);
#else
    EXPECT_FALSE(it->compiled_in);
#endif

    falcon::ProtocolRegistry registry;
    registry.load_builtin_handlers();
    const auto schemes = registry.supported_schemes();
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "dav") !=
                schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "davs") !=
                schemes.end());
}

//==============================================================================
// parse_dav_url / to_http_url 纯函数矩阵
//==============================================================================

TEST_F(WebdavHandlerTest, ParseUrlBasicDefaults) {
    const auto ep = falcon::protocols::detail::parse_dav_url(
        "dav://example.com/dav/report.pdf", {});
    EXPECT_FALSE(ep.secure);
    EXPECT_EQ(ep.host, "example.com");
    EXPECT_EQ(ep.port, 0);  // scheme 缺省
    EXPECT_EQ(ep.user.empty(), true);
    EXPECT_EQ(ep.password.empty(), true);
    EXPECT_EQ(ep.path, "/dav/report.pdf");
}

TEST_F(WebdavHandlerTest, ParseUrlUserinfoAndPasswordAndPort) {
    const auto ep = falcon::protocols::detail::parse_dav_url(
        "dav://bob:se%40cret@example.com:8080/dir/file.txt", {});
    EXPECT_EQ(ep.host, "example.com");
    EXPECT_EQ(ep.port, 8080);
    EXPECT_EQ(ep.user, "bob");
    EXPECT_EQ(ep.password, "se@cret");
    EXPECT_EQ(ep.path, "/dir/file.txt");
}

TEST_F(WebdavHandlerTest, ParseUrlPathStaysPercentEncodedPlusKept) {
    // path 不 decode（curl 原样发送、服务器解码——与 SFTP decode 语义
    // 相反）；user/pass decode；'+' 原样保留（非表单语义）
    const auto ep = falcon::protocols::detail::parse_dav_url(
        "dav://us%65r:p%40ss+w@example.com/pa%20th/f%69le.txt", {});
    EXPECT_EQ(ep.user, "user");
    EXPECT_EQ(ep.password, "p@ss+w");
    EXPECT_EQ(ep.path, "/pa%20th/f%69le.txt");
}

TEST_F(WebdavHandlerTest, ParseUrlSecureAndIPv6LiteralWithPort) {
    const auto ep = falcon::protocols::detail::parse_dav_url(
        "davs://[::1]:8443/remote/a.bin", {});
    EXPECT_TRUE(ep.secure);
    EXPECT_EQ(ep.host, "::1");
    EXPECT_EQ(ep.port, 8443);
    EXPECT_EQ(ep.path, "/remote/a.bin");
}

TEST_F(WebdavHandlerTest, ParseUrlUsernameFallbackChain) {
    // URL 无 userinfo → options.http_username / http_password 双兜底
    const auto ep = falcon::protocols::detail::parse_dav_url(
        "dav://example.com/f.txt", withUser("optsuser", "optspass"));
    EXPECT_EQ(ep.user, "optsuser");
    EXPECT_EQ(ep.password, "optspass");
    // URL userinfo 优先：不从 options 覆盖
    const auto ep2 = falcon::protocols::detail::parse_dav_url(
        "dav://urluser:urlpass@example.com/f.txt",
        withUser("optsuser", "optspass"));
    EXPECT_EQ(ep2.user, "urluser");
    EXPECT_EQ(ep2.password, "urlpass");
}

TEST_F(WebdavHandlerTest, ToHttpUrlRewritesScheme) {
    EXPECT_EQ(falcon::protocols::detail::to_http_url(
                  falcon::protocols::detail::parse_dav_url("dav://example.com/dav/f.txt", {})),
              "http://example.com/dav/f.txt");
    EXPECT_EQ(falcon::protocols::detail::to_http_url(falcon::protocols::detail::parse_dav_url(
                  "davs://example.com:8443/remote.php/webdav", {})),
              "https://example.com:8443/remote.php/webdav");
    // port==0（scheme 缺省）不输出端口；IPv6 字面量保持 [..] 形态
    EXPECT_EQ(falcon::protocols::detail::to_http_url(
                  falcon::protocols::detail::parse_dav_url("dav://[::1]/dav/f.txt", {})),
              "http://[::1]/dav/f.txt");
}

TEST_F(WebdavHandlerTest, ParseUrlRejectsMalformedForms) {
    // 无 host
    EXPECT_THROW(falcon::protocols::detail::parse_dav_url("dav:///only/path.txt", {}),
                 InvalidURLException);
    // 空 path
    EXPECT_THROW(falcon::protocols::detail::parse_dav_url("dav://example.com", {}),
                 InvalidURLException);
    EXPECT_THROW(falcon::protocols::detail::parse_dav_url("dav://example.com:5000", {}),
                 InvalidURLException);
    // 非法端口
    EXPECT_THROW(falcon::protocols::detail::parse_dav_url("dav://example.com:abc/f", {}),
                 InvalidURLException);
    EXPECT_THROW(falcon::protocols::detail::parse_dav_url("dav://example.com:99999/f", {}),
                 InvalidURLException);
    EXPECT_THROW(falcon::protocols::detail::parse_dav_url("dav://example.com:0/f", {}),
                 InvalidURLException);
    // 畸形 IPv6
    EXPECT_THROW(falcon::protocols::detail::parse_dav_url("davs://[::1/remote", {}),
                 InvalidURLException);
}

//==============================================================================
// 回环 e2e（ScriptedHttpServer：请求行/头到达断言 + 下载语义）
//==============================================================================

TEST_F(WebdavHandlerTest, DownloadE2EWithBasicAuthChallenge) {
    ScriptedHttpServer server;
    server.start();
    const std::string body = "webdav body content";
    const std::string path = "/dav/e2e.bin";
    FakeResponse unauthorized;
    unauthorized.status = 401;
    unauthorized.status_text = "Unauthorized";
    unauthorized.headers = {{"WWW-Authenticate", "Basic realm=\"falcon\""}};
    unauthorized.body = "auth required";
    FakeResponse ok;
    ok.status = 200;
    ok.body = body;
    // 剧本：首个 GET 吃 401（curl 按 401 挑战换 Basic 重试），第二发 200
    server.set_script(path, {unauthorized, ok});

    DavTempDir dir;
    DownloadOptions options;
    options.http_username = "alice";
    options.http_password = "secret";
    auto task = std::make_shared<DownloadTask>(
        1, "dav://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dir.file("e2e.bin"));
    task->set_status(TaskStatus::Downloading);

    handler()->download(task, nullptr);

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    // 服务器侧铁证：Basic 凭据真实到达（curl 按 401 挑战重试的第二发）
    const auto requests = server.requests();
    ASSERT_GE(requests.size(), 2u);
    bool saw_basic = false;
    for (const auto& r : requests) {
        if (r.method == "GET" && r.path == path) {
            const auto auth = r.headers.find("authorization");
            if (auth != r.headers.end() &&
                auth->second == "Basic YWxpY2U6c2VjcmV0") {
                saw_basic = true;
            }
        }
    }
    EXPECT_TRUE(saw_basic);
    // 成品逐字节
    std::ifstream out(dir.file("e2e.bin"), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(out)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, body);
    // 临时文件已发布（rename 走完）
    EXPECT_FALSE(fs::exists(dir.file("e2e.bin") + ".falcon.tmp"));
}

TEST_F(WebdavHandlerTest, ResumeContinuesFromTmpWithRangeHeader) {
    ScriptedHttpServer server;
    server.start();
    const std::string body = "0123456789ABCDEF";
    const std::string path = "/dav/resume.bin";
    FakeResponse ok;
    ok.status = 200;
    ok.body = body;
    ok.support_range = true;  // Range 请求自动 206 + 切片
    server.set_response(path, ok);

    DavTempDir dir;
    const std::string tmp = dir.file("resume.bin") + ".falcon.tmp";
    // 预置半截断点（前 8 字节已落盘）
    {
        std::ofstream tmpf(tmp, std::ios::binary | std::ios::trunc);
        tmpf << body.substr(0, 8);
    }

    DownloadOptions options;
    options.resume_enabled = true;
    auto task = std::make_shared<DownloadTask>(
        2, "dav://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dir.file("resume.bin"));
    task->set_status(TaskStatus::Downloading);

    handler()->download(task, nullptr);

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    // 服务器侧铁证：续传请求携带 Range: bytes=8-
    const auto requests = server.requests();
    ASSERT_FALSE(requests.empty());
    bool saw_range = false;
    for (const auto& r : requests) {
        if (r.method == "GET" && r.path == path &&
            r.range == "bytes=8-") {
            saw_range = true;
        }
    }
    EXPECT_TRUE(saw_range);
    // 成品逐字节一致（断点 + 续传段拼接）
    std::ifstream out(dir.file("resume.bin"), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(out)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, body);
    EXPECT_FALSE(fs::exists(tmp));
}

TEST_F(WebdavHandlerTest, ResumeAgainstRangeLiarFailsCleanly) {
    ScriptedHttpServer server;
    server.start();
    const std::string body = "0123456789ABCDEF";
    const std::string path = "/dav/liar.bin";
    FakeResponse ok;
    ok.status = 200;
    ok.body = body;
    // support_range=false：带 Range 也回 200 全量（Range 撒谎服务器）
    server.set_response(path, ok);

    DavTempDir dir;
    const std::string tmp = dir.file("liar.bin") + ".falcon.tmp";
    {
        std::ofstream tmpf(tmp, std::ios::binary | std::ios::trunc);
        tmpf << body.substr(0, 8);
    }

    DownloadOptions options;
    options.resume_enabled = true;
    options.max_retries = 1;
    options.retry_delay_seconds = 0;
    auto task = std::make_shared<DownloadTask>(
        3, "dav://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dir.file("liar.bin"));
    task->set_status(TaskStatus::Downloading);

    // curl 自带 Range 防护：带 Range 收到 200 → CURLE_RANGE_ERROR。
    // 断点数据绝不接续 200 全量响应体（成品不可能混合新旧内容）
    EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    EXPECT_FALSE(fs::exists(dir.file("liar.bin")));
    // 残量 .falcon.tmp 保留给重试（失败收口语义与 FTP 同约定）
    EXPECT_TRUE(fs::exists(tmp));
}

TEST_F(WebdavHandlerTest, PauseDuringDownloadKeepsTmpAndReturnsSilently) {
    ScriptedHttpServer server;
    server.start();
    const std::string body(128 * 1024, 'P');
    const std::string path = "/dav/slow.bin";
    FakeResponse ok;
    ok.status = 200;
    ok.body = body;
    server.set_response(path, ok);
    // 32KB/100ms 慢发：首块写入即冲破 ofstream 8KB filebuf 落盘
    server.set_slow_body(path, 100 * 1000, 32 * 1024);

    DavTempDir dir;
    const std::string tmp = dir.file("slow.bin") + ".falcon.tmp";
    DownloadOptions options;
    auto task = std::make_shared<DownloadTask>(
        4, "dav://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dir.file("slow.bin"));
    task->set_status(TaskStatus::Downloading);

    std::thread worker([&] { handler()->download(task, nullptr); });

    // 锚：磁盘出现首批数据（慢发写入已冲刷）再暂停——确定性窗口
    bool anchor = false;
    for (int i = 0; i < 300; ++i) {
        std::error_code ec;
        const auto sz = fs::file_size(tmp, ec);
        // file_size 对不存在文件返回 uintmax_t(-1) 恒过比较——必须以
        // ec 判存在（Paused 在传输开始前置位 → worker 循环顶静默
        // return，锚成假命中）
        if (!ec && sz >= 32 * 1024) {
            anchor = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(anchor);
    task->set_status(TaskStatus::Paused);
    worker.join();

    // 静默收口：状态保持 Paused，半成品保留（断点挂点），无最终名
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_TRUE(fs::exists(tmp));
    EXPECT_FALSE(fs::exists(dir.file("slow.bin")));
}

TEST_F(WebdavHandlerTest, Download4xxFailsWithoutRename) {
    ScriptedHttpServer server;
    server.start();
    const std::string path = "/dav/missing.bin";
    FakeResponse notfound;
    notfound.status = 404;
    notfound.status_text = "Not Found";
    notfound.body = "<html>404 error page</html>";
    server.set_response(path, notfound);

    DavTempDir dir;
    DownloadOptions options;
    options.max_retries = 0;
    auto task = std::make_shared<DownloadTask>(
        5, "dav://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dir.file("missing.bin"));
    task->set_status(TaskStatus::Downloading);

    // 未开 FAILONERROR：4xx 也走 CURLE_OK——不查状态码就会把错误页
    // body 改名成成品。>=400 一律按失败收口，错误页绝不顶最终名
    EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    EXPECT_FALSE(fs::exists(dir.file("missing.bin")));
}

TEST_F(WebdavHandlerTest, GetFileInfo4xxThrows) {
    ScriptedHttpServer server;
    server.start();
    const std::string path = "/dav/info404.bin";
    FakeResponse notfound;
    notfound.status = 404;
    notfound.status_text = "Not Found";
    notfound.body = "nope";
    server.set_head_response(path, notfound);
    server.set_response(path, notfound);

    DownloadOptions options;
    EXPECT_THROW(handler()->get_file_info(
                     "dav://127.0.0.1:" + std::to_string(server.port()) +
                         path,
                     options),
                 NetworkException);
}

TEST_F(WebdavHandlerTest, RenameFailureThrowsFileIO) {
    ScriptedHttpServer server;
    server.start();
    const std::string path = "/dav/renamedest.bin";
    FakeResponse ok;
    ok.status = 200;
    ok.body = "payload";
    server.set_response(path, ok);

    DavTempDir dir;
    // 目标路径被目录占用：std::rename(file, dir) 失败 → FileIOException，
    // 绝不静默丢成品
    const std::string dest = dir.file("renamedest.bin");
    fs::create_directories(dest);
    DownloadOptions options;
    auto task = std::make_shared<DownloadTask>(
        6, "dav://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dest);
    task->set_status(TaskStatus::Downloading);

    EXPECT_THROW(handler()->download(task, nullptr), FileIOException);
}

TEST_F(WebdavHandlerTest, DownloadConnectionRefusedExhaustsRetries) {
    DavTempDir dir;
    DownloadOptions options;
    options.max_retries = 1;
    options.retry_delay_seconds = 0;  // 门禁测试不等退避

    auto task = std::make_shared<DownloadTask>(
        7, "dav://127.0.0.1:1/nope.bin", options);
    task->set_output_path(dir.file("out.bin"));

    EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    // 失败收口：无成品；残量 tmp 保留（与 FTP/Range 撒谎用例同约定），
    // 但必须为空——连接拒绝下一字节都没下到
    std::error_code ec;
    const auto tmp_size =
        fs::exists(dir.file("out.bin") + ".falcon.tmp", ec)
            ? fs::file_size(dir.file("out.bin") + ".falcon.tmp", ec)
            : 0;
    EXPECT_EQ(tmp_size, 0u);
    EXPECT_FALSE(fs::exists(dir.file("out.bin")));
}

TEST_F(WebdavHandlerTest, DavsAgainstPlainHttpServerFailsCleanly) {
    ScriptedHttpServer server;
    server.start();
    const std::string path = "/dav/plain.bin";
    FakeResponse ok;
    ok.status = 200;
    ok.body = "plain";
    server.set_response(path, ok);

    DavTempDir dir;
    DownloadOptions options;
    options.max_retries = 0;
    options.timeout_seconds = 2;
    // davs → https：对明文服务器做 TLS 握手必然失败，NetworkException
    // 收口（davs 路径真实到达 TLS 层的观测）
    auto task = std::make_shared<DownloadTask>(
        8, "davs://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dir.file("plain.bin"));
    task->set_status(TaskStatus::Downloading);

    EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    EXPECT_FALSE(fs::exists(dir.file("plain.bin")));
}

TEST_F(WebdavHandlerTest, CurlEasyInitInjectionFailsCleanly) {
    DavTempDir dir;
    DownloadOptions options;
    auto task = std::make_shared<DownloadTask>(9, "dav://127.0.0.1:1/x",
                                               options);
    task->set_output_path(dir.file("x.bin"));
    task->set_status(TaskStatus::Downloading);

    {
        ::falcon::detail::ScopedInjection guard(
            ::falcon::detail::InjectPoint::CurlEasyInit);
        EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    }
    EXPECT_FALSE(fs::exists(dir.file("x.bin")));
}

//==============================================================================
// timeout_seconds 语义（B6 P0 钉子：停滞看门狗，非总时长硬帽）
//==============================================================================

TEST_F(WebdavHandlerTest, SlowHealthyTransferSurvivesTimeoutSeconds) {
    // 慢而健康的传输（总时长 > timeout_seconds）必须照常完成——旧映射
    // （CURLOPT_TIMEOUT 总帽）下本用例必红：4096B @16B/10ms ≈ 2.56s
    // 总时长，timeout_seconds=2 会在 2s 处被硬帽杀死。LOW_SPEED 看门狗
    // 语义下 1.6KB/s 远高于 1 B/s 阈值，全程健康
    ScriptedHttpServer server;
    server.start();
    const std::string body(4096, 'S');
    const std::string path = "/dav/slowhealthy.bin";
    FakeResponse ok;
    ok.status = 200;
    ok.body = body;
    server.set_response(path, ok);
    server.set_slow_body(path, 10 * 1000, 16);  // 16B/10ms ≈ 1.6KB/s

    DavTempDir dir;
    DownloadOptions options;
    options.max_retries = 0;  // 旧映射失败即 throw，钉子双向可判
    options.timeout_seconds = 2;
    auto task = std::make_shared<DownloadTask>(
        10, "dav://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dir.file("slowhealthy.bin"));
    task->set_status(TaskStatus::Downloading);

    handler()->download(task, nullptr);

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    std::ifstream out(dir.file("slowhealthy.bin"), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(out)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, body);
}

TEST_F(WebdavHandlerTest, StalledTransferAbortsAtTimeoutSeconds) {
    // 静默服务器（延迟 5s 才发首字节）必须在 timeout_seconds 量级被
    // LOW_SPEED 看门狗中止——停滞超时语义的「会咬人」方向
    ScriptedHttpServer server;
    server.start();
    const std::string path = "/dav/stalled.bin";
    FakeResponse ok;
    ok.status = 200;
    ok.body = "stalled-body";
    ok.defer_partial_ms = 5000;  // 5s 内零字节（远超 1s 看门狗）
    server.set_response(path, ok);
    // HEAD 探测走干净应答（defer 只留给 GET——HEAD 阶段挂死只测到了
    // CURLOPT_TIMEOUT 探测帽，不是下载面看门狗）
    FakeResponse head;
    head.status = 200;
    head.body = "stalled-body";
    server.set_head_response(path, head);

    DavTempDir dir;
    DownloadOptions options;
    options.max_retries = 0;
    options.timeout_seconds = 1;
    auto task = std::make_shared<DownloadTask>(
        11, "dav://127.0.0.1:" + std::to_string(server.port()) + path,
        options);
    task->set_output_path(dir.file("stalled.bin"));
    task->set_status(TaskStatus::Downloading);

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    // 看门狗 ~1s 生效：绝不允许等到 defer 的 5s（旧语义/无看门狗都会
    // 拖到 5s 服务器侧才收口）；上限放宽防 CI 调度抖动误报
    EXPECT_GE(elapsed.count(), 900);
    EXPECT_LE(elapsed.count(), 4500);
    EXPECT_FALSE(fs::exists(dir.file("stalled.bin")));
}

} // namespace
