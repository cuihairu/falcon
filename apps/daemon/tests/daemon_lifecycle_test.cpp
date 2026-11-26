// DaemonManager lifecycle tests.
//
// Part 1 exercises DaemonManager in-process (pid file handling, run loop,
// callbacks, state transitions).
// Part 2 exercises daemonize() + signal handling in forked children so the
// double-fork, setsid, stdio redirection and pid-file creation happen in an
// isolated process. Children finish through std::exit so that gcov data is
// flushed back into the build tree.

#include "daemon/daemon.hpp"

#include <falcon/detail/injection.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kTimeoutMs = 10000;

std::string make_temp_dir() {
    std::string tmpl = ::testing::TempDir() + "/falcon-daemon-lc-XXXXXX";
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

void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = kTimeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// Read exactly `want` bytes from a pipe within the deadline. Never reads
// beyond `want` so surplus bytes stay in the pipe for later readers.
std::optional<std::string> read_pipe(int fd, std::size_t want, int timeout_ms = kTimeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string out;
    char buf[64];
    while (out.size() < want) {
        const auto remain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remain_ms <= 0) break;
        pollfd p{};
        p.fd = fd;
        p.events = POLLIN;
        const int r = ::poll(&p, 1, static_cast<int>(remain_ms));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        const std::size_t want_now = want - out.size() < sizeof(buf) ? want - out.size() : sizeof(buf);
        const ssize_t n = ::read(fd, buf, want_now);
        if (n > 0) {
            out.append(buf, buf + n);
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    if (out.size() >= want) return out.substr(0, want);
    return std::nullopt;
}

void report_byte(int wfd, char c) {
    const ssize_t rc = ::write(wfd, &c, 1);
    (void)rc;
}

// A child process that just sleeps until it is killed (or its safety alarm
// fires). Used as a "live other instance".
pid_t spawn_sleeper() {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::alarm(30);
        for (;;) {
            ::pause();
        }
        ::_exit(0);
    }
    return pid;
}

// A probe forks a child; the child reports bytes through a pipe, the parent
// reads them. In the child `pid == 0` and `wfd` is the pipe write end.
struct Probe {
    pid_t pid = -1;
    int fd = -1;   // read end (parent only)
    int wfd = -1;  // write end (child only)
};

bool spawn_probe(Probe& probe) {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::alarm(30);
        probe.pid = 0;
        probe.wfd = fds[1];
        return true;
    }
    ::close(fds[1]);
    probe.pid = pid;
    probe.fd = fds[0];
    return true;
}

// Child body of DaemonDaemonizeTest.DaemonizeSuccessAndSignals.
// Kept in a function so the local DaemonManager is destroyed on return —
// std::exit() would skip automatic destructors and leak the pid file.
void run_daemonize_success_probe(int wfd, const std::string& pid_file,
                                 const std::string& work_dir) {
    falcon::daemon::DaemonConfig cfg;
    cfg.pid_file = pid_file;
    cfg.working_dir = work_dir;
    cfg.redirect_stdio = false;
    cfg.create_pid_file = true;
    falcon::daemon::DaemonManager dm(cfg);

    const bool ok = dm.daemonize();
    report_byte(wfd, ok ? 'D' : 'F');
    if (ok) {
        report_byte(wfd, dm.read_pid_file() == dm.get_pid() ? 'P' : 'p');

        ::raise(SIGTERM);
        report_byte(wfd, dm.should_stop() ? 'S' : 's');

        // Give the parent a window to observe the pid file.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        ::raise(SIGHUP);
        // SIGHUP 在信号处理器里只置 pending 标志（async-signal-safe），
        // 实际回调由 run() 循环执行——本探针未进入 run()，故断言标志位
        report_byte(wfd, dm.reload_pending() ? 'R' : 'r');
    }
    ::close(wfd);
}

} // namespace

// ===========================================================================
// Helper functions
// ===========================================================================

TEST(DaemonHelpersTest, DefaultPidFileIsNotEmpty) {
    const std::string pid_file = falcon::daemon::get_default_pid_file();
    EXPECT_FALSE(pid_file.empty());
}

