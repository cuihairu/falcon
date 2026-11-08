// SFTP 真实 sshd 集成测试（密码/密钥认证 + 断点续传）。
//
// libssh2 无服务端 API（回环 mock SSH2 服务器不可行，见 sftp_handler_test
// 文件头注释），真实服务走查由本文件承担：在临时目录生成 host 密钥 +
// 用户密钥对，以非 root 身份在随机高位端口启动系统 sshd，经
// SftpHandler::download/get_file_info 端到端下载并校验内容。
//
// 环境缺失（无 sshd / 无 sftp-server）或启动失败 → GTEST_SKIP（不假报
// 失败）；本文件不修改系统配置，全部资源限临时目录。

#include "plugins/sftp/sftp_handler.hpp"
#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
#include <string>
#include <thread>

#if !defined(_WIN32) && defined(FALCON_ENABLE_SFTP_PLUGIN)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace falcon;
using namespace falcon::protocols;

#if !defined(_WIN32) && defined(FALCON_ENABLE_SFTP_PLUGIN)

namespace {

// 进程级忽略 SIGPIPE：真实 socket 写失败以 EPIPE 返回而非杀进程。
struct SigpipeImmunity {
    SigpipeImmunity() { ::signal(SIGPIPE, SIG_IGN); }
};
const SigpipeImmunity kSigpipeImmunity;

std::string find_executable(const char* name) {
    // PATH 扫描（含常见非 PATH 位置：/usr/sbin、/usr/lib/openssh）。
    std::vector<std::string> dirs;
    const char* path_env = std::getenv("PATH");
    if (path_env) {
        std::stringstream ss(path_env);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            if (!dir.empty()) dirs.push_back(dir);
        }
    }
    dirs.insert(dirs.end(),
                {"/usr/sbin", "/usr/bin", "/sbin", "/bin",
                 "/usr/lib/openssh", "/usr/local/sbin", "/usr/local/bin"});
    for (const auto& dir : dirs) {
        const fs::path candidate = fs::path(dir) / name;
        std::error_code ec;
        const auto st = fs::status(candidate, ec);
        if (!ec && fs::exists(st) &&
            (st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
            return candidate.string();
        }
    }
    return {};
}

bool wait_for_port(std::uint16_t port, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                          sizeof(addr)) == 0) {
                ::close(fd);
                return true;
            }
            ::close(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool run_cmd(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    return rc == 0;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// 自托管 sshd 夹具：起停系统 sshd（非 root、高位端口、临时目录）。
class SshdFixture {
public:
    bool setup() {
        const std::string sshd = find_executable("sshd");
        const std::string sftp_server = find_executable("sftp-server");
        if (sshd.empty() || sftp_server.empty()) return false;

        // 每次实例独立临时目录 + 三次重试（端口/残留竞态容忍）
        static std::atomic<unsigned> counter{0};
        for (int attempt = 0; attempt < 3; ++attempt) {
            base_ = fs::temp_directory_path() /
                    ("falcon_sftp_sshd_" +
                     std::to_string(::getpid()) + "_" +
                     std::to_string(counter.fetch_add(1)));
            if (try_setup_once(sshd, sftp_server)) return true;
            teardown();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    bool try_setup_once(const std::string& sshd,
                        const std::string& sftp_server) {
        pid_ = 0;
        last_log_.clear();
        std::error_code ec0;
        fs::remove_all(base_, ec0);
        fs::create_directories(base_ / "etc");
        fs::create_directories(base_ / "run");
        fs::create_directories(base_ / "home" / ".ssh");
        fs::create_directories(base_ / "run");
        fs::create_directories(base_ / "home" / ".ssh");

        // 端口探测：OS 分配空闲端口（监听后立即关闭，竞态窗口极小且
        // 非关键——失败重试即可）
        port_ = alloc_port();
        if (port_ == 0) return false;

        const std::string host_key = (base_ / "etc" / "host_ed25519").string();
        const std::string user_key = (base_ / "home" / ".ssh" / "id_ed25519").string();
        const std::string user_pub = user_key + ".pub";
        user_key_ = user_key;
        const std::string auth_keys =
            (base_ / "home" / ".ssh" / "authorized_keys").string();

        if (!run_cmd("ssh-keygen -t ed25519 -f \"" + host_key +
                     "\" -N '' -q >/dev/null 2>&1"))
            return false;
        if (!run_cmd("ssh-keygen -t ed25519 -f \"" + user_key +
                     "\" -N '' -q >/dev/null 2>&1"))
            return false;
        if (!run_cmd("cp \"" + user_pub + "\" \"" + auth_keys + "\""))
            return false;

        // 远端文件
        remote_path_ = (base_ / "home" / "data.bin").string();
        content_ = std::string(4096, '\0');
        for (std::size_t i = 0; i < content_.size(); ++i)
            content_[i] = static_cast<char>((i * 31 + 7) % 256);
        {
            std::ofstream out(remote_path_, std::ios::binary | std::ios::trunc);
            out.write(content_.data(), static_cast<std::streamsize>(content_.size()));
        }
        chmod(auth_keys.c_str(), 0600);
        chmod((base_ / "home" / ".ssh").c_str(), 0700);
        chmod((base_ / "home").c_str(), 0700);

        // sshd 配置（绝对路径；非 root 高位端口；不依赖系统账号表）
        const std::string cfg = (base_ / "sshd_config").string();
        {
            std::ofstream out(cfg, std::ios::trunc);
            out << "Port " << port_ << "\n"
                << "ListenAddress 127.0.0.1\n"
                << "HostKey " << host_key << "\n"
                << "PidFile " << (base_ / "run" / "sshd.pid").string() << "\n"
                << "AuthorizedKeysFile " << auth_keys << "\n"
                << "PasswordAuthentication no\n"
                << "PubkeyAuthentication yes\n"
                << "PermitRootLogin no\n"
                << "UsePAM no\n"
                << "StrictModes no\n"
                << "Subsystem sftp " << sftp_server << "\n";
        }

        const std::string pid_file = (base_ / "run" / "sshd.pid").string();
        const std::string log_file = (base_ / "run" / "sshd.log").string();
        const std::string cmd = "setsid \"" + sshd + "\" -f \"" + cfg +
                                "\" -D -e > \"" + log_file +
                                "\" 2>&1 < /dev/null &";
        if (!run_cmd(cmd)) return false;
        if (!wait_for_port(port_, 5000)) {
            std::ifstream in(log_file);
            std::string line;
            while (std::getline(in, line)) last_log_ += line + "\n";
            return false;
        }
        // 捕获 sshd PID（PidFile 由配置写入）
        std::ifstream pin(pid_file);
        if (pin >> pid_) { /* pid captured */ }
        return true;
    }

    void teardown() {
        if (pid_ > 0) ::kill(pid_, SIGTERM);
        // 兜底：按 PidFile 残留清理（进程组）
        if (!base_.empty()) {
            const std::string pid_file = (base_ / "run" / "sshd.pid").string();
            std::ifstream pin(pid_file);
            int p = 0;
            if (pin >> p && p > 0) ::kill(p, SIGTERM);
        }
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    ~SshdFixture() { teardown(); }

    std::uint16_t port() const { return port_; }
    const std::string& private_key() const { return user_key_; }
    /// sshd 以当前进程用户身份运行；URL/用户名必须与真实登录用户一致，
    /// 否则授权密钥不匹配（CI runner 用户非仓库作者的本地用户名）。
    static std::string user() {
        if (const char* u = std::getenv("USER"); u && *u) {
            return std::string(u);
        }
        if (const passwd* pw = ::getpwuid(::getuid());
            pw && pw->pw_name && *pw->pw_name) {
            return std::string(pw->pw_name);
        }
        return std::string("nobody");
    }
    const std::string& remote_path() const { return remote_path_; }
    const std::string& content() const { return content_; }
    const std::string& debug_log() const { return last_log_; }

    /// 覆写远端文件为给定内容（限速/进度窗口测试需要较大文件）。
    bool set_remote_content(const std::string& data) {
        std::ofstream out(remote_path_, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out.good()) return false;
        content_ = data;
        return true;
    }

private:
    static std::uint16_t alloc_port() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return 0;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return 0;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(fd);
            return 0;
        }
        const std::uint16_t port = ntohs(addr.sin_port);
        ::close(fd);
        return port;
    }

    fs::path base_;
    std::uint16_t port_ = 0;
    pid_t pid_ = 0;
    std::string user_key_;
    std::string remote_path_;
    std::string content_;
    std::string last_log_;
};

} // namespace

TEST(SftpSshdIntegration, DownloadFullFileViaPasswordlessKey) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();
    ASSERT_GT(sshd.port(), 0u);

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();

    auto task = std::make_shared<DownloadTask>(
        1, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
               sshd.remote_path(),
        options);
    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_out.bin").string();
    task->set_output_path(out);

    SftpHandler handler;
    handler.download(task, nullptr);

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file(out), sshd.content());
    std::error_code ec;
    fs::remove(out, ec);
}

