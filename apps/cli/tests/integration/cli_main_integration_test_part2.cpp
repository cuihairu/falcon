/**
 * @file cli_main_integration_test_part2.cpp
 * @brief Integration tests that exercise the real falcon-cli binary (POSIX only)
 *
 * Runs the actual executable via fork()+execve so that the --coverage
 * instrumentation of main.cpp is taken into account. The child processes are
 * never terminated with SIGKILL; they either exit on their own or receive
 * SIGTERM (which the CLI handles gracefully) so gcda data is still flushed.
 */

#if !defined(_WIN32)

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef FALCON_CLI_BIN
#error "FALCON_CLI_BIN must be provided by tests/CMakeLists.txt"
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kDefaultTimeoutSeconds = 30;

// A URL that is refused immediately by the kernel (nothing listens on port 1).
const char* const kRefusedUrlA = "http://127.0.0.1:1/falcon-it-a.bin";
const char* const kRefusedUrlB = "http://127.0.0.1:1/falcon-it-b.bin";

struct CliRunResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string out;
    std::string err;
};


TEST_F(CliMainIntegrationTest, RichConfigMergedIntoSuccessfulDownload) {
    const std::string payload = make_payload(128 * 1024, 0x77);
    SlowFileServer server(payload, 30);
    ASSERT_TRUE(server.start());

    // 全字段非默认配置：走 merge_config_with_file 的「CLI 为默认值才采纳」
    // 各分支；CLI 侧 -H/-o/-p 覆盖/补充 config 之外的字段。
    const auto cfg = work_dir_ / "rich-config.json";
    {
        std::ofstream of(cfg);
        of << "{\n"
           << "  \"max_connections\": 2,\n"
           << "  \"timeout_seconds\": 15,\n"
           << "  \"max_retries\": 1,\n"
           << "  \"default_download_dir\": \"" << out_dir_.string() << "\",\n"
           << "  \"speed_limit\": 4194304,\n"
           << "  \"user_agent\": \"FalconIt/1.0\",\n"
           << "  \"referer\": \"http://ref.example/\",\n"
           << "  \"cookie_file\": \"" << (work_dir_ / "cookies.txt").string()
           << "\",\n"
           << "  \"http_username\": \"ituser\",\n"
           << "  \"http_password\": \"itpass\",\n"
           << "  \"client_cert\": \"" << (work_dir_ / "no-such-cert.pem").string()
           << "\",\n"
           << "  \"client_key\": \"" << (work_dir_ / "no-such-key.pem").string()
           << "\",\n"
           << "  \"rpc_secret\": \"it-secret\",\n"
           << "  \"rpc_listen_port\": 6801,\n"
           << "  \"verbose\": true,\n"
           << "  \"resume_enabled\": false,\n"
           << "  \"auto_renaming\": true,\n"
           << "  \"conditional_download\": true,\n"
           << "  \"file_allocation\": \"trunc\",\n"
           << "  \"seed_ratio\": 2.5,\n"
           << "  \"seed_time_minutes\": 30,\n"
           << "  \"verify_ssl\": false,\n"
           << "  \"headers\": { \"X-From-Config\": \"yes\" }\n"
           << "}\n";
    }

    // 不带 -d/-t/-r：让 default_download_dir/timeout/max_retries 从 config
    // 采纳（CLI 显式值会抢占这些分支）。
    const std::vector<std::string> args{
        "--retry-wait", "0",
        "-C", cfg.string(),
        "-o", "named.bin",
        "-p", "high",
        "-H", "X-From-Cli: hi",
        server.url("/cfg-source.bin")};

    auto r = run_cli(args);
    server.stop();

    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0) << r.out << r.err;
    EXPECT_TRUE(contains(r.out, "OK [Task"));
    EXPECT_TRUE(file_bytes_equals(out_dir_ / "named.bin", payload));

    // 配置项真实到达服务器：UA/referer/自定义头（config + CLI 各一）
    const auto reqs = server.requests();
    ASSERT_FALSE(reqs.empty());
    bool saw_get = false;
    for (const auto& req : reqs) {
        if (req.rfind("GET", 0) == 0) {
            saw_get = true;
            EXPECT_NE(req.find("User-Agent: FalconIt/1.0"), std::string::npos)
                << req;
            EXPECT_NE(req.find("Referer: http://ref.example/"), std::string::npos)
                << req;
            EXPECT_NE(req.find("X-From-Config: yes"), std::string::npos) << req;
            EXPECT_NE(req.find("X-From-Cli: hi"), std::string::npos) << req;
        }
    }
    EXPECT_TRUE(saw_get);
}

TEST_F(CliMainIntegrationTest, ShowConfigPathMarksExistingHomeConfig) {
    const auto cfg_dir = home_dir_ / ".config" / "falcon";
    std::filesystem::create_directories(cfg_dir);
    {
        std::ofstream of(cfg_dir / "config.json");
        of << "{}\n";
    }

    auto r = run_cli({"--show-config-path"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, ".config/falcon/config.json"));
    EXPECT_TRUE(contains(r.out, "(exists)"));
}

