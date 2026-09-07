/**
 * @file cli_main_integration_test.cpp
 * @brief Integration tests that exercise the real falcon-cli binary (POSIX only)
 *
 * Runs the actual executable via fork()+execve so that the --coverage
 * instrumentation of main.cpp is taken into account. The child processes are
 * never terminated with SIGKILL; they either exit on their own or receive
 * SIGTERM (which the CLI handles gracefully) so gcda data is still flushed.
 */

#if !defined(_WIN32)

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

std::string unique_suffix() {
    static std::atomic<unsigned long> counter{0};
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? info->name() : "test";
    name += "_";
    name += std::to_string(static_cast<unsigned long>(::getpid()));
    name += "_";
    name += std::to_string(counter.fetch_add(1UL));
    return name;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/**
 * @brief 打开一个“停滞”TCP 监听器（127.0.0.1 上的临时端口）
 *
 * listen() 后永不 accept()：内核 backlog 完成 TCP 握手，但没有任何数据
 * 返回给客户端。用于模拟「连接成功但永远不响应」的服务器，使下载任务
 * 保持运行状态，从而可以测试 SIGINT / 交互按键取消路径。
 * 连接拒绝（127.0.0.1:1）会立即失败，无法用于此类测试。
 *
 * @param port_out 输出绑定的端口号
 * @return 监听 fd（调用方负责 close），失败返回 -1
 */
int open_stall_listener(uint16_t& port_out) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;  // 临时端口
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port_out = ntohs(bound.sin_port);
    return fd;
}

class CliMainIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Writing stdin to an exited child must not kill the test process.
        std::signal(SIGPIPE, SIG_IGN);

        work_dir_ = std::filesystem::temp_directory_path() /
                    ("falcon_cli_it_" + unique_suffix());
        home_dir_ = work_dir_ / "home";
        out_dir_ = work_dir_ / "downloads";
        std::filesystem::create_directories(home_dir_);
        std::filesystem::create_directories(out_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(work_dir_, ec);
    }

    std::filesystem::path work_dir_;
    std::filesystem::path home_dir_;
    std::filesystem::path out_dir_;

    // ------------------------------------------------------------------
    // Child process helpers
    // ------------------------------------------------------------------

    [[noreturn]] void exec_child(const std::vector<std::string>& args,
                                 int stdin_fd,
                                 int stdout_fd,
                                 int stderr_fd) const {
        ::setsid();
        if (::dup2(stdin_fd, STDIN_FILENO) < 0) ::_exit(126);
        if (::dup2(stdout_fd, STDOUT_FILENO) < 0) ::_exit(126);
        if (::dup2(stderr_fd, STDERR_FILENO) < 0) ::_exit(126);

        // Close the original descriptors (they may alias each other for a pty).
        if (stdin_fd > STDERR_FILENO) ::close(stdin_fd);
        if (stdout_fd > STDERR_FILENO && stdout_fd != stdin_fd) ::close(stdout_fd);
        if (stderr_fd > STDERR_FILENO && stderr_fd != stdin_fd && stderr_fd != stdout_fd) {
            ::close(stderr_fd);
        }

        if (::chdir(work_dir_.c_str()) != 0) ::_exit(126);

        std::vector<std::string> env_strings{
            "HOME=" + home_dir_.string(),
            "PATH=/usr/local/bin:/usr/bin:/bin",
            "TERM=dumb",
            "LANG=C.UTF-8",
        };

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("falcon-cli"));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        std::vector<char*> envp;
        for (auto& e : env_strings) {
            envp.push_back(const_cast<char*>(e.c_str()));
        }
        envp.push_back(nullptr);

        ::execve(FALCON_CLI_BIN, argv.data(), envp.data());
        ::_exit(127);
    }

    // ------------------------------------------------------------------
    // Pipe based runner
    // ------------------------------------------------------------------

    CliRunResult run_cli(const std::vector<std::string>& args,
                         int timeout_seconds = kDefaultTimeoutSeconds,
                         const std::string& stdin_data = "",
                         long signal_after_ms = -1,
                         int signal_number = SIGINT) {
        CliRunResult result;

        int in_pipe[2];
        int out_pipe[2];
        int err_pipe[2];
        if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
            ADD_FAILURE() << "pipe() failed: " << std::strerror(errno);
            return result;
        }

        // Mark every raw pipe fd close-on-exec. The child dup2()s the ends it
        // needs onto 0/1/2 (dup2 clears CLOEXEC on the target), so after
        // execve() all unused ends — especially the stdin *write* end — are
        // closed in the child. Without this the child keeps in_pipe[1] open
        // and a `-i -` read from stdin never sees EOF, hanging the test.
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                       err_pipe[0], err_pipe[1]}) {
            ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
        }

        ::pid_t pid = ::fork();
        if (pid < 0) {
            ADD_FAILURE() << "fork() failed: " << std::strerror(errno);
            return result;
        }

        if (pid == 0) {
            exec_child(args, in_pipe[0], out_pipe[1], err_pipe[1]);
        }

        // Parent: close child-side descriptors.
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);

        if (!stdin_data.empty()) {
            std::size_t written = 0;
            while (written < stdin_data.size()) {
                const ssize_t n = ::write(in_pipe[1], stdin_data.data() + written,
                                          stdin_data.size() - written);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;  // child already exited: fine
                }
                written += static_cast<std::size_t>(n);
            }
        }
        ::close(in_pipe[1]);

        set_nonblocking(out_pipe[0]);
        set_nonblocking(err_pipe[0]);

        bool out_open = true;
        bool err_open = true;
        bool signalled = false;
        const auto start = Clock::now();
        const auto deadline = start + std::chrono::seconds(timeout_seconds);
        const auto signal_at =
            start + std::chrono::milliseconds(signal_after_ms < 0 ? 0 : signal_after_ms);

        while (out_open || err_open) {
            const auto now = Clock::now();

            if (!signalled && signal_after_ms >= 0 && now >= signal_at) {
                ::kill(pid, signal_number);
                signalled = true;
            }
            if (now >= deadline) {
                result.timed_out = true;
                ::kill(pid, SIGTERM);
                break;
            }

            struct ::pollfd pfds[2];
            int count = 0;
            int out_idx = -1;
            int err_idx = -1;
            if (out_open) {
                out_idx = count;
                pfds[count].fd = out_pipe[0];
                pfds[count].events = POLLIN;
                pfds[count].revents = 0;
                ++count;
            }
            if (err_open) {
                err_idx = count;
                pfds[count].fd = err_pipe[0];
                pfds[count].events = POLLIN;
                pfds[count].revents = 0;
                ++count;
            }

            const int ready = ::poll(pfds, static_cast<nfds_t>(count), 50);
            if (ready > 0) {
                if (out_idx >= 0 && (pfds[out_idx].revents & (POLLIN | POLLHUP))) {
                    drain_fd(pfds[out_idx].fd, result.out, out_open);
                }
                if (err_idx >= 0 && (pfds[err_idx].revents & (POLLIN | POLLHUP))) {
                    drain_fd(pfds[err_idx].fd, result.err, err_open);
                }
            } else if (ready < 0 && errno != EINTR) {
                break;
            }
        }

        if (result.timed_out) {
            // Grace period: keep draining so the report contains diagnostics.
            const auto grace_deadline = Clock::now() + std::chrono::seconds(3);
            while ((out_open || err_open) && Clock::now() < grace_deadline) {
                struct ::pollfd pfds[2];
                int count = 0;
                int out_idx = -1;
                int err_idx = -1;
                if (out_open) {
                    out_idx = count;
                    pfds[count].fd = out_pipe[0];
                    pfds[count].events = POLLIN;
                    pfds[count].revents = 0;
                    ++count;
                }
                if (err_open) {
                    err_idx = count;
                    pfds[count].fd = err_pipe[0];
                    pfds[count].events = POLLIN;
                    pfds[count].revents = 0;
                    ++count;
                }
                if (::poll(pfds, static_cast<nfds_t>(count), 200) > 0) {
                    if (out_idx >= 0 && (pfds[out_idx].revents & (POLLIN | POLLHUP))) {
                        drain_fd(pfds[out_idx].fd, result.out, out_open);
                    }
                    if (err_idx >= 0 && (pfds[err_idx].revents & (POLLIN | POLLHUP))) {
                        drain_fd(pfds[err_idx].fd, result.err, err_open);
                    }
                }
            }
        }

        ::close(out_pipe[0]);
        ::close(err_pipe[0]);

        reap(pid, result, timeout_seconds);
        return result;
    }

    // ------------------------------------------------------------------
    // PTY based runner (stdout is a terminal => interactive code paths)
    // ------------------------------------------------------------------

    CliRunResult run_cli_interactive(const std::vector<std::string>& args,
                                     const std::string& keys,
                                     long send_after_ms,
                                     int timeout_seconds = kDefaultTimeoutSeconds) {
        CliRunResult result;

        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0) {
            ADD_FAILURE() << "posix_openpt() failed";
            return result;
        }
        if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            ADD_FAILURE() << "grantpt/unlockpt failed";
            ::close(master);
            return result;
        }
        const char* slave_name = ::ptsname(master);
        if (slave_name == nullptr) {
            ADD_FAILURE() << "ptsname failed";
            ::close(master);
            return result;
        }

        ::pid_t pid = ::fork();
        if (pid < 0) {
            ADD_FAILURE() << "fork() failed";
            ::close(master);
            return result;
        }

        if (pid == 0) {
            const int slave = ::open(slave_name, O_RDWR);
            if (slave < 0) ::_exit(126);
            ::close(master);
            exec_child(args, slave, slave, slave);
        }

        set_nonblocking(master);

        bool sent = false;
        const auto start = Clock::now();
        const auto deadline = start + std::chrono::seconds(timeout_seconds);
        const auto send_at = start + std::chrono::milliseconds(send_after_ms);
        bool eof = false;

        while (!eof) {
            const auto now = Clock::now();
            if (!sent && now >= send_at) {
                std::size_t written = 0;
                while (written < keys.size()) {
                    const ssize_t n = ::write(master, keys.data() + written,
                                              keys.size() - written);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    written += static_cast<std::size_t>(n);
                }
                sent = true;
            }
            if (now >= deadline) {
                result.timed_out = true;
                ::kill(pid, SIGTERM);
                break;
            }

            struct ::pollfd pfd;
            pfd.fd = master;
            pfd.events = POLLIN;
            pfd.revents = 0;
            const int ready = ::poll(&pfd, 1, 50);
            if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
                char buf[4096];
                for (;;) {
                    const ssize_t n = ::read(master, buf, sizeof(buf));
                    if (n > 0) {
                        result.out.append(buf, static_cast<std::size_t>(n));
                        continue;
                    }
                    if (n == 0) {
                        eof = true;
                        break;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    // EIO: slave side fully closed.
                    eof = true;
                    break;
                }
            } else if (ready < 0 && errno != EINTR) {
                break;
            }
        }

        ::close(master);
        reap(pid, result, timeout_seconds);
        return result;
    }