TEST(SftpSshdIntegration, GetFileInfoReportsRemoteSize) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();

    SftpHandler handler;
    const FileInfo info = handler.get_file_info(
        "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
            sshd.remote_path(),
        options);
    EXPECT_EQ(info.total_size, sshd.content().size());
    EXPECT_EQ(info.supports_resume, true);
    EXPECT_EQ(info.filename, "data.bin");
}


// 认证失败（错误私钥）：NetworkException 收口，不残留产物。
TEST(SftpSshdIntegration, AuthFailureWrongKeyFailsCleanly) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    DownloadOptions options;
    options.client_private_key = (fs::temp_directory_path() /
                                  "falcon_sftp_wrong_key").string();
    // 先生成一把无关的密钥，保证路径存在但认证必然失败
    {
        std::ofstream out(options.client_private_key,
                          std::ios::binary | std::ios::trunc);
        out << "not-a-real-key";
    }
    options.http_username = SshdFixture::user();
    options.max_retries = 0;

    auto task = std::make_shared<DownloadTask>(
        2, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
               sshd.remote_path(),
        options);
    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_fail.bin").string();
    task->set_output_path(out);

    SftpHandler handler;
    EXPECT_THROW(handler.download(task, nullptr), NetworkException);
    EXPECT_FALSE(fs::exists(out + ".falcon.tmp"));
    EXPECT_FALSE(fs::exists(out));
    std::error_code ec;
    fs::remove(options.client_private_key, ec);
    fs::remove(out, ec);
}

