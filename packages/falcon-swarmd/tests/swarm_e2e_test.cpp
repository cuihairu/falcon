// ============================================================================
// falcon-swarmd 真二进制 e2e（SwarmBinaryE2E，POSIX-only）
//
// fork + execv 真实二进制（FALCON_SWARMD_BIN 编译定义），覆盖：
//   - 全流程：CLI 参数启动 → WS 订阅收 onPeerJoined（含 client node_id）
//     → SwarmClient 注册/心跳/空表查询 → SIGTERM 信号停机 exit 0
//     （gcda 铁律：子进程恒 SIGTERM 优雅收尾，SIGKILL 仅兜底）
//   - 心跳超时摘除端到端：client detach()（停心跳保持注册）→
//     Rendezvous sweep 摘除 → 订阅侧收 onPeerLeft
//   - 配置面：坏 JSON 配置文件非零退出（daemonize 前报错）+ --help 零退出
//   - CLI 参数边界（无效数值/未知参数 → exit 1 + 精确 stderr）、配置文件
//     缺失/未知键告警、前台全参数成功路径（无 token 告警 + 监听行 +
//     未鉴权 query -32003）、端口占用失败收口、守护化生命周期
//     （pid 文件/SIGTERM 排水/log 落盘）
//
// 覆盖率定性登记（main.cpp）：
//   - L262-265 daemonize 失败分支结构性不可达——Daemonize* 注入点是进程内
//     注入（libfalcon-core injection.hpp），fork+execv 的真二进制无法置位；
//     自然构造 fork/setsid 失败（进程数 RLIMIT/会话首进程）在测试进程内
//     不可行且属系统级扰动，按「不可达分支如实登记」跳过
//   - daemon 模式的 main.cpp 早期行（预载/参数解析/-d）经孙进程 flush 覆盖
//     （fork 复制计数器，孙进程正常 return 0 落盘）——daemonize() 内部的
//     _exit 路径属 daemon.cpp（daemon 包，另有登记）
// ============================================================================

#include "client/swarm_client.hpp"
#include "swarm_rdv_harness.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#error "This test is POSIX-only (guarded by CMake NOT WIN32)"
#endif

using namespace falcon::swarm;
using falcon::swarm::test::SwarmWsClient;
using falcon::swarm::test::wait_until;

namespace {

// ---- 子进程管理（main_integration_test 骨架的精简形）---------------------

struct Proc {
    pid_t pid = -1;
    int out_fd = -1;
    int err_fd = -1;
    bool exited = false;
    int exit_status = 0;

    std::string out;
    std::string err;
};

bool proc_start(Proc& p, const std::vector<std::string>& args) {
    int out_pipe[2];
    int err_pipe[2];
    if (::pipe(out_pipe) != 0) return false;
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
            ::close(fd);
        }
        return false;
    }

    if (pid == 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        if (out_pipe[1] > STDERR_FILENO) ::close(out_pipe[1]);
        if (err_pipe[1] > STDERR_FILENO) ::close(err_pipe[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(FALCON_SWARMD_BIN));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(FALCON_SWARMD_BIN, argv.data());
        ::_exit(127);  // exec failed
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    p.pid = pid;
    p.out_fd = out_pipe[0];
    p.err_fd = err_pipe[0];
    ::fcntl(p.out_fd, F_SETFL, ::fcntl(p.out_fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(p.err_fd, F_SETFL, ::fcntl(p.err_fd, F_GETFL) | O_NONBLOCK);
    return true;
}

void drain_fd(int fd, std::string& into, bool& eof) {
    char buf[1024];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            into.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) eof = true;
        break;
    }
}

void poll_drain(Proc& p, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    bool out_eof = false;
    bool err_eof = false;
    while (!out_eof || !err_eof) {
        int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count());
        if (remain <= 0) return;
        pollfd fds[2];
        int nfds = 0;
        int out_idx = -1;
        int err_idx = -1;
        if (!out_eof) {
            fds[nfds].fd = p.out_fd;
            fds[nfds].events = POLLIN;
            out_idx = nfds;
            ++nfds;
        }
        if (!err_eof) {
            fds[nfds].fd = p.err_fd;
            fds[nfds].events = POLLIN;
            err_idx = nfds;
            ++nfds;
        }
        const int r = ::poll(fds, static_cast<nfds_t>(nfds), remain);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return;
        }
        if (out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP)) != 0) {
            drain_fd(p.out_fd, p.out, out_eof);
        }
        if (err_idx >= 0 && (fds[err_idx].revents & (POLLIN | POLLHUP)) != 0) {
            drain_fd(p.err_fd, p.err, err_eof);
        }
    }
}