private:
    static void set_nonblocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    static void drain_fd(int fd, std::string& sink, bool& open_flag) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                sink.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                open_flag = false;
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            open_flag = false;
            return;
        }
    }

    static void reap(::pid_t pid, CliRunResult& result, int timeout_seconds) {
        int status = 0;
        bool reaped = false;
        // Grace proportional to the timeout; never SIGKILL before exhausting it.
        const int attempts = max_int(50, timeout_seconds * 5);
        for (int i = 0; i < attempts && !reaped; ++i) {
            const ::pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                reaped = true;
                break;
            }
            if (r < 0 && errno == ECHILD) return;
            // Extra nudges in case the child hangs.
            if (i == attempts / 2) ::kill(pid, SIGTERM);
            ::usleep(100 * 1000);
        }
        if (!reaped) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            result.timed_out = true;
        }

        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
        }
    }

    static int max_int(int a, int b) { return a > b ? a : b; }
};

// ============================================================================
// Help / Version
// ============================================================================

TEST_F(CliMainIntegrationTest, HelpLongOptionExitsZero) {
    auto r = run_cli({"--help"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_FALSE(r.timed_out);
    EXPECT_TRUE(contains(r.out, "Falcon CLI v0.2.0"));
    EXPECT_TRUE(contains(r.out, "Usage:"));
    EXPECT_TRUE(contains(r.out, "--min-split-size"));
}

TEST_F(CliMainIntegrationTest, HelpShortOptionExitsZero) {
    auto r = run_cli({"-h"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "Usage:"));
}

TEST_F(CliMainIntegrationTest, VersionLongOptionExitsZero) {
    auto r = run_cli({"--version"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "Falcon CLI"));
    EXPECT_TRUE(contains(r.out, "v0.2.0"));
}

TEST_F(CliMainIntegrationTest, VersionShortOptionExitsZero) {
    auto r = run_cli({"-V"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "v0.2.0"));
}

// ============================================================================
// Missing / invalid arguments
// ============================================================================

TEST_F(CliMainIntegrationTest, NoArgumentsReportsMissingUrl) {
    auto r = run_cli({});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "missing URL"));
    EXPECT_TRUE(contains(r.err, "Use --help"));
}