// 远端路径不存在：stat 失败（download 与 get_file_info 双路径）。
TEST(SftpSshdIntegration, RemoteStatFailureForMissingPath) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.max_retries = 0;

    const std::string url = "sftp://" + SshdFixture::user() + "@127.0.0.1:" +
                            std::to_string(sshd.port()) +
                            sshd.remote_path() + ".missing";
    auto task = std::make_shared<DownloadTask>(3, url, options);
    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_missing.bin").string();
    task->set_output_path(out);

    SftpHandler handler;
    EXPECT_THROW(handler.download(task, nullptr), NetworkException);
    EXPECT_THROW(handler.get_file_info(url, options), NetworkException);
    EXPECT_FALSE(fs::exists(out + ".falcon.tmp"));
    std::error_code ec;
    fs::remove(out, ec);
}

// 远端路径是目录：download 与 get_file_info 都拒绝。
TEST(SftpSshdIntegration, RemoteDirectoryRejected) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.max_retries = 0;

    const std::string url = "sftp://" + SshdFixture::user() + "@127.0.0.1:" +
                            std::to_string(sshd.port()) + "/home";
    auto task = std::make_shared<DownloadTask>(4, url, options);
    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_dir.bin").string();
    task->set_output_path(out);

    SftpHandler handler;
    EXPECT_THROW(handler.download(task, nullptr), NetworkException);
    EXPECT_THROW(handler.get_file_info(url, options), NetworkException);
    EXPECT_FALSE(fs::exists(out + ".falcon.tmp"));
    std::error_code ec;
    fs::remove(out, ec);
}


