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
#include <iterator>
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

TEST_F(MainIntegrationTest, ConfFileReloadUpdatesSecretOnSighup) {
    TempDirGuard tmp(make_temp_dir());
    const std::string conf = tmp.path + "/daemon.json";
    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);

    const auto write_conf = [&](const char* secret) {
        std::ofstream out(conf);
        out << "{\"rpc\":{\"enabled\":true,\"host\":\"127.0.0.1\","
            << "\"port\":" << port << ",\"secret\":\"" << secret << "\"}}";
    };
    write_conf("reload-old-secret");

    Proc p;
    ASSERT_TRUE(proc_start(p, {"--conf-path", conf}, ""));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 8000));

    // Old secret from the file works.
    auto resp = http_post(port, make_rpc_body("system.listMethods", "[\"token:reload-old-secret\"]"));
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(contains(http_body(*resp), "aria2.addUri")) << *resp;

    // Rewrite the file with a new secret, then SIGHUP.
    write_conf("reload-new-secret");
    ASSERT_EQ(::kill(p.pid, SIGHUP), 0);
    // Main loop consumes the pending reload within ~100ms; poll for the
    // new secret to be enforced (config parse + server update take a moment).
    bool applied = false;
    for (int i = 0; i < 250 && !applied; ++i) {
        auto probe = http_post(port, make_rpc_body("system.listMethods", "[\"token:reload-new-secret\"]"));
        applied = probe.has_value() && contains(http_body(*probe), "aria2.addUri");
        if (!applied) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(applied) << "new secret never became active after SIGHUP";

    // Old secret is rejected after the reload.
    resp = http_post(port, make_rpc_body("system.listMethods", "[\"token:reload-old-secret\"]"));
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(contains(http_body(*resp), "-32001")) << *resp;

    proc_signal(p, SIGTERM);
    EXPECT_TRUE(proc_finish(p, 8000));
    EXPECT_EQ(exit_code_of(p), 0);
}

TEST_F(MainIntegrationTest, ConfFileReloadKeepsRunningOnBrokenFile) {
    TempDirGuard tmp(make_temp_dir());
    const std::string conf = tmp.path + "/daemon.json";
    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);

    {
        std::ofstream out(conf);
        out << "{\"rpc\":{\"enabled\":true,\"host\":\"127.0.0.1\","
            << "\"port\":" << port << ",\"secret\":\"survivor-secret\"}}";
    }

    Proc p;
    ASSERT_TRUE(proc_start(p, {"--conf-path", conf}, ""));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 8000));

    // Break the file, then SIGHUP: reload must fail open (keep the current
    // config) instead of killing a running daemon.
    {
        std::ofstream out(conf);
        out << "{ this is not json";
    }
    ASSERT_EQ(::kill(p.pid, SIGHUP), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Daemon is still alive and still authenticates with the old secret.
    auto resp = http_post(port, make_rpc_body("system.listMethods", "[\"token:survivor-secret\"]"));
    ASSERT_TRUE(resp.has_value()) << "daemon died after reloading a broken config";
    EXPECT_TRUE(contains(http_body(*resp), "aria2.addUri")) << *resp;

    proc_signal(p, SIGTERM);
    EXPECT_TRUE(proc_finish(p, 8000));
    EXPECT_EQ(exit_code_of(p), 0);
}

TEST_F(MainIntegrationTest, DownloadOptionsFromConfigAndSighupReload) {
    TempDirGuard tmp(make_temp_dir());
    const std::string conf = tmp.path + "/daemon.json";
    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);

    const auto write_conf = [&](int max_tasks) {
        std::ofstream out(conf);
        out << "{\"rpc\":{\"enabled\":true,\"host\":\"127.0.0.1\","
            << "\"port\":" << port << ",\"secret\":\"dl-conf-token\"},"
            << "\"download\":{\"max_concurrent_tasks\":" << max_tasks << "}}";
    };
    write_conf(2);

    Proc p;
    ASSERT_TRUE(proc_start(p, {"--conf-path", conf}, ""));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 8000));

    const auto body_with = [&](const std::string& params) {
        return http_post(port, make_rpc_body("aria2.getGlobalOption", params));
    };

    // 配置文件的 download 节在启动时生效
    auto resp = body_with("[\"token:dl-conf-token\"]");
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(contains(http_body(*resp), "\"max-concurrent-downloads\":\"2\"")) << *resp;

    // 改文件 + SIGHUP：下载参数是运行时可调项，热更立即生效
    write_conf(3);
    ASSERT_EQ(::kill(p.pid, SIGHUP), 0);
    bool applied = false;
    for (int i = 0; i < 250 && !applied; ++i) {
        auto probe = body_with("[\"token:dl-conf-token\"]");
        applied = probe.has_value() &&
                  contains(http_body(*probe), "\"max-concurrent-downloads\":\"3\"");
        if (!applied) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(applied) << "download option never picked up after SIGHUP";

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

// ===========================================================================
// Config file (--conf-path) tests
// ===========================================================================

void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    ASSERT_TRUE(out.good()) << path;
    out << content;
}

