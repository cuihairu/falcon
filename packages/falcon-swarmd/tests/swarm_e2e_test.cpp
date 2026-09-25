// ============================================================================
// falcon-swarmd 真二进制 e2e（SwarmBinaryE2E，POSIX-only）
//
// fork + execv 真实二进制（FALCON_SWARMD_BIN 编译定义），覆盖：
//   - 全流程：CLI 参数启动 → WS 订阅收 onPeerJoined（含 client node_id）
//     → SwarmClient 注册/心跳/空表查询 → SIGTERM 信号停机 exit 0
//     （gcda 铁律：子进程恒 SIGTERM 优雅收尾，SIGKILL 仅兜底）
//   - 心跳超时摘除端到端：client detach()（停心跳保持注册）→
//     server sweep 摘除 → 订阅侧收 onPeerLeft
//   - 配置面：坏 JSON 配置文件非零退出（daemonize 前报错）+ --help 零退出
// ============================================================================

#include "client/swarm_client.hpp"
#include "swarm_server_harness.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

}  // namespace

// ===========================================================================
// 全流程：二进制 + client 库 × 同一 server
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

    // detach = 模拟崩溃：停心跳、保持注册 → server 心跳超时摘除
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
