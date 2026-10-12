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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
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

//==============================================================================
// detail 纯函数工具（单元直达；原匿名命名空间，提升到 detail 供测试链接）
//==============================================================================

TEST(SftpDetailUtil, PercentDecodeValidAndLiteralFallback) {
    EXPECT_EQ(detail::percent_decode("%41%42"), "AB");
    EXPECT_EQ(detail::percent_decode("%3a%2F"), ":/");
    // NUL 字节也是合法转义目标
    EXPECT_EQ(detail::percent_decode("%00"), std::string(1, '\0'));
    // '+' 原样保留（非表单语义）
    EXPECT_EQ(detail::percent_decode("a+b%2Bc"), "a+b+c");
    // 截断尾部 %4（i+2 越界）原样输出
    EXPECT_EQ(detail::percent_decode("a%2"), "a%2");
    EXPECT_EQ(detail::percent_decode("%"), "%");
    // 非法 hex 字符原样输出
    EXPECT_EQ(detail::percent_decode("%G1xyz"), "%G1xyz");
    EXPECT_EQ(detail::percent_decode("%2g"), "%2g");
    EXPECT_EQ(detail::percent_decode("plain"), "plain");
}

TEST(SftpDetailUtil, Base64Rfc4648Vectors) {
    // RFC 4648 §10 测试向量（期望值独立于被测实现生成）
    const unsigned char f[] = {'f'};
    const unsigned char fo[] = {'f', 'o'};
    const unsigned char foo[] = {'f', 'o', 'o'};
    const unsigned char foob[] = {'f', 'o', 'o', 'b'};
    const unsigned char fooba[] = {'f', 'o', 'o', 'b', 'a'};
    const unsigned char foobar[] = {'f', 'o', 'o', 'b', 'a', 'r'};
    const unsigned char bin[] = {0x14, 0xfb, 0x9c, 0x03, 0xd9, 0x7e};
    unsigned char longblob[32];
    for (int i = 0; i < 32; ++i) longblob[i] = static_cast<unsigned char>(i);

    EXPECT_EQ(detail::base64_encode(nullptr, 0), "");
    EXPECT_EQ(detail::base64_encode(f, 1), "Zg==");
    EXPECT_EQ(detail::base64_encode(fo, 2), "Zm8=");
    EXPECT_EQ(detail::base64_encode(foo, 3), "Zm9v");
    EXPECT_EQ(detail::base64_encode(foob, 4), "Zm9vYg==");
    EXPECT_EQ(detail::base64_encode(fooba, 5), "Zm9vYmE=");
    EXPECT_EQ(detail::base64_encode(foobar, 6), "Zm9vYmFy");
    EXPECT_EQ(detail::base64_encode(bin, sizeof(bin)), "FPucA9l+");
    EXPECT_EQ(detail::base64_encode(longblob, 32),
              "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=");
}

TEST(SftpDetailUtil, RemoteBasenameAndTrim) {
    EXPECT_EQ(detail::remote_basename("/data/report.pdf"), "report.pdf");
    EXPECT_EQ(detail::remote_basename("/a/b/"), "");
    EXPECT_EQ(detail::remote_basename("noslash.txt"), "noslash.txt");
    EXPECT_EQ(detail::trim_copy("  x  \t"), "x");
    EXPECT_EQ(detail::trim_copy("inner  text"), "inner  text");
    EXPECT_EQ(detail::trim_copy("   "), "");
    EXPECT_EQ(detail::trim_copy(""), "");
}

TEST(SftpDetailUtil, ParsePortMatrix) {
    EXPECT_EQ(detail::parse_port("", "u"), 22);
    EXPECT_EQ(detail::parse_port("2222", "u"), 2222);
    EXPECT_EQ(detail::parse_port("65535", "u"), 65535);
    // 非数字 / 部分数字 / 越界 / 溢出全部拒绝
    EXPECT_THROW(detail::parse_port("abc", "u"), InvalidURLException);
    EXPECT_THROW(detail::parse_port("12x", "u"), InvalidURLException);
    EXPECT_THROW(detail::parse_port("-1", "u"), InvalidURLException);
    EXPECT_THROW(detail::parse_port("0", "u"), InvalidURLException);
    EXPECT_THROW(detail::parse_port("65536", "u"), InvalidURLException);
    EXPECT_THROW(detail::parse_port("99999999999999999999", "u"),
                 InvalidURLException);
}

TEST(SftpDetailUtil, HostEntryCoversMatrix) {
    // 明文精确匹配；裸 host 对任意端口生效（OpenSSH 检索顺序语义）
    EXPECT_TRUE(detail::host_entry_covers("example.com", "example.com", 22));
    EXPECT_TRUE(detail::host_entry_covers("example.com", "example.com", 2222));
    EXPECT_TRUE(detail::host_entry_covers("a.com,b.com", "b.com", 22));
    // 条目逐字比对不做条目内 trim——known_hosts 以空白分隔字段，
    // hosts 字段内含空格属非法语法，" b.com" 不命中
    EXPECT_FALSE(detail::host_entry_covers("a.com, b.com", "b.com", 22));
    // 非 22 端口的 [host]:port 形态；port==22 时该形态不参与
    EXPECT_TRUE(
        detail::host_entry_covers("[example.com]:2222", "example.com", 2222));
    EXPECT_FALSE(detail::host_entry_covers("[example.com]:22", "example.com", 22));
    EXPECT_FALSE(detail::host_entry_covers("[example.com]:2222", "example.com", 22));
    // 不覆盖 / 通配符不支持 / 空表
    EXPECT_FALSE(detail::host_entry_covers("example.com", "other.com", 22));
    EXPECT_FALSE(detail::host_entry_covers("*.example.com", "a.example.com", 22));
    EXPECT_FALSE(detail::host_entry_covers("", "example.com", 22));
}

