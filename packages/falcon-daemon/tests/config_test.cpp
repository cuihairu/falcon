// daemon.json 配置文件加载测试。
// 覆盖：全量/部分键应用、缺省保持、文件与 JSON 错误、类型错误、
// 未知节/未知键告警、~ 路径展开、默认路径拼接。

#include "daemon/config.hpp"
#include "rpc/json_rpc_server.hpp"  // JsonRpcServerConfig 完整类型

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

/// 写临时配置文件并返回路径（getpid 区分并行分片的测试进程）
std::string write_config(const std::string& content) {
    static int counter = 0;
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(::getpid());
#endif
    const std::string path = ::testing::TempDir() +
        "falcon-config-" + std::to_string(pid) + "-" +
        std::to_string(counter++) + ".json";
    std::ofstream out(path);
    out << content;
    return path;
}

struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
};

/// 设置 home 环境变量（POSIX: HOME / Windows: USERPROFILE），返回原值
std::string set_home_env(const std::string& value) {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
    std::string old = n > 0 ? std::string(buf, n) : std::string();
    SetEnvironmentVariableA("USERPROFILE", value.c_str());
#else
    const char* old = std::getenv("HOME");
    std::string old_str = old ? old : "";
    setenv("HOME", value.c_str(), 1);
#endif
    return old;
}

void restore_home_env(const std::string& value) {
    if (value.empty()) return;
    set_home_env(value);
}

struct AllConfigs {
    falcon::daemon::rpc::JsonRpcServerConfig rpc;
    falcon::daemon::DaemonConfig daemon;
    falcon::daemon::DownloadConfig download;
    std::string task_db_path;
    bool enable_rpc = false;
    bool run_as_daemon = false;
};

falcon::daemon::ConfigLoadResult load(const std::string& path, AllConfigs& c) {
    return falcon::daemon::apply_config_file(path, c.rpc, c.daemon,
                                             c.task_db_path, c.enable_rpc,
                                             c.run_as_daemon, c.download);
}

TEST(ConfigTest, ApplyFullConfig) {
    const TempFile file(write_config(R"({
        "rpc": {
            "enabled": true,
            "host": "0.0.0.0",
            "port": 6900,
            "secret": "file-secret",
            "allow_origin_all": true
        },
        "daemon": {
            "run_as_daemon": true,
            "pid_file": "/tmp/falcon-test.pid",
            "working_dir": "/tmp",
            "log_file": "/tmp/falcon-test.log"
        },
        "storage": {
            "task_db_path": "/tmp/falcon-tasks.db"
        },
        "download": {
            "max_concurrent_tasks": 5,
            "max_overall_speed_limit": 1048576,
            "http_engine": "v2"
        }
    })"));

    AllConfigs c;
    const auto result = load(file.path, c);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.warnings.empty());

    EXPECT_TRUE(c.enable_rpc);
    EXPECT_EQ(c.rpc.bind_address, "0.0.0.0");
    EXPECT_EQ(c.rpc.listen_port, 6900);
    EXPECT_EQ(c.rpc.secret, "file-secret");
    EXPECT_TRUE(c.rpc.allow_origin_all);

    EXPECT_TRUE(c.run_as_daemon);
    EXPECT_EQ(c.daemon.pid_file, "/tmp/falcon-test.pid");
    // pid_file 显式给出即创建（与 --pid-file 一致）
    EXPECT_TRUE(c.daemon.create_pid_file);
    EXPECT_EQ(c.daemon.working_dir, "/tmp");
    EXPECT_EQ(c.daemon.log_file, "/tmp/falcon-test.log");

    EXPECT_EQ(c.task_db_path, "/tmp/falcon-tasks.db");

    ASSERT_TRUE(c.download.max_concurrent_tasks.has_value());
    EXPECT_EQ(*c.download.max_concurrent_tasks, 5u);
    ASSERT_TRUE(c.download.max_overall_speed_limit.has_value());
    EXPECT_EQ(*c.download.max_overall_speed_limit, 1048576u);
    EXPECT_EQ(c.download.http_engine, "v2");
}

