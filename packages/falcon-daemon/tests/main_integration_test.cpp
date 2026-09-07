// Integration tests for the falcon-daemon binary (src/main.cpp).
//
// Each test runs the actual executable (fork + execv) with different argument
// sets and asserts on the exit code / output. The binary is compiled with
// --coverage, so every child that terminates normally through exit()/return
// writes its .gcda data back into the build tree and contributes to coverage.
//
// NOTE: children are always stopped with SIGTERM (never SIGKILL) so that the
// normal shutdown path (and the gcov flush) runs.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef FALCON_DAEMON_BIN
#define FALCON_DAEMON_BIN ""
#endif

// The JSON helpers are implemented without nlohmann/json to keep this test
// independent of the daemon libraries; we only need to build request strings
// and grep response bodies.

namespace {

namespace fs = std::filesystem;

constexpr int kStartTimeoutMs = 10000;

// ===========================================================================
// Small utilities
// ===========================================================================

std::string make_temp_dir() {
    std::string tmpl = ::testing::TempDir() + "/falcon-daemon-main-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* r = ::mkdtemp(buf.data());
    EXPECT_NE(r, nullptr);
    return r != nullptr ? std::string(r) : tmpl;
}

struct TempDirGuard {
    std::string path;
    explicit TempDirGuard(std::string p) : path(std::move(p)) {}
    ~TempDirGuard() { std::error_code ec; fs::remove_all(path, ec); }
};

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = kStartTimeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool daemon_binary_available() {
    return std::strlen(FALCON_DAEMON_BIN) > 0 &&
           ::access(FALCON_DAEMON_BIN, X_OK) == 0;
}

// ===========================================================================
// Child process plumbing
// ===========================================================================

struct Proc {
    pid_t pid = -1;
    int out_fd = -1;
    int err_fd = -1;
    bool out_eof = false;
    bool err_eof = false;
    bool exited = false;
    int exit_status = -1;  // raw wait status
    std::string out;
    std::string err;
};

extern char** environ;

void drain_pipe(int& fd, bool& eof, std::string& sink) {
    if (fd < 0 || eof) return;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            sink.append(buf, buf + n);
            continue;
        }
        if (n == 0) {
            eof = true;
            ::close(fd);
            fd = -1;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        eof = true;
        ::close(fd);
        fd = -1;
        return;
    }
}

void poll_drain(Proc& p, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (p.out_eof && p.err_eof) return;
        const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now())
                                .count();
        if (remain <= 0) return;
        pollfd fds[2]{};
        int nfds = 0;
        int out_idx = -1;
        int err_idx = -1;
        if (!p.out_eof) {
            fds[nfds].fd = p.out_fd;
            fds[nfds].events = POLLIN;
            out_idx = nfds;
            ++nfds;
        }
        if (!p.err_eof) {
            fds[nfds].fd = p.err_fd;
            fds[nfds].events = POLLIN;
            err_idx = nfds;
            ++nfds;
        }
        if (nfds == 0) return;
        const int r = ::poll(fds, static_cast<nfds_t>(nfds), static_cast<int>(remain));
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (r == 0) return;
        if (out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP)) != 0) {
            drain_pipe(p.out_fd, p.out_eof, p.out);
        }
        if (err_idx >= 0 && (fds[err_idx].revents & (POLLIN | POLLHUP)) != 0) {
            drain_pipe(p.err_fd, p.err_eof, p.err);
        }
    }
}