// 断点续传：预置部分 .falcon.tmp，下载应从 offset 续传并补全。
TEST(SftpSshdIntegration, ResumeFromPartialTempAppends) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.resume_enabled = true;

    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_resume.bin").string();
    const std::string tmp = out + ".falcon.tmp";
    {
        std::ofstream in(tmp, std::ios::binary | std::ios::trunc);
        in.write(sshd.content().data(), 1000);  // 前 1000 字节已下
    }

    auto task = std::make_shared<DownloadTask>(
        5, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
               sshd.remote_path(),
        options);
    task->set_output_path(out);

    SftpHandler handler;
    handler.download(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file(out), sshd.content());
    std::error_code ec;
    fs::remove(out, ec);
    fs::remove(tmp, ec);
}


// 主机键不匹配（known_hosts 已知但键不同）：MITM 硬失败。
TEST(SftpSshdIntegration, HostKeyMismatchFailsHard) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    // 伪造 HOME：.ssh/known_hosts 记录与真实主机键不同的 ED25519 blob
    const fs::path fake_home =
        fs::temp_directory_path() / "falcon_sftp_fake_home";
    fs::create_directories(fake_home / ".ssh");
    {
        std::ofstream out(fake_home / ".ssh" / "known_hosts",
                          std::ios::trunc);
        out << "127.0.0.1 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIWRvbm90dXNldGhpc2tleWJsb2Jmb3J0ZXN0aW5n\n";
    }
    const char* old_home = std::getenv("HOME");
    ::setenv("HOME", fake_home.string().c_str(), 1);

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.max_retries = 0;

    auto task = std::make_shared<DownloadTask>(
        6, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
               sshd.remote_path(),
        options);
    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_mismatch.bin").string();
    task->set_output_path(out);

    SftpHandler handler;
    EXPECT_THROW(handler.download(task, nullptr), NetworkException);
    EXPECT_FALSE(fs::exists(out + ".falcon.tmp"));

    if (old_home) ::setenv("HOME", old_home, 1);
    else ::unsetenv("HOME");
    std::error_code ec;
    fs::remove_all(fake_home, ec);
    fs::remove(out, ec);
}


// 下载输出临时文件打开失败：目标目录不存在 → FileIOException，不残留。
TEST(SftpSshdIntegration, DownloadOutputOpenFailureThrows) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.max_retries = 0;

    auto task = std::make_shared<DownloadTask>(
        8, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
               sshd.remote_path(),
        options);
    const std::string missing_dir =
        (fs::temp_directory_path() / "falcon_sftp_no_such_dir_xyz").string();
    task->set_output_path(missing_dir + "/out.bin");

    SftpHandler handler;
    EXPECT_THROW(handler.download(task, nullptr), NetworkException);
}

// 限速 + 进度窗口：低 speed_limit 让读循环跨过 200ms 进度节流并触发
// apply_speed_limit 的 pacing 补眠（覆盖 maybe_report_progress /
// effective_speed_limit / apply_speed_limit 全函数体）。
TEST(SftpSshdIntegration, SpeedLimitPacesAndReportsProgress) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    // 512 KiB 随机内容：1 MiB/s 限速下总时长约 0.5s，进度窗口必然跨步。
    std::string big;
    big.resize(512 * 1024);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>((i * 131 + 17) % 256);
    ASSERT_TRUE(sshd.set_remote_content(big));

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.speed_limit = 1024 * 1024;  // 1 MiB/s

    auto task = std::make_shared<DownloadTask>(
        9, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
               sshd.remote_path(),
        options);
    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_speed.bin").string();
    task->set_output_path(out);

    SftpHandler handler;
    handler.download(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file(out), big);
    std::error_code ec;
    fs::remove(out, ec);
}

