// SFTP Handler 门禁测试
//
// libssh2 无服务端 API（既有定性：回环 mock SSH2 服务器不可行），
// 测试只到门禁量：接口元数据、URL 解析全变体（纯函数，含
// percent-decode/IPv6/错误路径）、registry 注册（强符号拉取）、
// 连接拒绝干净失败（重试耗尽语义 + 零残留）。
// 真实服务走查（密码/密钥认证 + REST 续传）由系统 sshd 集成验收
// 承担，不在自动化测试面。

#include "plugins/sftp/sftp_handler.hpp"
#include <falcon/protocol_registry.hpp>
#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

namespace {

using namespace falcon;
using namespace falcon::protocols;

class SftpHandlerTest : public ::testing::Test {
protected:
    void SetUp() override { handler_ = std::make_unique<SftpHandler>(); }

    SftpHandler* handler() { return handler_.get(); }

    static DownloadOptions withUser(const std::string& user) {
        DownloadOptions options;
        options.http_username = user;
        return options;
    }

private:
    std::unique_ptr<SftpHandler> handler_;
};

//==============================================================================
// 协议识别与元数据
//==============================================================================

TEST_F(SftpHandlerTest, ProtocolMetadata) {
    EXPECT_EQ(handler()->protocol_name(), "sftp");
    const auto schemes = handler()->supported_schemes();
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "sftp") !=
                schemes.end());
    EXPECT_EQ(handler()->priority(), 50);
    EXPECT_TRUE(handler()->supports_resume());
}

TEST_F(SftpHandlerTest, CanHandleRecognizesSftpOnly) {
    EXPECT_TRUE(handler()->can_handle("sftp://example.com/file.txt"));
    EXPECT_TRUE(handler()->can_handle("sftp://user@host:2222/dir/f.bin"));
    EXPECT_FALSE(handler()->can_handle("ssh://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("ftp://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("http://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("sftp:example.com"));
    EXPECT_FALSE(handler()->can_handle(""));
}

TEST_F(SftpHandlerTest, ProtocolRegistryLoadsSftpHandler) {
    // 引用 describe_builtin_protocols 把 builtin_protocol_handlers.cpp.o
    // 强符号拉进链接（FTP 套件的 weak stub 教训——不引用则 registry 为空）
    const auto described = falcon::describe_builtin_protocols();
    const auto it = std::find_if(
        described.begin(), described.end(),
        [](const falcon::BuiltinProtocolInfo& info) {
            return info.protocol == "sftp";
        });
    ASSERT_NE(it, described.end());
#if defined(FALCON_ENABLE_SFTP_PLUGIN)
    EXPECT_TRUE(it->compiled_in);
    EXPECT_TRUE(it->registry_integrated);
#else
    EXPECT_FALSE(it->compiled_in);
#endif

    falcon::ProtocolRegistry registry;
    registry.load_builtin_handlers();
    const auto schemes = registry.supported_schemes();
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "sftp") !=
                schemes.end());
}

//==============================================================================
// parse_sftp_url 纯函数矩阵
//==============================================================================

TEST_F(SftpHandlerTest, ParseUrlBasicDefaults) {
    const auto ep = detail::parse_sftp_url(
        "sftp://alice@example.com/data/report.pdf", withUser("ignored"));
    EXPECT_EQ(ep.host, "example.com");
    EXPECT_EQ(ep.port, 22);
    EXPECT_EQ(ep.user, "alice");
    EXPECT_EQ(ep.password.empty(), true);
    EXPECT_EQ(ep.path, "/data/report.pdf");
}

TEST_F(SftpHandlerTest, ParseUrlUserinfoAndPasswordAndPort) {
    const auto ep = detail::parse_sftp_url(
        "sftp://bob:se%40cret@example.com:2222/dir/file.txt", {});
    EXPECT_EQ(ep.host, "example.com");
    EXPECT_EQ(ep.port, 2222);
    EXPECT_EQ(ep.user, "bob");
    EXPECT_EQ(ep.password, "se@cret");
    EXPECT_EQ(ep.path, "/dir/file.txt");
}

TEST_F(SftpHandlerTest, ParseUrlPercentDecodingUserPassPathNotHost) {
    // host 不 decode（DNS 名不含百分号语义）；user/pass/path decode；
    // '+' 原样保留（SFTP 路径不是表单语义）
    const auto ep = detail::parse_sftp_url(
        "sftp://us%65r:p%40ss+w@ho%73t.com/pa%20th/f%69le.txt", {});
    EXPECT_EQ(ep.host, "ho%73t.com");
    EXPECT_EQ(ep.user, "user");
    EXPECT_EQ(ep.password, "p@ss+w");
    EXPECT_EQ(ep.path, "/pa th/file.txt");
}

