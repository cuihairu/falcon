// Integration tests for falcon-mcp's main() CLI arg parsing (POSIX only).
// Spawns the real binary via fork()+execve so --coverage counters of
// tools/falcon-mcp.cpp are recorded. Children exit on their own (never
// SIGKILLed) so gcda is flushed.

#if !defined(_WIN32)

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef FALCON_MCP_BIN
#error "FALCON_MCP_BIN must be provided by tests/CMakeLists.txt"
#endif

namespace {

struct ProcResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string out;
    std::string err;
};

ProcResult run_mcp(const std::vector<std::string>& args, int timeout_ms = 8000) {
    int out_pipe[2], err_pipe[2];
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        return {.timed_out = true};
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) ::close(fd);
        return {.timed_out = true};
    }
    if (pid == 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        if (out_pipe[1] > STDERR_FILENO) ::close(out_pipe[1]);
        if (err_pipe[1] > STDERR_FILENO) ::close(err_pipe[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(FALCON_MCP_BIN));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(FALCON_MCP_BIN, argv.data());
        ::_exit(127);
    }
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    ::fcntl(out_pipe[0], F_SETFL, ::fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(err_pipe[0], F_SETFL, ::fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);

    ProcResult r;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            exited = true;
            if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        r.timed_out = true;
    }
    // Drain remaining output.
    char buf[4096];
    for (int fd : {out_pipe[0], err_pipe[0]}) {
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            (fd == out_pipe[0] ? r.out : r.err).append(buf, static_cast<std::size_t>(n));
        }
        ::close(fd);
    }
    return r;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

TEST(McpMainIntegrationTest, HelpExitsZeroAndPrintsUsage) {
    auto r = run_mcp({"--help"});
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "falcon-mcp"));
    EXPECT_TRUE(contains(r.out, "--rpc-url"));
}

TEST(McpMainIntegrationTest, ShortHelpExitsZero) {
    auto r = run_mcp({"-h"});
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
}

TEST(McpMainIntegrationTest, VersionExitsZero) {
    auto r = run_mcp({"--version"});
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "falcon-mcp"));
}

TEST(McpMainIntegrationTest, UnknownArgumentExitsTwo) {
    auto r = run_mcp({"--bogus"});
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_TRUE(contains(r.err, "--bogus")) << r.err;
}

TEST(McpMainIntegrationTest, RpcUrlMissingValueExitsTwo) {
    auto r = run_mcp({"--rpc-url"});
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_TRUE(contains(r.err, "需要一个值")) << r.err;
}

TEST(McpMainIntegrationTest, SecretMissingValueExitsTwo) {
    auto r = run_mcp({"--secret"});
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 2);
}

TEST(McpMainIntegrationTest, RpcUrlAndSecretAcceptedThenRuns) {
    // With a URL + secret, the bridge tries to connect to the daemon; the
    // daemon is absent, so the bridge reads EOF from stdin and exits 0.
    // Feed empty stdin to avoid hanging.
    auto r = run_mcp({"--rpc-url", "http://127.0.0.1:1", "--secret", "tok"});
    // exit 0 because stdin EOF immediately closes session best-effort.
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 0);
}

} // namespace
#endif  // !defined(_WIN32)