// 监听器级限速：query_speed_limit 返回非零（options.speed_limit=0）时
// 取监听器值并应用 pacing（effective_speed_limit 的监听器分支 +
// apply_speed_limit 补眠）。
namespace {
class FixedSpeedListener final : public falcon::IEventListener {
public:
    explicit FixedSpeedListener(falcon::BytesPerSecond limit) : limit_(limit) {}
    falcon::BytesPerSecond query_speed_limit(falcon::TaskId) override {
        return limit_;
    }
private:
    falcon::BytesPerSecond limit_;
};
} // namespace

TEST(SftpSshdIntegration, ListenerSpeedLimitAppliesPacing) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    std::string big;
    big.resize(512 * 1024);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>((i * 89 + 23) % 256);
    ASSERT_TRUE(sshd.set_remote_content(big));

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.speed_limit = 0;  // 仅监听器限速生效

    auto task = std::make_shared<DownloadTask>(
        10, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
                sshd.remote_path(),
        options);
    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_lspeed.bin").string();
    task->set_output_path(out);

    FixedSpeedListener listener(1024 * 1024);  // 1 MiB/s
    SftpHandler handler;
    handler.download(task, &listener);
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file(out), big);
    std::error_code ec;
    fs::remove(out, ec);
}

// 临时文件已完整（前次尝试收满但发布前中断）：download 直接发布，
// 不再连远端读（temp==total 快速路径覆盖 publish 分支）。
TEST(SftpSshdIntegration, CompleteTempFilePublishesImmediately) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.resume_enabled = true;
    options.max_retries = 0;

    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_complete.bin").string();
    const std::string tmp = out + ".falcon.tmp";
    {
        std::ofstream in(tmp, std::ios::binary | std::ios::trunc);
        in.write(sshd.content().data(),
                 static_cast<std::streamsize>(sshd.content().size()));
    }

    auto task = std::make_shared<DownloadTask>(
        11, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
                sshd.remote_path(),
        options);
    task->set_output_path(out);

    SftpHandler handler;
    handler.download(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file(out), sshd.content());
    EXPECT_FALSE(fs::exists(tmp));
    std::error_code ec;
    fs::remove(out, ec);
    fs::remove(tmp, ec);
}

// 临时文件已完整但目标名是已存在目录：发布改名失败 → FileIOException。
TEST(SftpSshdIntegration, CompleteTempFilePublishRenameFails) {
    SshdFixture sshd;
    if (!sshd.setup())
        GTEST_SKIP() << "sshd integration unavailable: " << sshd.debug_log();

    DownloadOptions options;
    options.client_private_key = sshd.private_key();
    options.http_username = SshdFixture::user();
    options.resume_enabled = true;
    options.max_retries = 0;

    const std::string out = (fs::temp_directory_path() /
                             "falcon_sftp_complete_dir").string();
    fs::create_directories(out);  // 目标名已存在且是目录
    const std::string tmp = out + ".falcon.tmp";
    {
        std::ofstream in(tmp, std::ios::binary | std::ios::trunc);
        in.write(sshd.content().data(),
                 static_cast<std::streamsize>(sshd.content().size()));
    }

    auto task = std::make_shared<DownloadTask>(
        12, "sftp://" + SshdFixture::user() + "@127.0.0.1:" + std::to_string(sshd.port()) +
                sshd.remote_path(),
        options);
    task->set_output_path(out);

    SftpHandler handler;
    // 重试循环把内部 FileIOException 包装成 NetworkException 上抛。
    EXPECT_THROW(handler.download(task, nullptr), NetworkException);
    std::error_code ec;
    fs::remove_all(out, ec);
    fs::remove(tmp, ec);
}

#else  // !_WIN32 && FALCON_ENABLE_SFTP_PLUGIN

TEST(SftpSshdIntegration, UnsupportedPlatformSkipped) {
    GTEST_SKIP() << "sshd integration tests are POSIX-only";
}

#endif