bool proc_wait_exit(Proc& p, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t r = ::waitpid(p.pid, &status, WNOHANG);
        if (r == p.pid) {
            p.exited = true;
            p.exit_status = status;
            poll_drain(p, 300);
            return true;
        }
        poll_drain(p, 50);
    }
    return false;
}

// SIGTERM 优雅收尾为主（gcda 落盘），SIGKILL 仅兜底防挂死
void proc_terminate(Proc& p) {
    if (!p.exited && p.pid > 0) {
        ::kill(p.pid, SIGTERM);
        if (!proc_wait_exit(p, 10000)) {
            ::kill(p.pid, SIGKILL);
            int status = 0;
            ::waitpid(p.pid, &status, 0);
            p.exited = true;
            p.exit_status = status;
        }
    }
    if (p.out_fd >= 0) ::close(p.out_fd);
    if (p.err_fd >= 0) ::close(p.err_fd);
}

int exit_code_of(const Proc& p) {
    if (!p.exited || !WIFEXITED(p.exit_status)) return -1;
    return WEXITSTATUS(p.exit_status);
}

// ---- 端口与身份辅助 -------------------------------------------------------

std::uint16_t pick_free_port() {
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(listener, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr),
                     sizeof(addr)), 0);
    EXPECT_EQ(::listen(listener, 1), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr),
                            &len), 0);
    const std::uint16_t port = ntohs(addr.sin_port);
    ::close(listener);
    return port;
}

// 内存临时身份（e2e 不落盘密钥——key store 往返已由单元测试覆盖）
SwarmKeyMaterial memory_key() {
    SwarmKeyMaterial m;
    auto kp = SwarmCrypto::generate_keypair();
    m.private_seed = std::move(kp.private_seed);
    m.public_key_der = std::move(kp.public_key_der);
    m.node_id = SwarmCrypto::fingerprint(m.public_key_der);
    m.pubkey_hex = SwarmCrypto::bytes_to_hex(m.public_key_der.data(),
                                             m.public_key_der.size());
    return m;
}

bool tcp_connect(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr));
    ::close(fd);
    return rc == 0;
}

SwarmClientConfig make_client_config(std::uint16_t port,
                                     const std::string& token) {
    SwarmClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<int>(port);
    cfg.server_token = token;
    cfg.rpc_timeout_ms = std::chrono::milliseconds(2000);
    cfg.reconnect_delay = std::chrono::milliseconds(100);
    return cfg;
}

// 从订阅流中等待指定 method 的通知帧，返回其 params（object）
nlohmann::json wait_notification(SwarmWsClient& ws, const std::string& method,
                                 int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count());
        auto frame = ws.read_frame(std::max(remain, 1));
        if (!frame || frame->opcode != 0x1) continue;
        try {
            const auto j = nlohmann::json::parse(frame->payload);
            if (j.value("method", std::string()) == method) {
                return j.value("params", nlohmann::json::object());
            }
        } catch (const std::exception&) {
            // 非 JSON 帧（异常为空设计内）——继续等下一帧
        }
    }
    return {};
}