TEST_F(SftpHandlerTest, ParseUrlIPv6LiteralWithPort) {
    const auto ep = detail::parse_sftp_url(
        "sftp://[::1]:2022/uploads/a.bin", {});
    EXPECT_EQ(ep.host, "::1");
    EXPECT_EQ(ep.port, 2022);
    EXPECT_EQ(ep.path, "/uploads/a.bin");
}

TEST_F(SftpHandlerTest, ParseUrlUsernameFallbackChain) {
    // URL 无 userinfo → options.http_username 兜底
    const auto ep = detail::parse_sftp_url("sftp://example.com/f.txt",
                                           withUser("optsuser"));
    EXPECT_EQ(ep.user, "optsuser");
}

TEST_F(SftpHandlerTest, ParseUrlRejectsMalformedForms) {
    // 无 host
    EXPECT_THROW(detail::parse_sftp_url("sftp:///only/path.txt", {}),
                 InvalidURLException);
    // 空 path
    EXPECT_THROW(detail::parse_sftp_url("sftp://example.com", {}),
                 InvalidURLException);
    EXPECT_THROW(detail::parse_sftp_url("sftp://example.com:22", {}),
                 InvalidURLException);
    // 非法端口
    EXPECT_THROW(detail::parse_sftp_url("sftp://example.com:abc/f", {}),
                 InvalidURLException);
    EXPECT_THROW(detail::parse_sftp_url("sftp://example.com:99999/f", {}),
                 InvalidURLException);
    EXPECT_THROW(detail::parse_sftp_url("sftp://example.com:0/f", {}),
                 InvalidURLException);
    // 畸形 IPv6
    EXPECT_THROW(detail::parse_sftp_url("sftp://[::1/path", {}),
                 InvalidURLException);
}

//==============================================================================
// 连接拒绝干净失败（真实 socket 路径，零服务端依赖）
//==============================================================================

namespace fs = std::filesystem;

class SftpTempDir {
public:
    SftpTempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("falcon_sftp_test_" +
                 std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count() %
                     1000000) +
                 std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~SftpTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    SftpTempDir(const SftpTempDir&) = delete;
    SftpTempDir& operator=(const SftpTempDir&) = delete;
    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

private:
    fs::path path_;
};

TEST_F(SftpHandlerTest, GetFileInfoConnectionRefusedFailsCleanly) {
    // 端口 1 恒拒绝：解析/连接链路真实走一遍，NetworkException 收口
    DownloadOptions options;
    options.http_username = "ci";
    EXPECT_THROW(handler()->get_file_info("sftp://127.0.0.1:1/nope.bin",
                                          options),
                 NetworkException);
}

TEST_F(SftpHandlerTest, DownloadExhaustsRetriesWithoutResidue) {
    SftpTempDir dir;
    DownloadOptions options;
    options.http_username = "ci";
    options.max_retries = 1;
    options.retry_delay_seconds = 0; // 门禁测试不等退避

    auto task = std::make_shared<DownloadTask>(
        1, "sftp://127.0.0.1:1/nope.bin", options);
    task->set_output_path(dir.file("out.bin"));

    EXPECT_THROW(handler()->download(task, nullptr), NetworkException);
    // 失败路径零产物：临时文件从未创建（连接先于文件打开）
    EXPECT_FALSE(fs::exists(dir.file("out.bin") + ".falcon.tmp"));
    EXPECT_FALSE(fs::exists(dir.file("out.bin")));
}

TEST_F(SftpHandlerTest, ParseInvalidUrlFailsBeforeAnyConnection) {
    SftpTempDir dir;
    DownloadOptions options;
    auto task = std::make_shared<DownloadTask>(2, "sftp:///no-host.bin",
                                               options);
    task->set_output_path(dir.file("out.bin"));
    // 解析在重试循环之前：InvalidURLException 直接穿透（异常层级上
    // 与 NetworkException 是兄弟类，不是派生关系）
    EXPECT_THROW(handler()->download(task, nullptr), InvalidURLException);
    EXPECT_FALSE(fs::exists(dir.file("out.bin")));
}

} // namespace