bool proc_start(Proc& p, const std::vector<std::string>& args, const std::string& cwd) {
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
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) ::close(fd);
        return false;
    }

    if (pid == 0) {
        // === child ===
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        if (out_pipe[1] > STDERR_FILENO) ::close(out_pipe[1]);
        if (err_pipe[1] > STDERR_FILENO) ::close(err_pipe[1]);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            ::_exit(126);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(FALCON_DAEMON_BIN));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(FALCON_DAEMON_BIN, argv.data());
        ::_exit(127);  // exec failed
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    p.pid = pid;
    p.out_fd = out_pipe[0];
    p.err_fd = err_pipe[0];
    // Non-blocking reads driven by poll().
    ::fcntl(p.out_fd, F_SETFL, ::fcntl(p.out_fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(p.err_fd, F_SETFL, ::fcntl(p.err_fd, F_GETFL) | O_NONBLOCK);
    return true;
}

void proc_signal(Proc& p, int sig) {
    if (p.pid > 0 && !p.exited) {
        ::kill(p.pid, sig);
    }
}

bool check_exited(Proc& p) {
    int status = 0;
    const pid_t r = ::waitpid(p.pid, &status, WNOHANG);
    if (r == p.pid) {
        p.exited = true;
        p.exit_status = status;
        return true;
    }
    return false;
}

bool proc_wait_exit(Proc& p, int timeout_ms) {
    if (!wait_until([&] { return check_exited(p); }, timeout_ms)) return false;
    poll_drain(p, 500);
    return true;
}

// Wait for the child to exit on its own; SIGKILL only as a last resort.
bool proc_finish(Proc& p, int timeout_ms) {
    if (proc_wait_exit(p, timeout_ms)) return true;
    proc_signal(p, SIGKILL);
    int status = 0;
    ::waitpid(p.pid, &status, 0);
    p.exited = true;
    p.exit_status = status;
    return false;
}

int exit_code_of(const Proc& p) {
    if (!p.exited || !WIFEXITED(p.exit_status)) return -1;
    return WEXITSTATUS(p.exit_status);
}

// ===========================================================================
// Minimal TCP + HTTP/JSON-RPC client
// ===========================================================================

uint16_t pick_free_port() {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint16_t port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            port = ntohs(bound.sin_port);
        }
    }
    ::close(s);
    return port;
}

bool tcp_connect(uint16_t port) {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(s);
    return ok;
}

std::optional<std::string> http_post(uint16_t port, const std::string& body) {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return std::nullopt;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return std::nullopt;
    }

    std::string req;
    req += "POST /jsonrpc HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    req += body;

    std::size_t off = 0;
    while (off < req.size()) {
        const ssize_t n = ::send(s, req.data() + off, req.size() - off, 0);
        if (n <= 0) {
            ::close(s);
            return std::nullopt;
        }
        off += static_cast<std::size_t>(n);
    }

    std::string resp;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, buf + n);
        if (resp.size() > 1024 * 1024) break;
    }
    ::close(s);
    return resp;
}

std::string http_body(const std::string& resp) {
    const std::string sep = "\r\n\r\n";
    const auto pos = resp.find(sep);
    if (pos == std::string::npos) return "";
    return resp.substr(pos + sep.size());
}

std::string make_rpc_body(const std::string& method, const std::string& params_json) {
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"") + method +
           "\",\"params\":" + params_json + "}";
}

// ===========================================================================
// High-level runners
// ===========================================================================

struct RunResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string out;
    std::string err;
};

// Run to natural exit (fast-exiting invocations only).
RunResult run_wait(const std::vector<std::string>& args, int timeout_ms = kStartTimeoutMs,
                   const std::string& cwd = "") {
    RunResult result;
    Proc p;
    if (!proc_start(p, args, cwd)) {
        result.timed_out = true;
        return result;
    }
    result.timed_out = !proc_finish(p, timeout_ms);
    result.exit_code = exit_code_of(p);
    result.out = p.out;
    result.err = p.err;
    if (!p.exited || !WIFEXITED(p.exit_status)) result.exit_code = -1;
    return result;
}