// mkstemp 落一个内容文件（配置面用例共用；返回路径，失败返回空串）
std::string write_temp_file(const char* tag, const std::string& content) {
    std::string tmpl = std::string("/tmp/") + tag + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    EXPECT_GE(fd, 0);
    if (fd < 0) return {};
    const ssize_t n = ::write(fd, content.data(), content.size());
    EXPECT_EQ(static_cast<std::size_t>(n), content.size());
    ::close(fd);
    return std::string(buf.data());
}

std::string read_file_if_exists(const std::string& path) {
    std::string content;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return content;
    char buf[4096];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        content.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return content;
}

// 短命失败运行：exit 1 + stderr 含期望片段（CLI 参数边界用例共用）
void expect_fail_run(const std::vector<std::string>& args,
                     const std::string& expect_err) {
    Proc p;
    ASSERT_TRUE(proc_start(p, args));
    const bool exited = proc_wait_exit(p, 10000);
    proc_terminate(p);  // 已退出为 no-op
    ASSERT_TRUE(exited) << "should exit promptly, stderr: " << p.err;
    EXPECT_EQ(exit_code_of(p), 1) << "stderr: " << p.err;
    EXPECT_NE(p.err.find(expect_err), std::string::npos)
        << "stderr: " << p.err;
}

}  // namespace

// ===========================================================================
// 全流程：二进制 + client 库 × 同一 Rendezvous
// ===========================================================================

TEST(SwarmBinaryE2E, BinaryFullFlow) {
    const std::uint16_t port = pick_free_port();
    const std::string token = "e2e-token";

    Proc p;
    ASSERT_TRUE(proc_start(p, {
        "--no-conf",
        "--swarm-host", "127.0.0.1",
        "--swarm-port", std::to_string(port),
        "--server-token", token,
        "--heartbeat-interval-s", "1",
    }));

    // 服务器就绪（TCP 可连）后再订阅/注册
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 10000));

    // 订阅侧：先于 client 注册就位，onPeerJoined 必达
    SwarmWsClient ws;
    bool upgrade_ok = false;
    ASSERT_TRUE(ws.connect(port, token, &upgrade_ok)) << "WS upgrade failed";

    SwarmClient client(make_client_config(port, token), memory_key());
    std::string error;
    ASSERT_TRUE(client.start(&error)) << error;
    EXPECT_FALSE(client.session().empty());

    // WS 通知到达：onPeerJoined（object params，含 client node_id）
    const nlohmann::json joined =
        wait_notification(ws, "falcon.swarm.onPeerJoined", 8000);
    ASSERT_FALSE(joined.is_null()) << "no onPeerJoined within 8s";
    EXPECT_EQ(joined.value("node_id", std::string()), client.node_id());

    // 空表查询往返（用户裁决：阶段 0 资源表恒空，协议形态正确即可）
    nlohmann::json result;
    const SwarmError err = client.query(std::string(64, 'b'), &result);
    ASSERT_TRUE(err.ok()) << err.message;
    EXPECT_EQ(result.value("sha256", std::string()), std::string(64, 'b'));
    ASSERT_TRUE(result.contains("sources"));
    EXPECT_TRUE(result["sources"].is_array());
    EXPECT_EQ(result["sources"].size(), 0u);

    // SIGTERM 信号停机：干净退出（gcda 铁律），订阅侧收 CLOSE
    client.stop();
    proc_terminate(p);
    EXPECT_EQ(exit_code_of(p), 0) << "stderr: " << p.err;
}