TEST(ConfigTest, DownloadSectionOptionalSemantics) {
    const TempFile file(write_config(R"({
        "download": { "max_concurrent_tasks": 3 }
    })"));

    AllConfigs c;
    // 预置另一个键已有值：文件未出现的键必须保持原样（不动引擎默认）
    c.download.max_overall_speed_limit = 42u;

    const auto result = load(file.path, c);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(c.download.max_concurrent_tasks.has_value());
    EXPECT_EQ(*c.download.max_concurrent_tasks, 3u);
    ASSERT_TRUE(c.download.max_overall_speed_limit.has_value());
    EXPECT_EQ(*c.download.max_overall_speed_limit, 42u);
}

TEST(ConfigTest, DownloadSectionAbsentKeepsNullopt) {
    const TempFile file(write_config(R"({ "rpc": { "port": 6802 } })"));

    AllConfigs c;
    const auto result = load(file.path, c);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(c.download.max_concurrent_tasks.has_value());
    EXPECT_FALSE(c.download.max_overall_speed_limit.has_value());
    // 未出现的键保持默认：HTTP 数据面引擎默认 v1
    EXPECT_EQ(c.download.http_engine, "v1");
}

TEST(ConfigTest, HttpEngineV1Explicit) {
    const TempFile file(write_config(
        R"({ "download": { "http_engine": "v1" } })"));

    AllConfigs c;
    const auto result = load(file.path, c);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_EQ(c.download.http_engine, "v1");
}

TEST(ConfigTest, HttpEngineInvalidValueWarnsAndIgnores) {
    const TempFile file(write_config(
        R"({ "download": { "http_engine": "curl" } })"));

    AllConfigs c;
    // 预置调用方现值：非法值告警忽略，保持现值不覆盖
    c.download.http_engine = "v2";

    const auto result = load(file.path, c);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_NE(result.warnings[0].find("download.http_engine"), std::string::npos)
        << result.warnings[0];
    EXPECT_EQ(c.download.http_engine, "v2");
}

TEST(ConfigTest, DownloadSectionTypeMismatchFails) {
    const TempFile file(write_config(R"({
        "download": { "max_concurrent_tasks": "three" }
    })"));

    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'max_concurrent_tasks'"),
              std::string::npos);
}

TEST(ConfigTest, PartialConfigKeepsValues) {
    const TempFile file(write_config(R"({
        "rpc": { "port": 7001 }
    })"));

    AllConfigs c;
    c.rpc.listen_port = 12345;  // 模拟"已设置"的状态
    c.rpc.secret = "keep-me";
    c.daemon.pid_file = "/keep/me.pid";
    c.task_db_path = "/keep/tasks.db";

    const auto result = load(file.path, c);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(c.rpc.listen_port, 7001);  // 文件中出现的键覆盖
    EXPECT_EQ(c.rpc.secret, "keep-me");  // 未出现的键保持
    EXPECT_FALSE(c.enable_rpc);
    EXPECT_EQ(c.daemon.pid_file, "/keep/me.pid");
    EXPECT_EQ(c.task_db_path, "/keep/tasks.db");
}

TEST(ConfigTest, MissingFileFails) {
    AllConfigs c;
    const auto result =
        load(::testing::TempDir() + "falcon-config-does-not-exist.json", c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("cannot open"), std::string::npos);
}