TEST(SftpDetailUtil, ExpandHomePathFollowsHomeEnv) {
    // 不改环境变量：期望值由 detail::home_dir() 推导
    //（HOME/USERPROFILE 缺失时 "~/" 原样返回）
    const std::string home = detail::home_dir();
    EXPECT_EQ(detail::expand_home_path("~/x/y"),
              home.empty() ? std::string("~/x/y") : home + "/x/y");
    // 非 "~/" 前缀原样返回（含 "~user" 与裸 "~"）
    EXPECT_EQ(detail::expand_home_path("/abs/path"), "/abs/path");
    EXPECT_EQ(detail::expand_home_path("rel"), "rel");
    EXPECT_EQ(detail::expand_home_path("~user/x"), "~user/x");
    EXPECT_EQ(detail::expand_home_path("~"), "~");
}

// ---- scan_known_hosts：known_hosts 扫描裁决矩阵（安全关键路径）----

namespace {

constexpr const char* kKeyType = "ssh-ed25519";
constexpr const char* kB64 = "AAAAed25519keyblob";

} // namespace

TEST(SftpDetailUtil, ScanKnownHostsMatch) {
    std::istringstream in(
        std::string("example.com ssh-ed25519 ") + kB64 + "\n");
    EXPECT_EQ(detail::scan_known_hosts(in, kKeyType, kB64, "example.com", 22),
              detail::KnownHostsVerdict::kMatch);
}

TEST(SftpDetailUtil, ScanKnownHostsMismatchOnBlobAndKeytype) {
    // 覆盖条目 blob 不同 → mismatch（MITM 嫌疑）
    std::istringstream blob_diff(
        std::string("example.com ssh-ed25519 AAAAdifferentblob\n"));
    EXPECT_EQ(detail::scan_known_hosts(blob_diff, kKeyType, kB64,
                                       "example.com", 22),
              detail::KnownHostsVerdict::kMismatch);
    // 覆盖条目键类型不同 → mismatch
    std::istringstream type_diff(
        std::string("example.com ssh-rsa ") + kB64 + "\n");
    EXPECT_EQ(detail::scan_known_hosts(type_diff, kKeyType, kB64,
                                       "example.com", 22),
              detail::KnownHostsVerdict::kMismatch);
}

TEST(SftpDetailUtil, ScanKnownHostsUnknownAndSkips) {
    // 他机条目不构成覆盖 → unknown
    std::istringstream other("other.com ssh-ed25519 AAAAxyz\n");
    EXPECT_EQ(detail::scan_known_hosts(other, kKeyType, kB64, "example.com", 22),
              detail::KnownHostsVerdict::kUnknown);
    // 注释/空行/缺 blob 行全部跳过，不构成 mismatch 证据
    std::istringstream junk(
        "# comment\n"
        "\n"
        "   \n"
        "example.com ssh-ed25519\n" // 两个 token，无 blob
        "unrelated ssh-rsa\n");
    EXPECT_EQ(detail::scan_known_hosts(junk, kKeyType, kB64, "example.com", 22),
              detail::KnownHostsVerdict::kUnknown);
}

TEST(SftpDetailUtil, ScanKnownHostsHashedEntriesSkippedNotMismatch) {
    // |1| 哈希条目跳过（跳过不构成 mismatch 证据），后续明文命中
    std::istringstream hashed_then_match(
        std::string("|1|saltish|hashed= ssh-ed25519 AAAAother\n") +
        "example.com ssh-ed25519 " + kB64 + "\n");
    EXPECT_EQ(detail::scan_known_hosts(hashed_then_match, kKeyType, kB64,
                                       "example.com", 22),
              detail::KnownHostsVerdict::kMatch);
    // 只有哈希条目 → unknown
    std::istringstream hashed_only("|1|saltish|hashed= ssh-ed25519 AAAAother\n");
    EXPECT_EQ(detail::scan_known_hosts(hashed_only, kKeyType, kB64,
                                       "example.com", 22),
              detail::KnownHostsVerdict::kUnknown);
}

TEST(SftpDetailUtil, ScanKnownHostsCommaListAndBracketedPortForms) {
    // hosts 字段逗号分隔列表；行首空白 trim
    std::istringstream comma(
        std::string("  other.com,example.com ssh-ed25519 ") + kB64 + "\n");
    EXPECT_EQ(detail::scan_known_hosts(comma, kKeyType, kB64, "example.com", 22),
              detail::KnownHostsVerdict::kMatch);
    // 非默认端口：[host]:port 形态与裸 host 形态都覆盖
    std::istringstream bracketed(
        std::string("[example.com]:2222 ssh-ed25519 ") + kB64 + "\n");
    EXPECT_EQ(detail::scan_known_hosts(bracketed, kKeyType, kB64,
                                       "example.com", 2222),
              detail::KnownHostsVerdict::kMatch);
    std::istringstream bare(
        std::string("example.com ssh-ed25519 ") + kB64 + "\n");
    EXPECT_EQ(detail::scan_known_hosts(bare, kKeyType, kB64, "example.com", 2222),
              detail::KnownHostsVerdict::kMatch);
}

TEST(SftpDetailUtil, ScanKnownHostsFirstMatchWinsOverEarlierMismatch) {
    // 前一覆盖条目不匹配、后一匹配 → kMatch（早退优先于 mismatch 记账）
    std::istringstream mixed(
        std::string("example.com ssh-ed25519 AAAAold\n") +
        "example.com ssh-ed25519 " + kB64 + "\n");
    EXPECT_EQ(detail::scan_known_hosts(mixed, kKeyType, kB64, "example.com", 22),
              detail::KnownHostsVerdict::kMatch);
}

} // namespace
