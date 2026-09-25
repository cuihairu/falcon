// ============================================================================
// swarm_config_test：swarm.json 配置加载单测（swarmd_config.cpp 全函数面）
//
// 纯文件 + 直调，零网络零竞速。对位参照：daemon 包 config.cpp 的 10 用例
// 矩阵（读取/错误路径/未知键告警/~ 展开）——配置解析是纯逻辑，错误路径
// 全部确定性可达，不留传输层 e2e 覆盖（e2e 只有一条 bad-config 剧本）。
// ============================================================================

#include "swarmd_config.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// 临时目录 RAII（固定名——仅本二进制使用，ctest 并行无跨 target 冲突）
class ConfigFileFixture : public ::testing::Test {
public:
    ConfigFileFixture() {
        std::error_code ec;
        dir_ = fs::temp_directory_path(ec) / "falcon_swarm_config_test";
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~ConfigFileFixture() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ConfigFileFixture(const ConfigFileFixture&) = delete;
    ConfigFileFixture& operator=(const ConfigFileFixture&) = delete;

    // 写入内容并返回文件路径（供 apply_config_file 消费）
    std::string write(const std::string& name, const std::string& content) {
        const auto p = dir_ / name;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << content;
        return p.string();
    }

private:
    fs::path dir_;
};

// 与实现同源读 HOME（POSIX）/ USERPROFILE（Windows）——独立事实源，
// 不经 expand_home_path 自身取值（防自我印证）。UCRT 的 getenv 直接
// 查进程环境块，Windows 上同样可读 USERPROFILE。
std::string env_home() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home ? std::string(home) : std::string();
}

constexpr const char* kFullConfig = R"json({
    "swarm": {
        "host": "0.0.0.0",
        "port": 7901,
        "server_token": "st-secret",
        "group_token": "gt-secret",
        "heartbeat_interval_s": 15,
        "heartbeat_timeout_s": 45,
        "challenge_ttl_s": 30,
        "sweep_interval_ms": 250,
        "rate_register_per_min": 10,
        "rate_query_per_min": 200,
        "blacklist": ["deadbeef", "cafebabe"]
    },
    "daemon": {
        "run_as_daemon": true,
        "pid_file": "~/falcon-swarmd.pid",
        "working_dir": "/tmp",
        "log_file": "/var/log/falcon-swarmd.log"
    }
})json";

}  // namespace

namespace falcon::swarm {

// ---------------------------------------------------------------------------
// 成功路径
// ---------------------------------------------------------------------------

TEST_F(ConfigFileFixture, AllFieldsRoundtrip) {
    SwarmdFileConfig config;
    const auto result = apply_config_file(write("full.json", kFullConfig), config);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_EQ(config.host, "0.0.0.0");
    EXPECT_EQ(config.port, 7901u);
    EXPECT_EQ(config.server_token, "st-secret");
    EXPECT_EQ(config.group_token, "gt-secret");
    EXPECT_EQ(config.heartbeat_interval_s, 15);
    EXPECT_EQ(config.heartbeat_timeout_s, 45);
    EXPECT_EQ(config.challenge_ttl_s, 30);
    EXPECT_EQ(config.sweep_interval_ms, 250);
    EXPECT_EQ(config.rate_register_per_min, 10u);
    EXPECT_EQ(config.rate_query_per_min, 200u);
    ASSERT_EQ(config.blacklist.size(), 2u);
    EXPECT_EQ(config.blacklist[0], "deadbeef");
    EXPECT_EQ(config.blacklist[1], "cafebabe");
    EXPECT_TRUE(config.run_as_daemon);
    EXPECT_EQ(config.working_dir, "/tmp");
    EXPECT_EQ(config.log_file, "/var/log/falcon-swarmd.log");
}

TEST_F(ConfigFileFixture, PathKeysExpandHomePrefix) {
    SwarmdFileConfig config;
    const auto result = apply_config_file(write("paths.json", kFullConfig), config);

    ASSERT_TRUE(result.ok) << result.error;
    const auto home = env_home();
    if (!home.empty()) {
        EXPECT_EQ(config.pid_file, home + "/falcon-swarmd.pid");
    } else {
        // HOME 缺失时展开器原样返回（不发明路径）
        EXPECT_EQ(config.pid_file, "~/falcon-swarmd.pid");
    }
}

TEST_F(ConfigFileFixture, MissingKeysPreserveDefaults) {
    SwarmdFileConfig config;
    // 预置非默认值：键缺省不动目标（optional 语义）
    config.port = 1234;
    config.host = "10.0.0.1";

    const auto result = apply_config_file(write("empty.json", "{}"), config);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_EQ(config.port, 1234u);
    EXPECT_EQ(config.host, "10.0.0.1");
    EXPECT_EQ(config.rate_register_per_min, 5u);
    EXPECT_FALSE(config.run_as_daemon);
}

// ---------------------------------------------------------------------------
// 错误路径（全部确定性）
// ---------------------------------------------------------------------------

TEST_F(ConfigFileFixture, MissingFileReportsError) {
    SwarmdFileConfig config;
    const auto result =
        apply_config_file((fs::temp_directory_path() /
                           "falcon_swarm_config_test" / "nope.json").string(),
                          config);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("cannot open config file"), std::string::npos)
        << result.error;
}