TEST_F(CliMainIntegrationTest, UnknownOptionIsNotTreatedAsUrl) {
    auto r = run_cli({"--definitely-not-a-known-option"});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "missing URL"));
}

TEST_F(CliMainIntegrationTest, MissingInputFileFails) {
    auto r = run_cli({"-i", (work_dir_ / "no-such-urls.txt").string()});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "cannot read URLs from"));
    EXPECT_TRUE(contains(r.err, "missing URL"));
}

TEST_F(CliMainIntegrationTest, OutputOptionRejectedForBatchDownload) {
    auto r = run_cli({"-o", "out.bin", kRefusedUrlA, kRefusedUrlB});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "not supported for batch downloads"));
}

TEST_F(CliMainIntegrationTest, UnsupportedUrlSchemeFailsToAddTask) {
    auto r = run_cli({"falcon-unknown-scheme://example.invalid/file.bin"});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "cannot add download tasks"));
}

// ============================================================================
// Config related subcommands
// ============================================================================

TEST_F(CliMainIntegrationTest, ShowConfigPathListsSearchPaths) {
    auto r = run_cli({"--show-config-path"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "Config search paths"));
    EXPECT_TRUE(contains(r.out, "Default config path:"));
    EXPECT_TRUE(contains(r.out, home_dir_.string()));
}

TEST_F(CliMainIntegrationTest, CreateDefaultConfigWritesFileUnderHome) {
    auto r = run_cli({"--create-default-config"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(contains(r.out, "Default config created at:"));

    const auto created = home_dir_ / ".config" / "falcon" / "config.json";
    EXPECT_TRUE(std::filesystem::exists(created));
}

TEST_F(CliMainIntegrationTest, CreateDefaultConfigFailsOnUnwritablePath) {
    auto r = run_cli({"--create-default-config", "-C", "/proc/falcon-it-readonly/config.json"});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "Failed to create default config"));
}

// ============================================================================
// Download error paths (immediately-refused local addresses only)
// ============================================================================

std::vector<std::string> fast_fail_prefix(const std::filesystem::path& out_dir) {
    return {"-d", out_dir.string(), "-r", "0", "--retry-wait", "0", "-t", "2"};
}

std::vector<std::string> fast_fail_args(const std::filesystem::path& out_dir,
                                        const char* url) {
    auto args = fast_fail_prefix(out_dir);
    args.emplace_back(url);
    return args;
}