TEST(SwarmBinaryE2E, BinaryHeartbeatTimeoutE2E) {
    const std::uint16_t port = pick_free_port();
    const std::string token = "e2e-timeout";

    Proc p;
    ASSERT_TRUE(proc_start(p, {
        "--no-conf",
        "--swarm-host", "127.0.0.1",
        "--swarm-port", std::to_string(port),
        "--server-token", token,
        "--heartbeat-timeout-s", "1",
        "--sweep-interval-ms", "50",
    }));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 10000));

    SwarmWsClient ws;
    bool upgrade_ok = false;
    ASSERT_TRUE(ws.connect(port, token, &upgrade_ok)) << "WS upgrade failed";

    SwarmClient client(make_client_config(port, token), memory_key());
    std::string error;
    ASSERT_TRUE(client.start(&error)) << error;

    // detach = 模拟崩溃：停心跳、保持注册 → Rendezvous 心跳超时摘除
    client.detach();

    const nlohmann::json left =
        wait_notification(ws, "falcon.swarm.onPeerLeft", 10000);
    ASSERT_FALSE(left.is_null()) << "no onPeerLeft within 10s";
    EXPECT_EQ(left.value("node_id", std::string()), client.node_id());

    proc_terminate(p);
    EXPECT_EQ(exit_code_of(p), 0) << "stderr: " << p.err;
}