TEST(DaemonHelpersTest, DefaultConfigDirIsNotEmpty) {
    const std::string dir = falcon::daemon::get_default_config_dir();
    EXPECT_FALSE(dir.empty());
}

TEST(DaemonHelpersTest, CreateDirectoriesSucceeds) {
    TempDirGuard tmp(make_temp_dir());
    const std::string target = tmp.path + "/a/b/c";
    EXPECT_TRUE(falcon::daemon::create_directories(target));
    EXPECT_TRUE(file_exists(target));
}

TEST(DaemonHelpersTest, CreateDirectoriesFailsUnderRegularFile) {
    TempDirGuard tmp(make_temp_dir());
    const std::string blocker = tmp.path + "/blocker";
    write_file(blocker, "x");
    EXPECT_FALSE(falcon::daemon::create_directories(blocker + "/sub"));
}

// ===========================================================================
// DaemonManager in-process behavior
// ===========================================================================

TEST(DaemonManagerTest, InitialState) {
    falcon::daemon::DaemonManager dm;
    EXPECT_FALSE(dm.is_daemon());
    EXPECT_FALSE(dm.should_stop());
    EXPECT_FALSE(dm.is_running());
    EXPECT_EQ(dm.get_state(), falcon::daemon::DaemonState::NotStarted);
    EXPECT_EQ(dm.get_pid(), static_cast<int>(::getpid()));
    EXPECT_TRUE(dm.get_last_error().empty());
}

TEST(DaemonManagerTest, ReadPidFileVariants) {
    TempDirGuard tmp(make_temp_dir());

    {
        falcon::daemon::DaemonConfig cfg;  // no pid file configured
        falcon::daemon::DaemonManager dm(cfg);
        EXPECT_EQ(dm.read_pid_file(), -1);
    }
    {
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = tmp.path + "/missing.pid";
        falcon::daemon::DaemonManager dm(cfg);
        EXPECT_EQ(dm.read_pid_file(), -1);
    }
    {
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = tmp.path + "/valid.pid";
        write_file(cfg.pid_file, "12345\n");
        falcon::daemon::DaemonManager dm(cfg);
        EXPECT_EQ(dm.read_pid_file(), 12345);
    }
    {
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = tmp.path + "/garbage.pid";
        write_file(cfg.pid_file, "not-a-number");
        falcon::daemon::DaemonManager dm(cfg);
        EXPECT_EQ(dm.read_pid_file(), -1);
    }
    {
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = tmp.path + "/zero.pid";
        write_file(cfg.pid_file, "0");
        falcon::daemon::DaemonManager dm(cfg);
        EXPECT_EQ(dm.read_pid_file(), -1);
    }
    {
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = tmp.path + "/negative.pid";
        write_file(cfg.pid_file, "-5");
        falcon::daemon::DaemonManager dm(cfg);
        EXPECT_EQ(dm.read_pid_file(), -1);
    }
}

TEST(DaemonManagerTest, AnotherInstanceOwnPidIsIgnored) {
    TempDirGuard tmp(make_temp_dir());
    falcon::daemon::DaemonConfig cfg;
    cfg.pid_file = tmp.path + "/self.pid";
    write_file(cfg.pid_file, std::to_string(::getpid()));

    falcon::daemon::DaemonManager dm(cfg);
    EXPECT_FALSE(dm.is_another_instance_running());
    EXPECT_FALSE(dm.stop_another_instance());
}

TEST(DaemonManagerTest, AnotherInstanceMissingFileIsIgnored) {
    TempDirGuard tmp(make_temp_dir());
    falcon::daemon::DaemonConfig cfg;
    cfg.pid_file = tmp.path + "/none.pid";

    falcon::daemon::DaemonManager dm(cfg);
    EXPECT_FALSE(dm.is_another_instance_running());
    EXPECT_FALSE(dm.stop_another_instance());
}