TEST_F(CliMainIntegrationTest, InteractivePauseAndResumeKeysReport) {
    // 停滞监听器让任务停在 Downloading，pty 键 'p'/'r' 分别触发
    // interactive_control_loop 的暂停/恢复分支。
    uint16_t port = 0;
    const int listener = open_stall_listener(port);
    ASSERT_GE(listener, 0);

    const std::vector<std::string> args{
        "-d", out_dir_.string(),
        "-r", "0",
        "-t", "3",
        "http://127.0.0.1:" + std::to_string(port) + "/stall.bin"};

    auto r = run_cli_interactive(args, "pr", 800);
    ::close(listener);

    EXPECT_FALSE(r.timed_out);
    EXPECT_TRUE(contains(r.out, "[p] Pause"));
    EXPECT_TRUE(contains(r.out, "Paused all active tasks"));
    EXPECT_TRUE(contains(r.out, "Resumed all paused tasks"));
    // 3s 超时后任务失败收口（pty runner 三路流汇入同一从端，FAIL 摘要在 out）
    EXPECT_TRUE(contains(r.out, "FAIL"));
    EXPECT_EQ(r.exit_code, 1);
}

// ============================================================================
// Signal handling / cancellation
// ============================================================================

TEST_F(CliMainIntegrationTest, SigintCancelsLongRunningDownload) {
    // 连接拒绝会立即失败（不重试），无法覆盖取消路径；
    // 停滞监听器接受连接但永不响应，让下载保持进行中。
    uint16_t port = 0;
    const int listener = open_stall_listener(port);
    ASSERT_GE(listener, 0);

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/stall.bin";
    const std::vector<std::string> args{
        "-d", out_dir_.string(), "-r", "0", "-t", "5", url};

    auto r = run_cli(args, kDefaultTimeoutSeconds, "", 700, SIGINT);
    ::close(listener);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "Download cancelled"));
}

// ============================================================================
// Interactive (pty) paths
// ============================================================================

TEST_F(CliMainIntegrationTest, InteractiveLoopExitsWhenTasksFinish) {
    auto r = run_cli_interactive(fast_fail_args(out_dir_, kRefusedUrlA),
                                 "" /* no keys */, 0);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.out, "[p] Pause"));
    EXPECT_TRUE(contains(r.out, "FAIL"));
}

TEST_F(CliMainIntegrationTest, InteractiveQuitKeyCancelsDownload) {
    // 同上：使用停滞监听器保证下载在进行中，按下 q 键触发取消
    uint16_t port = 0;
    const int listener = open_stall_listener(port);
    ASSERT_GE(listener, 0);

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/stall.bin";
    const std::vector<std::string> args{
        "-d", out_dir_.string(), "-r", "0", "-t", "5", url};

    auto r = run_cli_interactive(args, "q\n", 800);
    ::close(listener);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.out, "Download cancelled"));
}


// 配置文件合并全字段：proxy/p2sp/swarm 从 config 注入 args（CLI 非空
// 覆盖，空 = 保留配置值）。下载经代理失败——仅需证明解析路径执行，
// 不关心下载结果。
TEST_F(CliMainIntegrationTest, ConfigMergesProxyAndSwarmFields) {
    const auto cfg = work_dir_ / "proxy-swarm-config.json";
    {
        std::ofstream of(cfg);
        of << "{\n";
        of << "  \"proxy\": \"http://127.0.0.1:9\",\n";
        of << "  \"proxy_username\": \"pu\",\n";
        of << "  \"proxy_password\": \"pp\",\n";
        of << "  \"p2sp_share\": \"true\",\n";
        of << "  \"swarm_server\": \"http://127.0.0.1:9\",\n";
        of << "  \"p2sp_advertise\": \"127.0.0.1:6800\",\n";
        of << "  \"swarm_fingerprint\": \"deadbeef\"\n";
        of << "}\n";
    }

    auto args = fast_fail_args(out_dir_, kRefusedUrlA);
    args.push_back("-C");
    args.push_back(cfg.string());
    auto r = run_cli(args);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_FALSE(r.timed_out);
    EXPECT_TRUE(contains(r.err, "FAIL"));
}

// swarm_fingerprint 非空 + swarm 支持缺失 → 忽略警告（不阻断下载）。
TEST_F(CliMainIntegrationTest, SwarmFingerprintIgnoredWarningWhenUnsupported) {
    const auto cfg = work_dir_ / "fingerprint-config.json";
    {
        std::ofstream of(cfg);
        of << "{\n";
        of << "  \"swarm_fingerprint\": \"abcdef\"\n";
        of << "}\n";
    }

    auto args = fast_fail_args(out_dir_, kRefusedUrlA);
    args.push_back("-C");
    args.push_back(cfg.string());
    auto r = run_cli(args);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_FALSE(r.timed_out);
    EXPECT_TRUE(contains(r.err, "FAIL"));
}
} // namespace

#endif // !defined(_WIN32)