TEST(ConfigTest, InvalidJsonFails) {
    const TempFile file(write_config("{not valid json"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("failed to parse"), std::string::npos);
}

TEST(ConfigTest, RootNotObjectFails) {
    const TempFile file(write_config("[1, 2, 3]"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("root must be a JSON object"), std::string::npos);
}

TEST(ConfigTest, SectionNotObjectFails) {
    const TempFile file(write_config(R"({ "rpc": 5 })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("'rpc' section must be an object"),
              std::string::npos);
}

TEST(ConfigTest, TypeMismatchFails) {
    const TempFile file(write_config(R"({ "rpc": { "port": "not-a-number" } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'port'"),
              std::string::npos);
}

TEST(ConfigTest, UnknownSectionAndKeysWarn) {
    const TempFile file(write_config(R"({
        "unknown_section": {},
        "rpc": { "port": 6801, "prot": 123 },
        "storage": { "task_db_pat": "/x" }
    })"));

    AllConfigs c;
    const auto result = load(file.path, c);
    ASSERT_TRUE(result.ok) << result.error;
    // 节级告警先收集（root 遍历），键级告警随后按节处理顺序出现
    ASSERT_EQ(result.warnings.size(), 3u);
    EXPECT_NE(result.warnings[0].find("unknown config section: unknown_section"),
              std::string::npos);
    EXPECT_NE(result.warnings[1].find("unknown key in 'rpc' section: prot"),
              std::string::npos) << result.warnings[1];
    EXPECT_NE(result.warnings[2].find("unknown key in 'storage' section: task_db_pat"),
              std::string::npos) << result.warnings[2];
}

TEST(ConfigTest, ExpandHomePath) {
    const std::string old_home = set_home_env("/home/falcon-user");
    const auto expanded = falcon::daemon::expand_home_path("~/downloads");
    restore_home_env(old_home);
    EXPECT_EQ(expanded, "/home/falcon-user/downloads");
}

TEST(ConfigTest, ExpandHomePathWithoutPrefix) {
    EXPECT_EQ(falcon::daemon::expand_home_path("/var/lib/falcon"),
              "/var/lib/falcon");
    EXPECT_EQ(falcon::daemon::expand_home_path("relative/path"),
              "relative/path");
}

TEST(ConfigTest, TaskDbPathExpanded) {
    const std::string old_home = set_home_env("/home/falcon-user");
    const TempFile file(
        write_config(R"({ "storage": { "task_db_path": "~/tasks.db" } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    restore_home_env(old_home);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(c.task_db_path, "/home/falcon-user/tasks.db");
}

TEST(ConfigTest, DefaultConfigFileUnderConfigDir) {
    const auto path = falcon::daemon::get_default_config_file();
    EXPECT_EQ(path, falcon::daemon::get_default_config_dir() + "/daemon.json");
}

//==============================================================================
// 批次 S：非 rpc 节的「节非 object」与各节键类型错误
//（此前只测过 rpc 节的这两种失败形态）
//==============================================================================

TEST(ConfigTest, DaemonSectionNotObjectFails) {
    const TempFile file(write_config(R"({ "daemon": 5 })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("'daemon' section must be an object"),
              std::string::npos);
}

TEST(ConfigTest, StorageSectionNotObjectFails) {
    const TempFile file(write_config(R"({ "storage": "x" })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("'storage' section must be an object"),
              std::string::npos);
}

TEST(ConfigTest, DownloadSectionNotObjectFails) {
    const TempFile file(write_config(R"({ "download": [] })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("'download' section must be an object"),
              std::string::npos);
}

TEST(ConfigTest, RpcAllowOriginAllTypeMismatchFails) {
    const TempFile file(write_config(R"({ "rpc": { "allow_origin_all": "yes" } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'allow_origin_all'"),
              std::string::npos);
}

TEST(ConfigTest, DaemonPidFileTypeMismatchFails) {
    const TempFile file(write_config(R"({ "daemon": { "pid_file": 123 } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'pid_file'"),
              std::string::npos);
}

TEST(ConfigTest, DaemonWorkingDirTypeMismatchFails) {
    const TempFile file(write_config(R"({ "daemon": { "working_dir": true } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'working_dir'"),
              std::string::npos);
}

TEST(ConfigTest, DaemonLogFileTypeMismatchFails) {
    const TempFile file(write_config(R"({ "daemon": { "log_file": [] } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'log_file'"),
              std::string::npos);
}

TEST(ConfigTest, StorageTaskDbTypeMismatchFails) {
    const TempFile file(write_config(R"({ "storage": { "task_db_path": 42 } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'task_db_path'"),
              std::string::npos);
}

TEST(ConfigTest, DownloadSpeedLimitTypeMismatchFails) {
    const TempFile file(
        write_config(R"({ "download": { "max_overall_speed_limit": "fast" } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'max_overall_speed_limit'"),
              std::string::npos);
}

TEST(ConfigTest, DownloadHttpEngineTypeMismatchFails) {
    const TempFile file(write_config(R"({ "download": { "http_engine": 7 } })"));
    AllConfigs c;
    const auto result = load(file.path, c);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid type for key 'http_engine'"),
              std::string::npos);
}

} // namespace