TEST(DaemonManagerTest, AnotherInstanceDeadPidIsIgnored) {
    TempDirGuard tmp(make_temp_dir());
    const pid_t sleeper = spawn_sleeper();
    ASSERT_GT(sleeper, 0);
    ASSERT_EQ(::kill(sleeper, SIGKILL), 0);
    ASSERT_EQ(::waitpid(sleeper, nullptr, 0), sleeper);

    falcon::daemon::DaemonConfig cfg;
    cfg.pid_file = tmp.path + "/dead.pid";
    write_file(cfg.pid_file, std::to_string(sleeper));

    falcon::daemon::DaemonManager dm(cfg);
    EXPECT_FALSE(dm.is_another_instance_running());
    EXPECT_FALSE(dm.stop_another_instance());
}

TEST(DaemonManagerTest, LiveInstanceIsDetectedAndStopped) {
    TempDirGuard tmp(make_temp_dir());
    const pid_t sleeper = spawn_sleeper();
    ASSERT_GT(sleeper, 0);
    ASSERT_TRUE(wait_until([&] { return ::kill(sleeper, 0) == 0; }, 2000));

    falcon::daemon::DaemonConfig cfg;
    cfg.pid_file = tmp.path + "/live.pid";
    write_file(cfg.pid_file, std::to_string(sleeper));

    falcon::daemon::DaemonManager dm(cfg);
    EXPECT_TRUE(dm.is_another_instance_running());

    EXPECT_TRUE(dm.stop_another_instance());
    ASSERT_TRUE(wait_until([&] { return ::waitpid(sleeper, nullptr, WNOHANG) == sleeper; }, 2000));

    EXPECT_FALSE(dm.is_another_instance_running());
}