TEST(SwarmBinaryE2E, BinaryBadConfigExit) {
    // 坏 JSON 配置文件 → daemonize 之前报错退出（非零、非信号）
    char cfg_path[] = "/tmp/falcon_swarmd_e2e_bad_XXXXXX";
    const int fd = ::mkstemp(cfg_path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(static_cast<size_t>(
                  ::write(fd, "{not json", 9)),
              9u);
    ::close(fd);

    Proc p;
    ASSERT_TRUE(proc_start(p, {"--conf-path", cfg_path}));
    const bool exited = proc_wait_exit(p, 10000);
    proc_terminate(p);  // 若已退出则为 no-op
    ASSERT_TRUE(exited) << "bad config should exit promptly";
    EXPECT_NE(exit_code_of(p), 0);

    // --help 零退出
    Proc h;
    ASSERT_TRUE(proc_start(h, {"--help"}));
    ASSERT_TRUE(proc_wait_exit(h, 10000));
    proc_terminate(h);
    EXPECT_EQ(exit_code_of(h), 0);

    ::unlink(cfg_path);
}

// ===========================================================================
// CLI 参数边界：无效数值/未知参数 → exit 1 + 精确 stderr
// （parse_positive_int/parse_size 的 throw 两方向 + 主循环未知参数收口）
// ===========================================================================

TEST(SwarmBinaryE2E, BinaryCliInvalidArgsRejected) {
    struct ArgCase {
        std::vector<std::string> args;
        std::string expect_err;
    };
    const std::vector<ArgCase> cases = {
        // 未知参数（含 option 缺值——i+1 越界不进对应分支落未知参数）
        {{"--no-conf", "--bogus"}, "Unknown argument: --bogus"},
        {{"--no-conf", "--server-token"}, "Unknown argument: --server-token"},
        // --swarm-port 范围门（stoi 成功但越界）
        {{"--no-conf", "--swarm-port", "99999"},
         "Invalid --swarm-port: 99999"},
        // parse_positive_int：非数值（stoll invalid_argument）、<=0、超 int
        {{"--no-conf", "--heartbeat-interval-s", "abc"},
         "Invalid --heartbeat-interval-s: abc"},
        {{"--no-conf", "--heartbeat-interval-s", "0"},
         "Invalid --heartbeat-interval-s: 0"},
        {{"--no-conf", "--heartbeat-interval-s", "99999999999"},
         "Invalid --heartbeat-interval-s: 99999999999"},
        {{"--no-conf", "--heartbeat-timeout-s", "-5"},
         "Invalid --heartbeat-timeout-s: -5"},
        {{"--no-conf", "--challenge-ttl-s", "abc"},
         "Invalid --challenge-ttl-s: abc"},
        {{"--no-conf", "--sweep-interval-ms", "abc"},
         "Invalid --sweep-interval-ms: abc"},
        // parse_size：负值（<0 门）+ stoll 直接 out_of_range
        {{"--no-conf", "--rate-register-per-min", "-1"},
         "Invalid --rate-register-per-min: -1"},
        {{"--no-conf", "--rate-query-per-min", "99999999999999999999"},
         "Invalid --rate-query-per-min: 99999999999999999999"},
    };
    for (const auto& c : cases) {
        expect_fail_run(c.args, c.expect_err);
    }
}

// ===========================================================================
// 配置文件缺失：--conf-path 显式指定必须存在（daemonize 之前 fail-fast）
// ===========================================================================

TEST(SwarmBinaryE2E, BinaryConfPathNotFound) {
    // mkstemp 建后即删 = 保证不存在的路径
    char cfg_path[] = "/tmp/falcon_swarmd_e2e_absent_XXXXXX";
    const int fd = ::mkstemp(cfg_path);
    ASSERT_GE(fd, 0);
    ::close(fd);
    ::unlink(cfg_path);

    expect_fail_run({"--conf-path", cfg_path},
                    std::string("Config file not found: ") + cfg_path);
}

// ===========================================================================
// 端口占用：service.start() 失败 → exit 1 + last_error 明示
// ===========================================================================

TEST(SwarmBinaryE2E, BinaryPortOccupiedFails) {
    // 持有 listener 不放（pick_free_port 会 close——本用例必须占着）
    const int holder = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(holder, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(holder, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
              0);
    ASSERT_EQ(::listen(holder, 1), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(holder, reinterpret_cast<sockaddr*>(&addr), &len),
              0);
    const std::uint16_t port = ntohs(addr.sin_port);

    expect_fail_run({"--no-conf", "--swarm-host", "127.0.0.1", "--swarm-port",
                     std::to_string(port), "--server-token", "occupied"},
                    "Failed to start rendezvous service");
    ::close(holder);
}

// ===========================================================================
// 前台全参数成功路径（flushed 进程承载 main.cpp 早期行覆盖）：
// 配置文件未知键告警 + 无 token 告警 + 监听行 + 未鉴权 query -32003
// ===========================================================================

TEST(SwarmBinaryE2E, BinaryForegroundConfigWarningsAndFullArgs) {
    const std::uint16_t port = pick_free_port();
    const std::string cfg_path =
        write_temp_file("falcon_swarmd_e2e_fg",
                        R"({"swarm": {"bogus_option": "x"}})");
    ASSERT_FALSE(cfg_path.empty());

    Proc p;
    ASSERT_TRUE(proc_start(p, {
        "--conf-path", cfg_path,
        "--swarm-host", "127.0.0.1",
        "--swarm-port", std::to_string(port),
        "--group-token", "gtok1",
        "--challenge-ttl-s", "90",
        "--rate-register-per-min", "3",
        "--rate-query-per-min", "4",
        "--pid-file", cfg_path + ".pid",
        "--working-dir", "/tmp",
        "--log-file", cfg_path + ".log",
        // 刻意不给 --server-token：触发空 token 启动告警
    }));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 10000));

    // 未鉴权 query：无 token 直达 state → 未知 session -32003（非 -32001）
    const auto out = falcon::swarm::test::http_rpc_call(
        port, "falcon.swarm.query",
        {{"session", "s-nonexistent"}, {"sha256", std::string(64, 'c')}});
    EXPECT_EQ(out.error_code, -32003) << out.raw_body;

    proc_terminate(p);  // SIGTERM 优雅停机（gcda 铁律）
    ASSERT_EQ(exit_code_of(p), 0) << "stderr: " << p.err;

    // 前台 stdout 监听行（退出时 flush）
    EXPECT_NE(p.out.find("falcon-swarmd listening on 127.0.0.1:"),
              std::string::npos)
        << "stdout: " << p.out;
    // 两条启动告警（未知键在预载段、空 token 在参数循环后——均 daemonize 前）
    EXPECT_NE(p.err.find("unknown key in 'swarm' section: bogus_option"),
              std::string::npos)
        << "stderr: " << p.err;
    EXPECT_NE(p.err.find("Warning: server_token is empty"), std::string::npos)
        << "stderr: " << p.err;

    ::unlink(cfg_path.c_str());
    ::unlink((cfg_path + ".pid").c_str());
    ::unlink((cfg_path + ".log").c_str());
}