TEST_F(MainIntegrationTest, HelpMentionsConfPath) {
    auto r = run_wait({"--help"});
    ASSERT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "--conf-path")) << r.out;
    EXPECT_TRUE(contains(r.out, "--no-conf")) << r.out;
}

TEST_F(MainIntegrationTest, ConfPathMissingFileExitsOne) {
    auto r = run_wait({"--conf-path", "/falcon-does-not-exist/daemon.json"});
    ASSERT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "cannot open config file")) << r.err;
}

TEST_F(MainIntegrationTest, ConfPathInvalidJsonExitsOne) {
    TempDirGuard tmp(make_temp_dir());
    const std::string conf = tmp.path + "/daemon.json";
    write_file(conf, "{not valid json");

    auto r = run_wait({"--conf-path", conf});
    ASSERT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "failed to parse config file")) << r.err;
}

// rpc.enabled + rpc.port 从配置文件生效：无 CLI 参数也启动 RPC
TEST_F(MainIntegrationTest, ConfPathStartsRpcAndStopsCleanly) {
    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);

    TempDirGuard tmp(make_temp_dir());
    const std::string conf = tmp.path + "/daemon.json";
    write_file(conf, R"({
        "rpc": { "enabled": true, "port": )" + std::to_string(port) + R"( }
    })");

    auto r = run_terminate({"--conf-path", conf}, port);
    ASSERT_FALSE(r.timed_out) << r.out << r.err;
    EXPECT_EQ(r.exit_code, 0);
}

// CLI 显式参数覆盖配置文件值：文件 port=A，CLI --rpc-listen-port B → B 生效
TEST_F(MainIntegrationTest, CliOverridesConfigFilePort) {
    const uint16_t file_port = pick_free_port();
    const uint16_t cli_port = pick_free_port();
    ASSERT_NE(file_port, 0);
    ASSERT_NE(cli_port, 0);

    TempDirGuard tmp(make_temp_dir());
    const std::string conf = tmp.path + "/daemon.json";
    write_file(conf, R"({
        "rpc": { "enabled": true, "port": )" + std::to_string(file_port) + R"( }
    })");

    auto r = run_terminate({"--conf-path", conf,
                            "--rpc-listen-port", std::to_string(cli_port)},
                           cli_port);
    ASSERT_FALSE(r.timed_out) << r.out << r.err;
    EXPECT_EQ(r.exit_code, 0);
    // CLI 端口生效后，文件端口不应被占用
    EXPECT_FALSE(tcp_connect(file_port)) << "file port was used instead of CLI port";
}

// 文件中的 secret 生效 + 未知键告警不致命
TEST_F(MainIntegrationTest, ConfigFileSecretAndUnknownKeyWarning) {
    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);

    TempDirGuard tmp(make_temp_dir());
    const std::string conf = tmp.path + "/daemon.json";
    write_file(conf, R"({
        "rpc": { "enabled": true, "port": )" + std::to_string(port) +
                      R"(, "secret": "conf-secret", "prot": 6800 },
        "mystery_section": {}
    })");

    Proc p;
    ASSERT_TRUE(proc_start(p, {"--conf-path", conf}, ""));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port); }, 8000));

    // 无 token → -32001（secret 来自配置文件）
    auto resp = http_post(port, make_rpc_body("system.listMethods", "[]"));
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(contains(http_body(*resp), "-32001")) << *resp;

    proc_signal(p, SIGTERM);
    EXPECT_TRUE(proc_finish(p, 8000));
    EXPECT_EQ(exit_code_of(p), 0);

    // 未知节/键告警到 stderr，但进程正常启动
    EXPECT_TRUE(contains(p.err, "unknown key in 'rpc' section: prot")) << p.err;
    EXPECT_TRUE(contains(p.err, "unknown config section: mystery_section")) << p.err;
}