TEST(DaemonManagerTest, RunLoopInvokesStopCallback) {
    falcon::daemon::DaemonManager dm;
    std::atomic<bool> stopped{false};

    std::thread runner([&] { dm.run([&] { stopped = true; }, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_FALSE(dm.should_stop());

    dm.request_stop();
    runner.join();

    EXPECT_TRUE(stopped.load());
    EXPECT_TRUE(dm.should_stop());
    EXPECT_FALSE(dm.is_running());
    EXPECT_EQ(dm.get_state(), falcon::daemon::DaemonState::Stopped);
}

TEST(DaemonManagerTest, RunLoopWorksWithoutCallbacks) {
    falcon::daemon::DaemonManager dm;
    std::thread runner([&] { dm.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dm.stop();
    runner.join();
    EXPECT_EQ(dm.get_state(), falcon::daemon::DaemonState::Stopped);
}

TEST(DaemonManagerTest, RunLoopStoresReloadCallback) {
    falcon::daemon::DaemonManager dm;
    std::atomic<bool> reloaded{false};

    std::thread runner([&] { dm.run([] {}, [&] { reloaded = true; }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // SIGHUP 路径：request_reload() 只置标志，run() 循环负责执行回调
    dm.request_reload();
    bool fired = false;
    for (int i = 0; i < 100 && !fired; ++i) {
        fired = reloaded.load();
        if (!fired) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(fired);
    EXPECT_FALSE(dm.reload_pending());  // 消费后标志清零
    dm.request_stop();
    runner.join();
}

TEST(DaemonManagerTest, ReloadInvokesCallbackSetViaSetter) {
    falcon::daemon::DaemonManager dm;
    std::atomic<bool> reloaded{false};
    dm.set_reload_callback([&] { reloaded = true; });
    dm.reload();
    EXPECT_TRUE(reloaded.load());
}

TEST(DaemonManagerTest, ReloadWithoutCallbackIsSafe) {
    falcon::daemon::DaemonManager dm;
    dm.reload();  // no callback installed: must not crash
}

TEST(DaemonManagerTest, RunCallbackWinsOverSetter) {
    // run() installs its own stop callback; a callback set earlier via
    // set_stop_callback() is replaced (and therefore never fires).
    falcon::daemon::DaemonManager dm;
    std::atomic<bool> from_setter{false};
    std::atomic<bool> from_run{false};
    dm.set_stop_callback([&] { from_setter = true; });

    std::thread runner([&] { dm.run([&] { from_run = true; }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dm.request_stop();
    runner.join();
    EXPECT_TRUE(from_run.load());
    EXPECT_FALSE(from_setter.load());
}

TEST(DaemonManagerTest, StopTransitionsState) {
    falcon::daemon::DaemonManager dm;
    dm.stop();
    EXPECT_TRUE(dm.should_stop());
    EXPECT_FALSE(dm.is_running());
    EXPECT_EQ(dm.get_state(), falcon::daemon::DaemonState::Stopping);
    dm.request_stop();  // idempotent
    EXPECT_TRUE(dm.should_stop());
}

TEST(DaemonManagerTest, InstanceTracking) {
    EXPECT_EQ(falcon::daemon::DaemonManager::get_instance(), nullptr);
    {
        falcon::daemon::DaemonManager first;
        EXPECT_EQ(falcon::daemon::DaemonManager::get_instance(), &first);
        {
            falcon::daemon::DaemonManager second;
            EXPECT_EQ(falcon::daemon::DaemonManager::get_instance(), &second);
        }
        // Destroying the registered instance clears the global pointer.
        EXPECT_EQ(falcon::daemon::DaemonManager::get_instance(), nullptr);
    }
    EXPECT_EQ(falcon::daemon::DaemonManager::get_instance(), nullptr);
}

TEST(DaemonManagerTest, DestructorKeepsPidFileWhenNotDaemonized) {
    TempDirGuard tmp(make_temp_dir());
    const std::string pid_file = tmp.path + "/keep.pid";
    write_file(pid_file, "1");
    {
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = pid_file;
        falcon::daemon::DaemonManager dm(cfg);
    }
    EXPECT_TRUE(file_exists(pid_file));
}

// ===========================================================================
// daemonize() in isolated child processes
// ===========================================================================

TEST(DaemonDaemonizeTest, DaemonizeSuccessAndSignals) {
    TempDirGuard tmp(make_temp_dir());
    const std::string pid_file = tmp.path + "/nested/run/falcon.pid";

    Probe probe;
    ASSERT_TRUE(spawn_probe(probe));
    if (probe.pid == 0) {
        // === child; after daemonize() this code runs in the daemonized process ===
        run_daemonize_success_probe(probe.wfd, pid_file, tmp.path);
        // run_daemonize_success_probe 返回时局部 DaemonManager 已析构，
        // pid 文件随之删除（std::exit 不会运行自动对象析构，必须依赖作用域）。
        std::exit(0);
    }

    // === parent ===
    const auto first = read_pipe(probe.fd, 1, kTimeoutMs);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ((*first)[0], 'D') << "daemonize failed in child";

    // The daemonized process keeps the pid file alive until it exits.
    ASSERT_TRUE(wait_until([&] { return file_exists(pid_file); }, 5000));
    {
        const auto content = read_file(pid_file);
        ASSERT_TRUE(content.has_value());
        const int recorded = std::atoi(content->c_str());
        EXPECT_GT(recorded, 0);
        EXPECT_NE(recorded, static_cast<int>(::getpid()));
        EXPECT_NE(recorded, static_cast<int>(probe.pid));
    }

    const auto rest = read_pipe(probe.fd, 3, kTimeoutMs);
    ASSERT_TRUE(rest.has_value()) << "child did not finish probe sequence";
    EXPECT_EQ((*rest)[0], 'P');
    EXPECT_EQ((*rest)[1], 'S');
    EXPECT_EQ((*rest)[2], 'R');

    int status = 0;
    ASSERT_EQ(::waitpid(probe.pid, &status, 0), probe.pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    ::close(probe.fd);

    // Grandchild exits normally -> its destructor removes the pid file.
    EXPECT_TRUE(wait_until([&] { return !file_exists(pid_file); }));
}

TEST(DaemonDaemonizeTest, DaemonizeFailsWhenAnotherInstanceRunning) {
    TempDirGuard tmp(make_temp_dir());
    const pid_t sleeper = spawn_sleeper();
    ASSERT_GT(sleeper, 0);
    ASSERT_TRUE(wait_until([&] { return ::kill(sleeper, 0) == 0; }, 2000));

    const std::string pid_file = tmp.path + "/exist.pid";
    write_file(pid_file, std::to_string(sleeper));

    Probe probe;
    ASSERT_TRUE(spawn_probe(probe));
    if (probe.pid == 0) {
        const int wfd = probe.wfd;
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = pid_file;
        cfg.working_dir = tmp.path;
        cfg.redirect_stdio = false;
        cfg.create_pid_file = true;
        falcon::daemon::DaemonManager dm(cfg);
        report_byte(wfd, dm.daemonize() ? 'D' : 'F');
        ::close(wfd);
        std::exit(0);
    }

    const auto res = read_pipe(probe.fd, 1, kTimeoutMs);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ((*res)[0], 'F');
    int status = 0;
    ASSERT_EQ(::waitpid(probe.pid, &status, 0), probe.pid);
    ::close(probe.fd);

    ::kill(sleeper, SIGKILL);
    ::waitpid(sleeper, nullptr, 0);
}

TEST(DaemonDaemonizeTest, DaemonizeFailsWhenPidFileUnwritable) {
    Probe probe;
    ASSERT_TRUE(spawn_probe(probe));
    if (probe.pid == 0) {
        const int wfd = probe.wfd;
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = "/proc/falcon-daemon-cov.pid";  // parent dir exists, file creation fails
        cfg.working_dir = "";                           // exercises default chdir("/")
        cfg.redirect_stdio = true;                      // stdio -> /dev/null
        cfg.create_pid_file = true;
        falcon::daemon::DaemonManager dm(cfg);
        report_byte(wfd, dm.daemonize() ? 'D' : 'F');
        ::close(wfd);
        std::exit(0);
    }

    const auto res = read_pipe(probe.fd, 1, kTimeoutMs);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ((*res)[0], 'F');
    int status = 0;
    ASSERT_EQ(::waitpid(probe.pid, &status, 0), probe.pid);
    ::close(probe.fd);
}

TEST(DaemonDaemonizeTest, DaemonizeFailsWhenPidDirCreationFails) {
    TempDirGuard tmp(make_temp_dir());
    const std::string blocker = tmp.path + "/blocker";
    write_file(blocker, "x");
    const std::string log_file = tmp.path + "/daemon.log";

    Probe probe;
    ASSERT_TRUE(spawn_probe(probe));
    if (probe.pid == 0) {
        const int wfd = probe.wfd;
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = blocker + "/sub/falcon.pid";  // parent dir under a regular file
        cfg.working_dir = tmp.path;
        cfg.redirect_stdio = true;  // stdio -> log file branch
        cfg.log_file = log_file;
        cfg.create_pid_file = true;
        falcon::daemon::DaemonManager dm(cfg);
        report_byte(wfd, dm.daemonize() ? 'D' : 'F');
        ::close(wfd);
        std::exit(0);
    }

    const auto res = read_pipe(probe.fd, 1, kTimeoutMs);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ((*res)[0], 'F');
    int status = 0;
    ASSERT_EQ(::waitpid(probe.pid, &status, 0), probe.pid);
    ::close(probe.fd);

    // The log file was created by redirect_stdio() before the pid-file step.
    EXPECT_TRUE(wait_until([&] { return file_exists(log_file); }, 5000));
}

TEST(DaemonDaemonizeTest, DaemonizeWithoutPidFileHandlesSigint) {
    TempDirGuard tmp(make_temp_dir());

    Probe probe;
    ASSERT_TRUE(spawn_probe(probe));
    if (probe.pid == 0) {
        const int wfd = probe.wfd;
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = tmp.path + "/unused.pid";
        cfg.working_dir = tmp.path;
        cfg.redirect_stdio = false;
        cfg.create_pid_file = false;  // no pid file at all
        falcon::daemon::DaemonManager dm(cfg);
        const bool ok = dm.daemonize();
        report_byte(wfd, ok ? 'D' : 'F');
        if (ok) {
            ::raise(SIGINT);  // Ctrl+C path of the signal handler
            report_byte(wfd, dm.should_stop() ? 'I' : 'i');
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::close(wfd);
        std::exit(0);
    }

    const auto res = read_pipe(probe.fd, 2, kTimeoutMs);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ((*res)[0], 'D');
    EXPECT_EQ((*res)[1], 'I');
    int status = 0;
    ASSERT_EQ(::waitpid(probe.pid, &status, 0), probe.pid);
    ::close(probe.fd);

    EXPECT_FALSE(file_exists(tmp.path + "/unused.pid"));
}

// Injection: the first fork() short-circuits to -1 — no real fork, no
// _exit(0), so daemonize() fails in-process and the error message is
// directly assertable.
TEST(DaemonManagerTest, DaemonizeForkFailFailsCleanly) {
    falcon::daemon::DaemonConfig cfg;
    cfg.redirect_stdio = false;
    falcon::daemon::DaemonManager dm(cfg);
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::DaemonizeForkFail);
    EXPECT_FALSE(dm.daemonize());
    EXPECT_EQ(dm.get_last_error(), "First fork failed");
    EXPECT_FALSE(dm.is_daemon());
}

// Injection: setsid() failure. daemonize() only reaches setsid() inside the
// child of the first fork, so this runs as a probe: the probe process X forks
// Y via daemonize() and is then _exit(0)-ed as the "parent of the first
// fork"; Y is where the injection fires and the failure is collected. The
// 'F' byte therefore comes from Y (which exits immediately — no grandchild
// is left behind). alarm() is cleared across fork, but Y's path is pure
// in-memory work plus one pipe write, so there is nothing to hang on.
TEST(DaemonDaemonizeTest, DaemonizeSetsidFailFailsCleanly) {
    TempDirGuard tmp(make_temp_dir());
    Probe probe;
    ASSERT_TRUE(spawn_probe(probe));
    if (probe.pid == 0) {
        const int wfd = probe.wfd;
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = tmp.path + "/setsid.pid";
        cfg.working_dir = tmp.path;
        cfg.redirect_stdio = false;
        cfg.create_pid_file = false;
        falcon::daemon::DaemonManager dm(cfg);
        ::falcon::detail::ScopedInjection guard(
            ::falcon::detail::InjectPoint::DaemonizeSetsidFail);
        report_byte(wfd, dm.daemonize() ? 'D' : 'F');
        ::close(wfd);
        std::exit(0);
    }

    const auto res = read_pipe(probe.fd, 1, kTimeoutMs);
    ASSERT_TRUE(res.has_value()) << "probe produced no report";
    EXPECT_EQ((*res)[0], 'F');
    int status = 0;
    ASSERT_EQ(::waitpid(probe.pid, &status, 0), probe.pid);
    ::close(probe.fd);
}

// Injection: the second fork fails. Same probe shape as the setsid case —
// X is _exit(0)-ed after the first fork, Y passes setsid (not injected
// here) and collects the second-fork failure.
TEST(DaemonDaemonizeTest, DaemonizeSecondForkFailFailsCleanly) {
    TempDirGuard tmp(make_temp_dir());
    Probe probe;
    ASSERT_TRUE(spawn_probe(probe));
    if (probe.pid == 0) {
        const int wfd = probe.wfd;
        falcon::daemon::DaemonConfig cfg;
        cfg.pid_file = tmp.path + "/fork2.pid";
        cfg.working_dir = tmp.path;
        cfg.redirect_stdio = false;
        cfg.create_pid_file = false;
        falcon::daemon::DaemonManager dm(cfg);
        ::falcon::detail::ScopedInjection guard(
            ::falcon::detail::InjectPoint::DaemonizeFork2Fail);
        report_byte(wfd, dm.daemonize() ? 'D' : 'F');
        ::close(wfd);
        std::exit(0);
    }

    const auto res = read_pipe(probe.fd, 1, kTimeoutMs);
    ASSERT_TRUE(res.has_value()) << "probe produced no report";
    EXPECT_EQ((*res)[0], 'F');
    int status = 0;
    ASSERT_EQ(::waitpid(probe.pid, &status, 0), probe.pid);
    ::close(probe.fd);
}