// ===========================================================================
// 守护化生命周期：直系子进程快速 exit 0（fork 父 _exit）→ 孙进程按 pid 文件
// 存活、端口可连 → SIGTERM 排水 → ESRCH；日志落盘
// ===========================================================================

TEST(SwarmBinaryE2E, BinaryDaemonModeLifecycle) {
    char dir_tmpl[] = "/tmp/falcon_swarmd_e2e_daemon_XXXXXX";
    ASSERT_NE(::mkdtemp(dir_tmpl), nullptr);
    const std::string dir(dir_tmpl);
    const std::string pid_path = dir + "/swarmd.pid";
    const std::string log_path = dir + "/swarmd.log";
    const std::string cfg_path =
        write_temp_file("falcon_swarmd_e2e_dm",
                        R"({"swarm": {"bogus_option": "y"}})");
    ASSERT_FALSE(cfg_path.empty());
    const std::uint16_t port = pick_free_port();

    Proc p;
    ASSERT_TRUE(proc_start(p, {
        "--conf-path", cfg_path,
        "--swarm-host", "127.0.0.1",
        "--swarm-port", std::to_string(port),
        "--server-token", "dm-token",
        "--heartbeat-interval-s", "1",
        "--pid-file", pid_path,
        "--working-dir", dir,
        "--log-file", log_path,
        "-d",
    }));

    // 直系子进程（fork1 父）快速 _exit(0)
    ASSERT_TRUE(proc_wait_exit(p, 10000)) << "direct child should exit fast";
    EXPECT_EQ(exit_code_of(p), 0) << "stderr: " << p.err;
    // daemonize 前的告警已进直系子进程 stderr（cerr 逐字节即时）
    EXPECT_NE(p.err.find("unknown key in 'swarm' section: bogus_option"),
              std::string::npos)
        << "stderr: " << p.err;

    // pid 文件出现（孙进程写入）→ 存活且非直系子进程
    // （内容容忍尾换行——DaemonManager 写 "PID\n"）
    ASSERT_TRUE(wait_until(
        [&] {
            const std::string content = read_file_if_exists(pid_path);
            return !content.empty() &&
                   content.find_first_not_of("0123456789 \t\r\n") ==
                       std::string::npos &&
                   content.find_first_of("0123456789") != std::string::npos;
        },
        10000));
    const pid_t daemon_pid =
        static_cast<pid_t>(std::stol(read_file_if_exists(pid_path)));
    EXPECT_NE(daemon_pid, p.pid);
    EXPECT_EQ(::kill(daemon_pid, 0), 0) << "daemon pid not alive";

    // 端口可连（服务已启动）
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 10000));

    // SIGTERM 孙进程 → 正常退出（重定向后 flush 落 log 文件）
    ::kill(daemon_pid, SIGTERM);
    ASSERT_TRUE(wait_until(
        [&] { return ::kill(daemon_pid, 0) != 0 && errno == ESRCH; }, 15000))
        << "daemon pid still alive after SIGTERM";

    const std::string log = read_file_if_exists(log_path);
    // 守护模式 stdout 横幅不打印（main.cpp 仅前台打印）——日志里的监听
    // 铁证是 rendezvous 服务的 spdlog 行（swarm_rpc_server.cpp start()）
    EXPECT_NE(log.find("swarmd listening on"), std::string::npos)
        << "log: " << log;

    proc_terminate(p);  // 直系已退出，仅关管道
    ::unlink(cfg_path.c_str());
    ::unlink(pid_path.c_str());
    ::unlink(log_path.c_str());
    ::rmdir(dir.c_str());
}