// --no-conf 短路一切配置文件加载（显式路径也被忽略）
TEST_F(MainIntegrationTest, NoConfIgnoresConfPath) {
    auto r = run_terminate({"--no-conf",
                            "--conf-path", "/falcon-does-not-exist/daemon.json"});
    ASSERT_FALSE(r.timed_out) << r.out << r.err;
    EXPECT_EQ(r.exit_code, 0);
}

// 默认路径 $HOME/.config/falcon/daemon.json 存在时自动加载（aria2 语义）
TEST_F(MainIntegrationTest, DefaultConfigPathAutoLoaded) {
    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);

    TempDirGuard tmp(make_temp_dir());
    fs::create_directories(tmp.path + "/.config/falcon");
    write_file(tmp.path + "/.config/falcon/daemon.json",
               R"({ "rpc": { "enabled": true, "port": )" + std::to_string(port) + R"( } })");

    // 子进程继承 HOME → 默认配置路径指向 tmp
    const char* old_home = ::getenv("HOME");
    const std::string old_home_str = old_home ? old_home : "";
    ::setenv("HOME", tmp.path.c_str(), 1);

    auto r = run_terminate({}, port);

    if (!old_home_str.empty()) ::setenv("HOME", old_home_str.c_str(), 1);

    ASSERT_FALSE(r.timed_out) << r.out << r.err;
    EXPECT_EQ(r.exit_code, 0);
}

// ===========================================================================
// V2 HTTP engine (--http-engine v2) end-to-end tests
// ===========================================================================

std::string make_payload(std::size_t size) {
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 31 + 17) % 251);
    }
    return data;
}

std::string read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// 从 JSON 应答里提取字符串字段（result/status/completedLength 足够）
std::string json_string_field(const std::string& body, const std::string& field) {
    const std::string key = "\"" + field + "\":";
    const auto pos = body.find(key);
    if (pos == std::string::npos) return "";
    const auto first = body.find('"', pos + key.size());
    if (first == std::string::npos) return "";
    const auto last = body.find('"', first + 1);
    if (last == std::string::npos) return "";
    return body.substr(first + 1, last - first - 1);
}

// 回环 HTTP 文件服务器：内存负载，HEAD → 200 + Accept-Ranges，
// GET 带 Range → 206 区间、不带 → 200 全量。慢速分块投递（构造参数）
// 为暂停/轮询留出传输窗口；每连接独立线程，析构统一 join。
class RangeFileServer {
public:
    RangeFileServer(std::string path, std::string payload, int chunk_delay_ms)
        : path_(std::move(path)), payload_(std::move(payload)),
          chunk_delay_ms_(chunk_delay_ms) {}

    ~RangeFileServer() { stop(); }

    RangeFileServer(const RangeFileServer&) = delete;
    RangeFileServer& operator=(const RangeFileServer&) = delete;

    bool start() {
        const int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return false;
        int one = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(s, 16) != 0) {
            ::close(s);
            return false;
        }
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
            ::close(s);
            return false;
        }
        port_ = ntohs(bound.sin_port);
        url_ = "http://127.0.0.1:" + std::to_string(port_) + path_;
        listen_fd_ = s;
        accept_thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    const std::string& url() const { return url_; }

    void stop() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        // join accept 线程后再收连接线程：此后不再有新的 push
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
    }