TEST_F(CliMainIntegrationTest, SingleDownloadConnectionRefusedFails) {
    auto r = run_cli(fast_fail_args(out_dir_, kRefusedUrlA));
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_FALSE(r.timed_out);
    EXPECT_TRUE(contains(r.out, "Downloading:"));
    EXPECT_TRUE(contains(r.out, kRefusedUrlA));
    EXPECT_TRUE(contains(r.err, "FAIL"));
}

TEST_F(CliMainIntegrationTest, BatchDownloadConnectionRefusedFails) {
    auto args = fast_fail_args(out_dir_, kRefusedUrlA);
    args.push_back(kRefusedUrlB);
    args.push_back("-j");
    args.push_back("2");

    auto r = run_cli(args);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.out, "Downloading 2 tasks"));
    EXPECT_TRUE(contains(r.out, "Waiting..."));
    // 每个失败任务在 stderr 打印 "FAIL <url>"，汇总 "N FAILED" 在 stdout
    EXPECT_TRUE(contains(r.err, "FAIL"));
    EXPECT_TRUE(contains(r.out, "2 FAILED"));
}

TEST_F(CliMainIntegrationTest, QuietModeSuppressesProgressOutput) {
    auto args = fast_fail_args(out_dir_, kRefusedUrlA);
    args.push_back("-q");

    auto r = run_cli(args);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_FALSE(contains(r.out, "Downloading:"));
    EXPECT_TRUE(contains(r.err, "FAIL"));
}

TEST_F(CliMainIntegrationTest, InputFileUrlsAreUsedForDownload) {
    const auto url_file = work_dir_ / "urls.txt";
    {
        std::ofstream of(url_file);
        of << "# comment line\n";
        of << kRefusedUrlA << "\n";
        of << "\n";
        of << kRefusedUrlB << "\n";
    }

    auto args = fast_fail_prefix(out_dir_);
    args.push_back("-i");
    args.push_back(url_file.string());

    auto r = run_cli(args);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.out, "Downloading 2 tasks"));
    // 每个失败任务在 stderr 打印 "FAIL <url>"，汇总 "N FAILED" 在 stdout
    EXPECT_TRUE(contains(r.err, "FAIL"));
    EXPECT_TRUE(contains(r.out, "2 FAILED"));
}

TEST_F(CliMainIntegrationTest, StdinInputUrlsAreUsedForDownload) {
    auto args = fast_fail_prefix(out_dir_);
    args.push_back("-i");
    args.push_back("-");

    auto r = run_cli(args, kDefaultTimeoutSeconds,
                     std::string("# note\n") + kRefusedUrlA + "\n\n");
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "FAIL"));
}

TEST_F(CliMainIntegrationTest, ExplicitConfigFileIsLoadedAndMerged) {
    const auto cfg = work_dir_ / "explicit-config.json";
    {
        std::ofstream of(cfg);
        of << "{\n";
        of << "  \"timeout_seconds\": 15,\n";
        of << "  \"max_retries\": 1,\n";
        of << "  \"max_connections\": 2,\n";
        of << "  \"user_agent\": \"FalconIt/1.0\",\n";
        of << "  \"verbose\": true,\n";
        of << "  \"verify_ssl\": true\n";
        of << "}\n";
    }

    auto args = fast_fail_args(out_dir_, kRefusedUrlA);
    args.push_back("-C");
    args.push_back(cfg.string());

    auto r = run_cli(args);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "FAIL"));
}

TEST_F(CliMainIntegrationTest, ConfigFromHomeDirectoryIsUsed) {
    const auto cfg_dir = home_dir_ / ".config" / "falcon";
    std::filesystem::create_directories(cfg_dir);
    {
        std::ofstream of(cfg_dir / "config.json");
        of << "{\n";
        of << "  \"timeout_seconds\": 12,\n";
        of << "  \"user_agent\": \"HomeCfg/1.0\",\n";
        of << "  \"proxy\": \"\"\n";
        of << "}\n";
    }

    auto r = run_cli(fast_fail_args(out_dir_, kRefusedUrlA));
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_TRUE(contains(r.err, "FAIL"));
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

} // namespace

#endif // !defined(_WIN32)