// Start, optionally wait for an RPC port to come up, deliver signals, then
// SIGTERM and expect a clean exit.
RunResult run_terminate(const std::vector<std::string>& args,
                        uint16_t rpc_port = 0,
                        const std::vector<int>& pre_signals = {},
                        int uptime_ms = 300) {
    RunResult result;
    Proc p;
    if (!proc_start(p, args, "")) {
        result.timed_out = true;
        return result;
    }

    if (rpc_port != 0) {
        const bool ready = wait_until([&] {
            poll_drain(p, 20);
            return tcp_connect(rpc_port);
        }, 8000);
        if (!ready) {
            proc_finish(p, 3000);
            result.timed_out = true;
            result.out = p.out;
            result.err = p.err;
            return result;
        }
    } else {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(uptime_ms);
        while (std::chrono::steady_clock::now() < until) {
            poll_drain(p, 20);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    for (int sig : pre_signals) {
        proc_signal(p, sig);
        poll_drain(p, 300);
    }

    proc_signal(p, SIGTERM);
    result.timed_out = !proc_finish(p, 8000);
    result.exit_code = exit_code_of(p);
    result.out = p.out;
    result.err = p.err;
    if (!p.exited || !WIFEXITED(p.exit_status)) result.exit_code = -1;
    return result;
}

// ===========================================================================
// Tests
// ===========================================================================

class MainIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!daemon_binary_available()) {
            GTEST_SKIP() << "falcon-daemon binary not available";
        }
    }
};

TEST_F(MainIntegrationTest, HelpLongFlagExitsZero) {
    auto r = run_wait({"--help"});
    ASSERT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "Falcon Daemon")) << r.out;
    EXPECT_TRUE(contains(r.out, "--enable-rpc")) << r.out;
    EXPECT_TRUE(contains(r.out, "--rpc-listen-port")) << r.out;
}

TEST_F(MainIntegrationTest, HelpShortFlagExitsZero) {
    auto r = run_wait({"-h"});
    ASSERT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "Falcon Daemon")) << r.out;
}

TEST_F(MainIntegrationTest, UnknownArgumentExitsOne) {
    auto r = run_wait({"--bogus-flag"});
    ASSERT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "Unknown argument: --bogus-flag")) << r.err;
}

TEST_F(MainIntegrationTest, TrailingOptionWithoutValueIsRejected) {
    for (const char* const arg : {"--rpc-listen-port", "--rpc-secret", "--rpc-listen-host",
                                  "--pid-file", "--working-dir", "--log-file", "--task-db"}) {
        auto r = run_wait({std::string(arg)});
        ASSERT_FALSE(r.timed_out) << arg;
        EXPECT_EQ(r.exit_code, 1) << arg;
        EXPECT_TRUE(contains(r.err, "Unknown argument")) << arg << " -> " << r.err;
    }
}

TEST_F(MainIntegrationTest, EnableRpcFalseRunsUntilSigterm) {
    auto r = run_terminate({"--enable-rpc=false"});
    ASSERT_FALSE(r.timed_out) << r.out << r.err;
    EXPECT_EQ(r.exit_code, 0);
}

TEST_F(MainIntegrationTest, EnableRpcFalsyVariantsAllRun) {
    for (const char* const v : {"0", "false", "no", "off"}) {
        auto r = run_terminate({"--enable-rpc=" + std::string(v)});
        ASSERT_FALSE(r.timed_out) << v;
        EXPECT_EQ(r.exit_code, 0) << v;
    }
}

TEST_F(MainIntegrationTest, EnableRpcTruthyVariantsReachRpcStartup) {
    // With an invalid bind address the RPC server start fails fast (exit 1),
    // which proves the flag was parsed as enabled.
    for (const char* const v : {"1", "true", "TRUE", "yes", "on", "bogus", ""}) {
        auto r = run_wait({"--enable-rpc=" + std::string(v), "--rpc-listen-host", "not-an-ip"}, 15000);
        ASSERT_FALSE(r.timed_out) << v;
        EXPECT_EQ(r.exit_code, 1) << v;
        EXPECT_TRUE(contains(r.err, "Failed to start JSON-RPC server")) << v << " -> " << r.err;
    }
}