TEST_F(ConfigFileFixture, MalformedJsonReportsError) {
    SwarmdFileConfig config;
    const auto result =
        apply_config_file(write("bad.json", "{not json at all"), config);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("failed to parse config file"), std::string::npos)
        << result.error;
}

TEST_F(ConfigFileFixture, NonObjectRootReportsError) {
    SwarmdFileConfig config;
    const auto result = apply_config_file(write("arr.json", "[1, 2, 3]"), config);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "config file root must be a JSON object");
}

TEST_F(ConfigFileFixture, NonObjectSwarmSectionReportsError) {
    SwarmdFileConfig config;
    const auto result =
        apply_config_file(write("s42.json", R"json({"swarm": 42})json"), config);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "'swarm' section must be an object");
}

TEST_F(ConfigFileFixture, NonObjectDaemonSectionReportsError) {
    SwarmdFileConfig config;
    const auto result =
        apply_config_file(write("dstr.json", R"json({"daemon": "yes"})json"),
                          config);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "'daemon' section must be an object");
}

TEST_F(ConfigFileFixture, TypeErrorsReportKey) {
    // port 传字符串——read_key<uint16_t> 的类型门禁，错误带键名
    SwarmdFileConfig config;
    const auto result =
        apply_config_file(write("tport.json", R"json({"swarm": {"port": "abc"}})json"),
                          config);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "invalid type for key 'port'");
}

TEST_F(ConfigFileFixture, TypeErrorsReportDaemonKey) {
    SwarmdFileConfig config;
    const auto result =
        apply_config_file(write("trad.json",
                                R"json({"daemon": {"run_as_daemon": "yes"}})json"),
                          config);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "invalid type for key 'run_as_daemon'");
}

// ---------------------------------------------------------------------------
// 告警路径（向前兼容：不失败）
// ---------------------------------------------------------------------------

TEST_F(ConfigFileFixture, UnknownSectionWarnsButOk) {
    SwarmdFileConfig config;
    const auto result =
        apply_config_file(write("usec.json",
                                R"json({"swarm": {}, "future_section": {}})json"),
                          config);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_NE(result.warnings[0].find("unknown config section: future_section"),
              std::string::npos)
        << result.warnings[0];
}

TEST_F(ConfigFileFixture, UnknownKeyWarnsButOk) {
    SwarmdFileConfig config;
    const auto result =
        apply_config_file(write("ukey.json",
                                R"json({"swarm": {"port": 7901, "typo_key": 1}})json"),
                          config);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_NE(result.warnings[0].find("unknown key in 'swarm'"), std::string::npos)
        << result.warnings[0];
    EXPECT_NE(result.warnings[0].find("typo_key"), std::string::npos)
        << result.warnings[0];
    EXPECT_EQ(config.port, 7901u);
}

// ---------------------------------------------------------------------------
// expand_home_path 直测
// ---------------------------------------------------------------------------

TEST(ExpandHomePathTest, WithoutPrefixReturnsAsIs) {
    EXPECT_EQ(expand_home_path("/abs/path"), "/abs/path");
    EXPECT_EQ(expand_home_path("relative/x"), "relative/x");
    // 单个 "~" 与 "~user" 不是本展开器的语义（只认 "~/" 前缀），原样返回
    EXPECT_EQ(expand_home_path("~"), "~");
    EXPECT_EQ(expand_home_path("~user/x"), "~user/x");
}

TEST(ExpandHomePathTest, PrefixExpandsToHome) {
    const auto home = env_home();
    if (home.empty()) {
        // HOME 缺失：原样返回（不发明路径）
        EXPECT_EQ(expand_home_path("~/x"), "~/x");
        return;
    }
    EXPECT_EQ(expand_home_path("~/x"), home + "/x");
    EXPECT_EQ(expand_home_path("~/"), home + "/");
}

}  // namespace falcon::swarm