private:
    void accept_loop() {
        while (listen_fd_ >= 0) {
            pollfd pfd{};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 200) <= 0) continue;
            const int c = ::accept(listen_fd_, nullptr, nullptr);
            if (c < 0) continue;
            conn_threads_.emplace_back([this, c] { handle_connection(c); });
        }
    }

    void handle_connection(int fd) {
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 32 * 1024) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, buf + n);
        }

        const bool is_head = req.rfind("HEAD", 0) == 0;
        // Range: bytes=A-B / bytes=A-（无 B = 到文件尾）。
        // 双边界都必须忠实：206 长度与请求区间不符会被客户端的内容
        // 变更防护拒绝（Range 撒谎检测），任务按失败收口
        bool has_range = false;
        std::size_t range_begin = 0;
        std::size_t range_end = std::string::npos;
        const std::string range_key = "Range: bytes=";
        const auto range_pos = req.find(range_key);
        if (range_pos != std::string::npos) {
            has_range = true;
            std::size_t i = range_pos + range_key.size();
            while (i < req.size() && req[i] >= '0' && req[i] <= '9') {
                range_begin = range_begin * 10 + static_cast<std::size_t>(req[i] - '0');
                ++i;
            }
            if (i < req.size() && req[i] == '-') {
                ++i;
                if (req[i] >= '0' && req[i] <= '9') {
                    range_end = 0;
                    while (i < req.size() && req[i] >= '0' && req[i] <= '9') {
                        range_end = range_end * 10 +
                                    static_cast<std::size_t>(req[i] - '0');
                        ++i;
                    }
                }
            }
        }

        const std::string common =
            "Accept-Ranges: bytes\r\nETag: \"falcon-v2-e2e\"\r\nConnection: close\r\n";
        if (is_head) {
            const std::string head =
                "HTTP/1.1 200 OK\r\nContent-Length: " +
                std::to_string(payload_.size()) + "\r\n" + common + "\r\n";
            send_all(fd, head.data(), head.size());
            ::close(fd);
            return;
        }

        if (has_range && range_begin >= payload_.size()) {
            const std::string resp =
                "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n" +
                common + "\r\n";
            send_all(fd, resp.data(), resp.size());
            ::close(fd);
            return;
        }

        std::string status_head;
        const char* data = payload_.data();
        std::size_t size = payload_.size();
        if (has_range) {
            const std::size_t end =
                range_end == std::string::npos
                    ? payload_.size() - 1
                    : std::min(range_end, payload_.size() - 1);
            status_head =
                "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
                std::to_string(end - range_begin + 1) +
                "\r\nContent-Range: bytes " + std::to_string(range_begin) + "-" +
                std::to_string(end) + "/" + std::to_string(payload_.size()) +
                "\r\n" + common + "\r\n";
            data += range_begin;
            size = end - range_begin + 1;
        } else {
            status_head =
                "HTTP/1.1 200 OK\r\nContent-Length: " +
                std::to_string(payload_.size()) + "\r\n" + common + "\r\n";
        }

        if (!send_all(fd, status_head.data(), status_head.size())) {
            ::close(fd);
            return;
        }
        // 分块投递：慢速模式下块间留出暂停/轮询窗口
        const std::size_t chunk = 64 * 1024;
        std::size_t off = 0;
        while (off < size) {
            const std::size_t n = std::min(chunk, size - off);
            if (!send_all(fd, data + off, n)) break;
            off += n;
            if (chunk_delay_ms_ > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(chunk_delay_ms_));
            }
        }
        ::close(fd);
    }

    static bool send_all(int fd, const char* data, std::size_t size) {
        std::size_t off = 0;
        while (off < size) {
#ifdef __APPLE__
            const int flags = 0;
#else
            const int flags = MSG_NOSIGNAL;
#endif
            const ssize_t n = ::send(fd, data + off, size - off, flags);
            if (n <= 0) return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::string path_;
    std::string payload_;
    int chunk_delay_ms_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::string url_;
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

/// 启动 v2 模式 daemon（--no-conf + 独立 task_db），等待 RPC 端口就绪
static bool start_v2_daemon(Proc& p, uint16_t rpc_port, const std::string& db_path) {
    if (!proc_start(p, {"--no-conf", "--enable-rpc",
                        "--rpc-listen-port", std::to_string(rpc_port),
                        "--http-engine", "v2",
                        "--task-db", db_path},
                    "")) {
        return false;
    }
    return wait_until([&] {
        poll_drain(p, 20);
        return tcp_connect(rpc_port);
    }, 8000);
}

TEST_F(MainIntegrationTest, HttpEngineV2DownloadCompletesEndToEnd) {
    // 4MB：越过单段门禁，覆盖 V2 多段布局的成品发布
    const std::string payload = make_payload(4 * 1024 * 1024);
    RangeFileServer server("/v2e2e.bin", payload, 0);
    ASSERT_TRUE(server.start());

    const uint16_t port = pick_free_port();
    ASSERT_NE(port, 0);
    TempDirGuard tmp(make_temp_dir());
    const std::string db = tmp.path + "/tasks.db";

    Proc p;
    ASSERT_TRUE(start_v2_daemon(p, port, db));

    const std::string params =
        "[[\"" + server.url() + "\"],{\"dir\":\"" + tmp.path + "\"}]";
    const auto add = http_post(port, make_rpc_body("aria2.addUri", params));
    ASSERT_TRUE(add.has_value()) << "no addUri response";
    const std::string gid = json_string_field(http_body(*add), "result");
    ASSERT_FALSE(gid.empty()) << *add;

    const bool complete = wait_until([&] {
        const auto st =
            http_post(port, make_rpc_body("aria2.tellStatus", "[\"" + gid + "\"]"));
        return st && contains(http_body(*st), "\"status\":\"complete\"");
    }, 20000);
    ASSERT_TRUE(complete) << "task never completed";

    proc_signal(p, SIGTERM);
    EXPECT_TRUE(proc_finish(p, 10000));
    EXPECT_EQ(exit_code_of(p), 0) << p.err;

    // 成品逐字节一致；临时文件与控制文件已随发布消失
    const std::string final_path = tmp.path + "/v2e2e.bin";
    EXPECT_EQ(read_file_bytes(final_path), payload);
    EXPECT_FALSE(file_exists(final_path + ".falcon.tmp"));
    EXPECT_FALSE(file_exists(final_path + ".falcon.ctrl"));
}

TEST_F(MainIntegrationTest, HttpEngineV2PauseRestartResumeCompletes) {
    // 2MB 慢发（64KB 块 × 25ms ≈ 0.8s 全量）：留出暂停窗口。
    // 暂停 → SIGTERM 排水（V2 断点固化）→ 重启恢复 Paused 任务 →
    // unpause 续传 → 完成，成品逐字节一致。
    const std::string payload = make_payload(2 * 1024 * 1024);
    RangeFileServer server("/v2resume.bin", payload, 25);
    ASSERT_TRUE(server.start());

    const uint16_t port1 = pick_free_port();
    const uint16_t port2 = pick_free_port();
    ASSERT_NE(port1, 0);
    ASSERT_NE(port2, 0);
    TempDirGuard tmp(make_temp_dir());
    const std::string db = tmp.path + "/tasks.db";

    Proc p1;
    ASSERT_TRUE(start_v2_daemon(p1, port1, db));

    const std::string params =
        "[[\"" + server.url() + "\"],{\"dir\":\"" + tmp.path + "\"}]";
    const auto add = http_post(port1, make_rpc_body("aria2.addUri", params));
    ASSERT_TRUE(add.has_value()) << "no addUri response";
    const std::string gid = json_string_field(http_body(*add), "result");
    ASSERT_FALSE(gid.empty()) << *add;

    // 等传输出进度后立即暂停
    const bool started = wait_until([&] {
        const auto st =
            http_post(port1, make_rpc_body("aria2.tellStatus", "[\"" + gid + "\"]"));
        if (!st) return false;
        const std::string body = http_body(*st);
        return contains(body, "\"status\":\"active\"") &&
               json_string_field(body, "completedLength") != "0" &&
               !json_string_field(body, "completedLength").empty();
    }, 10000);
    ASSERT_TRUE(started) << "task never became active with progress";

    const auto pause = http_post(port1, make_rpc_body("aria2.pause", "[\"" + gid + "\"]"));
    ASSERT_TRUE(pause.has_value()) << "pause request failed";
    const bool paused = wait_until([&] {
        const auto st =
            http_post(port1, make_rpc_body("aria2.tellStatus", "[\"" + gid + "\"]"));
        return st && contains(http_body(*st), "\"status\":\"paused\"");
    }, 5000);
    ASSERT_TRUE(paused) << "task never paused";

    // 排水停机（断点固化 + 状态落库），再以 v2 模式重启
    proc_signal(p1, SIGTERM);
    EXPECT_TRUE(proc_finish(p1, 10000));
    EXPECT_EQ(exit_code_of(p1), 0) << p1.err;

    Proc p2;
    ASSERT_TRUE(start_v2_daemon(p2, port2, db));

    const auto un =
        http_post(port2, make_rpc_body("aria2.unpause", "[\"" + gid + "\"]"));
    ASSERT_TRUE(un.has_value()) << "unpause request failed: " << *un;

    const bool complete = wait_until([&] {
        const auto st =
            http_post(port2, make_rpc_body("aria2.tellStatus", "[\"" + gid + "\"]"));
        return st && contains(http_body(*st), "\"status\":\"complete\"");
    }, 30000);
    ASSERT_TRUE(complete) << "resumed task never completed";

    proc_signal(p2, SIGTERM);
    EXPECT_TRUE(proc_finish(p2, 10000));
    EXPECT_EQ(exit_code_of(p2), 0) << p2.err;

    const std::string final_path = tmp.path + "/v2resume.bin";
    EXPECT_EQ(read_file_bytes(final_path), payload);
    EXPECT_FALSE(file_exists(final_path + ".falcon.tmp"));
    EXPECT_FALSE(file_exists(final_path + ".falcon.ctrl"));
}

} // namespace