TEST_F(MainIntegrationTest, RpcServerServesRequestsAndStopsCleanly) {
    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);

    Proc p;
    ASSERT_TRUE(proc_start(p, {"--enable-rpc", "--rpc-listen-port", std::to_string(port),
                               "--rpc-allow-origin-all"},
                           ""));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 8000));

    const auto resp = http_post(port, make_rpc_body("system.listMethods", "[]"));
    ASSERT_TRUE(resp.has_value()) << "no RPC response";
    EXPECT_TRUE(contains(*resp, "HTTP/1.1 200")) << *resp;
    EXPECT_TRUE(contains(*resp, "Access-Control-Allow-Origin: *")) << *resp;
    const auto body = http_body(*resp);
    EXPECT_TRUE(contains(body, "aria2.addUri")) << body;
    EXPECT_TRUE(contains(body, "system.multicall")) << body;

    const auto ver = http_post(port, make_rpc_body("aria2.getVersion", "[]"));
    ASSERT_TRUE(ver.has_value());
    EXPECT_TRUE(contains(http_body(*ver), "0.1.0")) << *ver;

    proc_signal(p, SIGTERM);
    EXPECT_TRUE(proc_finish(p, 8000));
    EXPECT_EQ(exit_code_of(p), 0);
}

TEST_F(MainIntegrationTest, RpcSecretEnforcesToken) {
    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);

    Proc p;
    ASSERT_TRUE(proc_start(p, {"--enable-rpc", "--rpc-secret", "int-test-token",
                               "--rpc-listen-port", std::to_string(port)},
                           ""));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 8000));

    // Missing token -> unauthorized.
    auto resp = http_post(port, make_rpc_body("system.listMethods", "[]"));
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(contains(http_body(*resp), "-32001")) << *resp;

    // Wrong token -> unauthorized.
    resp = http_post(port, make_rpc_body("system.listMethods", "[\"token:wrong\"]"));
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(contains(http_body(*resp), "-32001")) << *resp;

    // Correct token -> success.
    resp = http_post(port, make_rpc_body("system.listMethods", "[\"token:int-test-token\"]"));
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(contains(http_body(*resp), "aria2.addUri")) << *resp;

    proc_signal(p, SIGTERM);
    EXPECT_TRUE(proc_finish(p, 8000));
    EXPECT_EQ(exit_code_of(p), 0);
}

TEST_F(MainIntegrationTest, RpcInvalidHostFailsFast) {
    auto r = run_wait({"--enable-rpc", "--rpc-listen-host", "not-an-ip"}, 15000);
    ASSERT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "Failed to start JSON-RPC server")) << r.err;
}

TEST_F(MainIntegrationTest, DaemonModeLifecycle) {
    TempDirGuard tmp(make_temp_dir());
    const std::string pid_file = tmp.path + "/daemon.pid";
    const std::string log_file = tmp.path + "/daemon.log";

    // The exec'd process daemonizes: the direct child exits 0 immediately,
    // the daemonized grandchild keeps running in the background.
    auto r = run_wait({"--daemon",
                       "--pid-file", pid_file,
                       "--working-dir", tmp.path,
                       "--log-file", log_file,
                       "--task-db", tmp.path + "/tasks.db",
                       "--enable-rpc=false"},
                      15000);
    ASSERT_FALSE(r.timed_out) << r.out << r.err;
    EXPECT_EQ(r.exit_code, 0);

    // The daemonized grandchild must have written its pid file.
    ASSERT_TRUE(wait_until([&] { return file_exists(pid_file); }, 8000));
    std::ifstream in(pid_file);
    int daemon_pid = -1;
    in >> daemon_pid;
    ASSERT_GT(daemon_pid, 0);
    EXPECT_NE(daemon_pid, static_cast<int>(::getpid()));
    EXPECT_EQ(::kill(static_cast<pid_t>(daemon_pid), 0), 0);

    // SIGHUP triggers the reload callback, SIGTERM the graceful shutdown.
    EXPECT_EQ(::kill(static_cast<pid_t>(daemon_pid), SIGHUP), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(::kill(static_cast<pid_t>(daemon_pid), SIGTERM), 0);

    // Normal exit removes the pid file (DaemonManager destructor).
    EXPECT_TRUE(wait_until([&] { return !file_exists(pid_file); }, 10000))
        << "daemonized process did not shut down cleanly";

    EXPECT_TRUE(file_exists(log_file));
}

} // namespace
