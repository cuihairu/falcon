/**
 * @file config_loader_coverage_test.cpp
 * @brief Additional unit tests covering untested branches of config_loader.cpp
 */

#include <gtest/gtest.h>

#ifdef FALCON_USE_JSON

#include "config_loader.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::filesystem::path make_unique_test_dir(const char* prefix) {
    const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long long>(::getpid());
#endif

    std::string name = prefix;
    if (test_info) {
        name += "_";
        name += test_info->test_suite_name();
        name += "_";
        name += test_info->name();
    }
    name += "_";
    name += std::to_string(pid);
    name += "_";
    name += std::to_string(static_cast<unsigned long long>(stamp));

    return std::filesystem::temp_directory_path() / name;
}

class ConfigLoaderCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = make_unique_test_dir("falcon_cli_cov");
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    std::filesystem::path write_config(const std::string& name, const std::string& content) {
        const auto path = test_dir_ / name;
        std::ofstream of(path);
        of << content;
        return path;
    }

    std::filesystem::path test_dir_;
};

} // namespace

// ============================================================================
// Full-field JSON parsing
// ============================================================================

TEST_F(ConfigLoaderCoverageTest, LoadConfigWithAllFieldsPopulated) {
    const auto path = write_config("full.json", R"json({
        "max_connections": 16,
        "max_concurrent_downloads": 4,
        "timeout_seconds": 90,
        "max_retries": 7,
        "retry_delay_seconds": 11,
        "default_download_dir": "/tmp/falcon-dl",
        "default_output_filename": "renamed.bin",
        "speed_limit": 2097152,
        "min_segment_size": 4194304,
        "resume_enabled": false,
        "adaptive_segment_sizing": false,
        "verify_ssl": false,
        "user_agent": "FullAgent/2.0",
        "proxy": "socks5://proxy:1080",
        "proxy_type": "socks5",
        "proxy_username": "puser",
        "proxy_password": "ppass",
        "referer": "https://referer.example/",
        "cookie_file": "cookies.in",
        "cookie_jar": "cookies.out",
        "http_username": "huser",
        "http_password": "hpass",
        "headers": {"X-One": "1", "X-Two": "2"},
        "enable_rpc": true,
        "rpc_listen_port": 7001,
        "rpc_secret": "topsecret",
        "rpc_allow_origin_all": true,
        "verbose": true,
        "quiet": true,
        "log_level": "debug",
        "show_progress": false,
        "use_head": true,
        "conditional_download": true,
        "auto_renaming": true,
        "create_directory": false,
        "overwrite_existing": true
    })json");

    auto config = falcon::cli::ConfigLoader::load(path.string());
    ASSERT_TRUE(config.has_value());

    EXPECT_EQ(16, config->max_connections);
    EXPECT_EQ(4, config->max_concurrent_downloads);
    EXPECT_EQ(90, config->timeout_seconds);
    EXPECT_EQ(7, config->max_retries);
    EXPECT_EQ(11, config->retry_delay_seconds);
    EXPECT_EQ("/tmp/falcon-dl", config->default_download_dir);
    EXPECT_EQ("renamed.bin", config->default_output_filename);
    EXPECT_EQ(2097152ULL, config->speed_limit);
    EXPECT_EQ(4194304ULL, config->min_segment_size);
    EXPECT_FALSE(config->resume_enabled);
    EXPECT_FALSE(config->adaptive_segment_sizing);
    EXPECT_FALSE(config->verify_ssl);
    EXPECT_EQ("FullAgent/2.0", config->user_agent);
    EXPECT_EQ("socks5://proxy:1080", config->proxy);
    EXPECT_EQ("socks5", config->proxy_type);
    EXPECT_EQ("puser", config->proxy_username);
    EXPECT_EQ("ppass", config->proxy_password);
    EXPECT_EQ("https://referer.example/", config->referer);
    EXPECT_EQ("cookies.in", config->cookie_file);
    EXPECT_EQ("cookies.out", config->cookie_jar);
    EXPECT_EQ("huser", config->http_username);
    EXPECT_EQ("hpass", config->http_password);
    EXPECT_EQ(2ULL, config->headers.size());
    EXPECT_EQ("1", config->headers.at("X-One"));
    EXPECT_EQ("2", config->headers.at("X-Two"));
    EXPECT_TRUE(config->enable_rpc);
    EXPECT_EQ(7001, config->rpc_listen_port);
    EXPECT_EQ("topsecret", config->rpc_secret);
    EXPECT_TRUE(config->rpc_allow_origin_all);
    EXPECT_TRUE(config->verbose);
    EXPECT_TRUE(config->quiet);
    EXPECT_EQ("debug", config->log_level);
    EXPECT_FALSE(config->show_progress);
    EXPECT_TRUE(config->use_head);
    EXPECT_TRUE(config->conditional_download);
    EXPECT_TRUE(config->auto_renaming);
    EXPECT_FALSE(config->create_directory);
    EXPECT_TRUE(config->overwrite_existing);
}

TEST_F(ConfigLoaderCoverageTest, LoadEmptyJsonObjectKeepsDefaults) {
    const auto path = write_config("empty.json", "{}");

    auto config = falcon::cli::ConfigLoader::load(path.string());
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(4, config->max_connections);
    EXPECT_EQ(30, config->timeout_seconds);
    EXPECT_TRUE(config->resume_enabled);
    EXPECT_TRUE(config->verify_ssl);
    EXPECT_EQ("Falcon/0.2.0", config->user_agent);
    EXPECT_TRUE(config->headers.empty());
}

TEST_F(ConfigLoaderCoverageTest, LoadBlankFileReportsParseError) {
    const auto path = write_config("blank.json", "");

    auto config = falcon::cli::ConfigLoader::load(path.string());
    EXPECT_FALSE(config.has_value());
    EXPECT_FALSE(falcon::cli::ConfigLoader::get_last_error().empty());
}

TEST_F(ConfigLoaderCoverageTest, LoadTypeMismatchReportsParseError) {
    const auto path = write_config("mismatch.json",
                                   R"json({"timeout_seconds": "not-a-number"})json");

    auto config = falcon::cli::ConfigLoader::load(path.string());
    EXPECT_FALSE(config.has_value());
    EXPECT_FALSE(falcon::cli::ConfigLoader::get_last_error().empty());
}

// ============================================================================
// Save round-trips
// ============================================================================

TEST_F(ConfigLoaderCoverageTest, SaveConfigWithAllOptionalFieldsAndReload) {
    falcon::cli::CliConfig config;
    config.max_connections = 32;
    config.max_concurrent_downloads = 3;
    config.timeout_seconds = 77;
    config.max_retries = 4;
    config.retry_delay_seconds = 6;
    config.default_download_dir = "/tmp/save-dl";
    config.default_output_filename = "saved.bin";
    config.speed_limit = 1048576;
    config.min_segment_size = 8388608;
    config.resume_enabled = false;
    config.adaptive_segment_sizing = false;
    config.verify_ssl = false;
    config.user_agent = "SaveAgent/9.0";
    config.proxy = "http://save-proxy:1";
    config.proxy_type = "http";
    config.proxy_username = "spu";
    config.proxy_password = "spp";
    config.referer = "https://save.example/ref";
    config.cookie_file = "save.in";
    config.cookie_jar = "save.out";
    config.http_username = "shu";
    config.http_password = "shp";
    config.headers["X-Save"] = "yes";
    config.enable_rpc = true;
    config.rpc_listen_port = 7100;
    config.rpc_secret = "save-secret";
    config.rpc_allow_origin_all = true;
    config.verbose = true;
    config.quiet = true;
    config.log_level = "trace";
    config.show_progress = false;
    config.use_head = true;
    config.conditional_download = true;
    config.auto_renaming = true;
    config.create_directory = false;
    config.overwrite_existing = true;

    const auto path = test_dir_ / "full-save.json";
    EXPECT_TRUE(falcon::cli::ConfigLoader::save(config, path.string()));

    auto loaded = falcon::cli::ConfigLoader::load(path.string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(32, loaded->max_connections);
    EXPECT_EQ(3, loaded->max_concurrent_downloads);
    EXPECT_EQ(77, loaded->timeout_seconds);
    EXPECT_EQ(4, loaded->max_retries);
    EXPECT_EQ(6, loaded->retry_delay_seconds);
    EXPECT_EQ("/tmp/save-dl", loaded->default_download_dir);
    EXPECT_EQ("saved.bin", loaded->default_output_filename);
    EXPECT_EQ(1048576ULL, loaded->speed_limit);
    EXPECT_EQ(8388608ULL, loaded->min_segment_size);
    EXPECT_FALSE(loaded->resume_enabled);
    EXPECT_FALSE(loaded->adaptive_segment_sizing);
    EXPECT_FALSE(loaded->verify_ssl);
    EXPECT_EQ("SaveAgent/9.0", loaded->user_agent);
    EXPECT_EQ("http://save-proxy:1", loaded->proxy);
    EXPECT_EQ("http", loaded->proxy_type);
    EXPECT_EQ("spu", loaded->proxy_username);
    EXPECT_EQ("spp", loaded->proxy_password);
    EXPECT_EQ("https://save.example/ref", loaded->referer);
    EXPECT_EQ("save.in", loaded->cookie_file);
    EXPECT_EQ("save.out", loaded->cookie_jar);
    EXPECT_EQ("shu", loaded->http_username);
    EXPECT_EQ("shp", loaded->http_password);
    EXPECT_EQ("yes", loaded->headers.at("X-Save"));
    EXPECT_TRUE(loaded->enable_rpc);
    EXPECT_EQ(7100, loaded->rpc_listen_port);
    EXPECT_EQ("save-secret", loaded->rpc_secret);
    EXPECT_TRUE(loaded->rpc_allow_origin_all);
    EXPECT_TRUE(loaded->verbose);
    EXPECT_TRUE(loaded->quiet);
    EXPECT_EQ("trace", loaded->log_level);
    EXPECT_FALSE(loaded->show_progress);
    EXPECT_TRUE(loaded->use_head);
    EXPECT_TRUE(loaded->conditional_download);
    EXPECT_TRUE(loaded->auto_renaming);
    EXPECT_FALSE(loaded->create_directory);
    EXPECT_TRUE(loaded->overwrite_existing);
}

TEST_F(ConfigLoaderCoverageTest, SaveMinimalConfigOmitsOptionalFields) {
    falcon::cli::CliConfig config;  // all defaults

    const auto path = test_dir_ / "minimal-save.json";
    EXPECT_TRUE(falcon::cli::ConfigLoader::save(config, path.string()));

    auto loaded = falcon::cli::ConfigLoader::load(path.string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(4, loaded->max_connections);
    EXPECT_TRUE(loaded->default_download_dir.empty());
    EXPECT_TRUE(loaded->proxy.empty());
    EXPECT_TRUE(loaded->headers.empty());
    EXPECT_FALSE(loaded->enable_rpc);
    EXPECT_EQ(6800, loaded->rpc_listen_port);
}

TEST_F(ConfigLoaderCoverageTest, SaveFailsWhenParentPathComponentIsFile) {
    const auto blocker = test_dir_ / "blocker";
    {
        std::ofstream of(blocker);
        of << "regular file";
    }

    falcon::cli::CliConfig config;
    const auto bad_path = blocker / "sub" / "config.json";
    EXPECT_FALSE(falcon::cli::ConfigLoader::save(config, bad_path.string()));
    EXPECT_NE(falcon::cli::ConfigLoader::get_last_error().find("directory"),
              std::string::npos);
}

// ============================================================================
// Default config creation
// ============================================================================

TEST_F(ConfigLoaderCoverageTest, CreateDefaultConfigWithExplicitPath) {
    const auto path = test_dir_ / "fresh" / "nested" / "config.json";
    EXPECT_TRUE(falcon::cli::ConfigLoader::create_default_config(path.string()));
    EXPECT_TRUE(std::filesystem::exists(path));

    auto loaded = falcon::cli::ConfigLoader::load(path.string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(4, loaded->max_connections);
    EXPECT_TRUE(loaded->resume_enabled);
}

#ifndef _WIN32

namespace {

class ScopedEnvOverride {
public:
    ScopedEnvOverride(const std::string& name, const std::string& value)
        : name_(name), had_old_(::getenv(name.c_str()) != nullptr) {
        if (had_old_) {
            old_value_ = ::getenv(name.c_str());
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ScopedEnvOverride(const std::string& name)  // unset variant
        : name_(name), had_old_(::getenv(name.c_str()) != nullptr) {
        if (had_old_) {
            old_value_ = ::getenv(name.c_str());
        }
        ::unsetenv(name_.c_str());
    }

    ~ScopedEnvOverride() {
        if (had_old_) {
            ::setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    bool had_old_;
    std::string old_value_;
};

class ScopedChdir {
public:
    explicit ScopedChdir(const std::filesystem::path& target) {
        original_ = std::filesystem::current_path();
        std::error_code ec;
        std::filesystem::current_path(target, ec);
        ok_ = !ec;
    }

    ~ScopedChdir() {
        std::error_code ec;
        std::filesystem::current_path(original_, ec);
    }

    bool ok() const { return ok_; }

private:
    std::filesystem::path original_;
    bool ok_ = false;
};

} // namespace

TEST_F(ConfigLoaderCoverageTest, CreateDefaultConfigUsesHomeEnvironment) {
    const auto home = test_dir_ / "home-root";
    std::filesystem::create_directories(home);
    ScopedEnvOverride home_override("HOME", home.string());

    EXPECT_TRUE(falcon::cli::ConfigLoader::create_default_config(""));
    const auto created = home / ".config" / "falcon" / "config.json";
    EXPECT_TRUE(std::filesystem::exists(created));

    auto loaded = falcon::cli::ConfigLoader::load(created.string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((home / "Downloads").string(), loaded->default_download_dir);
}

TEST_F(ConfigLoaderCoverageTest, ConfigDirAndPathFollowHomeEnvironment) {
    const auto home = test_dir_ / "home-alt";
    ScopedEnvOverride home_override("HOME", home.string());

    EXPECT_EQ((home / ".config" / "falcon").string(),
              falcon::cli::ConfigLoader::get_config_dir());
    EXPECT_EQ((home / ".config" / "falcon" / "config.json").string(),
              falcon::cli::ConfigLoader::get_default_config_path());

    const auto paths = falcon::cli::ConfigLoader::get_config_search_paths();
    ASSERT_GE(paths.size(), 3ULL);
    EXPECT_EQ("./falcon.json", paths[0]);
    EXPECT_EQ((home / ".config" / "falcon" / "config.json").string(), paths[1]);
    EXPECT_EQ("/etc/falcon/config.json", paths[2]);
}

TEST_F(ConfigLoaderCoverageTest, ConfigDirFallsBackToPasswdWithoutHome) {
    ScopedEnvOverride home_unset("HOME");
    const auto dir = falcon::cli::ConfigLoader::get_config_dir();
    EXPECT_FALSE(dir.empty());
}

TEST_F(ConfigLoaderCoverageTest, SearchFindsConfigInCurrentDirectory) {
    const auto cwd_holder = make_unique_test_dir("falcon_cli_cov_cwd");
    std::filesystem::create_directories(cwd_holder);
    {
        std::ofstream of(cwd_holder / "falcon.json");
        of << R"json({"timeout_seconds": 42})json";
    }
    const auto home_holder = make_unique_test_dir("falcon_cli_cov_home");
    std::filesystem::create_directories(home_holder);

    ScopedChdir chdir_guard(cwd_holder);
    ASSERT_TRUE(chdir_guard.ok());
    ScopedEnvOverride home_override("HOME", home_holder.string());

    auto config = falcon::cli::ConfigLoader::load("");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(42, config->timeout_seconds);
}

TEST_F(ConfigLoaderCoverageTest, SearchFallsThroughToEnvironmentConfig) {
    const auto cwd_holder = make_unique_test_dir("falcon_cli_cov_cwd2");
    std::filesystem::create_directories(cwd_holder);
    const auto home_holder = make_unique_test_dir("falcon_cli_cov_home2");
    std::filesystem::create_directories(home_holder);

    ScopedChdir chdir_guard(cwd_holder);
    ASSERT_TRUE(chdir_guard.ok());
    ScopedEnvOverride home_override("HOME", home_holder.string());
    ScopedEnvOverride max_conn("FALCON_MAX_CONNECTIONS", "12");
    ScopedEnvOverride proxy("FALCON_PROXY", "http://env-proxy:9");
    ScopedEnvOverride user_agent("FALCON_USER_AGENT", "EnvAgent/3.0");
    ScopedEnvOverride download_dir("FALCON_DOWNLOAD_DIR", "/tmp/env-dl");
    ScopedEnvOverride rpc_secret("FALCON_RPC_SECRET", "env-secret");
    ScopedEnvOverride log_level("FALCON_LOG_LEVEL", "warn");

    auto config = falcon::cli::ConfigLoader::load("");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(12, config->max_connections);
    EXPECT_EQ("http://env-proxy:9", config->proxy);
    EXPECT_EQ("EnvAgent/3.0", config->user_agent);
    EXPECT_EQ("/tmp/env-dl", config->default_download_dir);
    EXPECT_EQ("env-secret", config->rpc_secret);
    EXPECT_EQ("warn", config->log_level);
}

TEST_F(ConfigLoaderCoverageTest, SearchWithNoConfigAnywhereReturnsNullopt) {
    const auto cwd_holder = make_unique_test_dir("falcon_cli_cov_cwd3");
    std::filesystem::create_directories(cwd_holder);
    const auto home_holder = make_unique_test_dir("falcon_cli_cov_home3");
    std::filesystem::create_directories(home_holder);

    ScopedChdir chdir_guard(cwd_holder);
    ASSERT_TRUE(chdir_guard.ok());
    ScopedEnvOverride home_override("HOME", home_holder.string());
    ScopedEnvOverride unset_conn("FALCON_MAX_CONNECTIONS");
    ScopedEnvOverride unset_proxy("FALCON_PROXY");
    ScopedEnvOverride unset_agent("FALCON_USER_AGENT");
    ScopedEnvOverride unset_dir("FALCON_DOWNLOAD_DIR");
    ScopedEnvOverride unset_rpc("FALCON_RPC_SECRET");
    ScopedEnvOverride unset_log("FALCON_LOG_LEVEL");

    auto config = falcon::cli::ConfigLoader::load("");
    EXPECT_FALSE(config.has_value());

    auto defaulted = falcon::cli::ConfigLoader::load_or_default("");
    EXPECT_EQ(4, defaulted.max_connections);
    EXPECT_EQ(30, defaulted.timeout_seconds);
}

#endif // !_WIN32

// ============================================================================
// Validation
// ============================================================================

TEST_F(ConfigLoaderCoverageTest, ValidateCollectsAllBoundaryErrors) {
    falcon::cli::CliConfig bad;
    bad.max_connections = 0;
    bad.max_concurrent_downloads = 65;
    bad.timeout_seconds = -1;
    bad.max_retries = 101;
    bad.rpc_listen_port = 70000;
    bad.proxy = "http://some-proxy:1";  // with empty proxy_type: no extra error

    const auto errors = falcon::cli::ConfigLoader::validate(bad);
    EXPECT_EQ(5ULL, errors.size());
}

TEST_F(ConfigLoaderCoverageTest, ValidateAcceptsBoundaryValues) {
    falcon::cli::CliConfig good;
    good.max_connections = 64;
    good.max_concurrent_downloads = 64;
    good.timeout_seconds = 0;
    good.max_retries = 100;
    good.rpc_listen_port = 65535;
    good.proxy = "http://some-proxy:1";

    EXPECT_TRUE(falcon::cli::ConfigLoader::validate(good).empty());
}

// ============================================================================
// to_download_options full mapping
// ============================================================================

TEST_F(ConfigLoaderCoverageTest, ToDownloadOptionsMapsEveryField) {
    falcon::cli::CliConfig config;
    config.max_connections = 6;
    config.max_concurrent_downloads = 2;
    config.timeout_seconds = 55;
    config.max_retries = 9;
    config.retry_delay_seconds = 8;
    config.default_download_dir = "/map/dir";
    config.default_output_filename = "mapped.bin";
    config.speed_limit = 2048;
    config.min_segment_size = 4096;
    config.resume_enabled = false;
    config.adaptive_segment_sizing = false;
    config.verify_ssl = false;
    config.user_agent = "MapAgent/1.0";
    config.proxy = "http://map-proxy:2";
    config.proxy_type = "http";
    config.proxy_username = "mpu";
    config.proxy_password = "mpp";
    config.referer = "https://map.example/";
    config.cookie_file = "map.in";
    config.cookie_jar = "map.out";
    config.http_username = "mhu";
    config.http_password = "mhp";
    config.headers["X-Map"] = "1";
    config.create_directory = false;
    config.overwrite_existing = true;

    const auto opts = config.to_download_options();
    EXPECT_EQ(6ULL, opts.max_connections);
    EXPECT_EQ(55ULL, opts.timeout_seconds);
    EXPECT_EQ(9ULL, opts.max_retries);
    EXPECT_EQ(8ULL, opts.retry_delay_seconds);
    EXPECT_EQ("/map/dir", opts.output_directory);
    EXPECT_EQ("mapped.bin", opts.output_filename);
    EXPECT_EQ(2048ULL, opts.speed_limit);
    EXPECT_EQ(4096ULL, opts.min_segment_size);
    EXPECT_FALSE(opts.resume_enabled);
    EXPECT_FALSE(opts.adaptive_segment_sizing);
    EXPECT_FALSE(opts.verify_ssl);
    EXPECT_EQ("MapAgent/1.0", opts.user_agent);
    EXPECT_EQ("http://map-proxy:2", opts.proxy);
    EXPECT_EQ("http", opts.proxy_type);
    EXPECT_EQ("mpu", opts.proxy_username);
    EXPECT_EQ("mpp", opts.proxy_password);
    EXPECT_EQ("https://map.example/", opts.referer);
    EXPECT_EQ("map.in", opts.cookie_file);
    EXPECT_EQ("map.out", opts.cookie_jar);
    EXPECT_EQ("mhu", opts.http_username);
    EXPECT_EQ("mhp", opts.http_password);
    EXPECT_EQ("1", opts.headers.at("X-Map"));
    EXPECT_FALSE(opts.create_directory);
    EXPECT_TRUE(opts.overwrite_existing);
}

// ============================================================================
// merge_configs full coverage
// ============================================================================

TEST_F(ConfigLoaderCoverageTest, MergeConfigsAppliesEveryCliOverride) {
    falcon::cli::CliConfig file_config;
    file_config.max_connections = 2;
    file_config.max_concurrent_downloads = 1;
    file_config.timeout_seconds = 10;
    file_config.max_retries = 1;
    file_config.default_download_dir = "/file/dir";
    file_config.default_output_filename = "file.bin";
    file_config.speed_limit = 1;
    file_config.user_agent = "FileAgent/1.0";
    file_config.proxy = "http://file-proxy:1";
    file_config.proxy_type = "http";
    file_config.proxy_username = "fpu";
    file_config.proxy_password = "fpp";
    file_config.referer = "https://file.example/";
    file_config.cookie_file = "file.in";
    file_config.cookie_jar = "file.out";
    file_config.http_username = "fhu";
    file_config.http_password = "fhp";
    file_config.headers["X-File"] = "keep";
    file_config.rpc_secret = "file-secret";
    file_config.rpc_listen_port = 6801;
    file_config.resume_enabled = true;
    file_config.verify_ssl = true;
    file_config.enable_rpc = false;
    file_config.rpc_allow_origin_all = false;
    file_config.show_progress = true;

    falcon::cli::CliConfig cli_args;
    cli_args.max_connections = 24;              // != default 4 -> override
    cli_args.max_concurrent_downloads = 5;      // != default 1 -> override
    cli_args.timeout_seconds = 66;              // != default 30 -> override
    cli_args.max_retries = 8;                   // != default 3 -> override
    cli_args.default_download_dir = "/cli/dir";
    cli_args.default_output_filename = "cli.bin";
    cli_args.speed_limit = 4096;
    cli_args.user_agent = "Falcon/0.2.0";       // default value -> NOT applied
    cli_args.proxy = "http://cli-proxy:2";
    cli_args.proxy_type = "socks5";
    cli_args.proxy_username = "cpu";
    cli_args.proxy_password = "cpp";
    cli_args.referer = "https://cli.example/";
    cli_args.cookie_file = "cli.in";
    cli_args.cookie_jar = "cli.out";
    cli_args.http_username = "chu";
    cli_args.http_password = "chp";
    cli_args.headers["X-File"] = "overridden";
    cli_args.headers["X-Cli"] = "added";
    cli_args.rpc_secret = "cli-secret";
    cli_args.rpc_listen_port = 6802;
    cli_args.verbose = true;
    cli_args.quiet = true;
    cli_args.resume_enabled = false;
    cli_args.verify_ssl = false;
    cli_args.enable_rpc = true;
    cli_args.rpc_allow_origin_all = true;

    const auto merged = falcon::cli::merge_configs(file_config, cli_args);
    EXPECT_EQ(24, merged.max_connections);
    EXPECT_EQ(5, merged.max_concurrent_downloads);
    EXPECT_EQ(66, merged.timeout_seconds);
    EXPECT_EQ(8, merged.max_retries);
    EXPECT_EQ("/cli/dir", merged.default_download_dir);
    EXPECT_EQ("cli.bin", merged.default_output_filename);
    EXPECT_EQ(4096ULL, merged.speed_limit);
    EXPECT_EQ("FileAgent/1.0", merged.user_agent);  // default CLI agent ignored
    EXPECT_EQ("http://cli-proxy:2", merged.proxy);
    EXPECT_EQ("socks5", merged.proxy_type);
    EXPECT_EQ("cpu", merged.proxy_username);
    EXPECT_EQ("cpp", merged.proxy_password);
    EXPECT_EQ("https://cli.example/", merged.referer);
    EXPECT_EQ("cli.in", merged.cookie_file);
    EXPECT_EQ("cli.out", merged.cookie_jar);
    EXPECT_EQ("chu", merged.http_username);
    EXPECT_EQ("chp", merged.http_password);
    EXPECT_EQ("overridden", merged.headers.at("X-File"));
    EXPECT_EQ("added", merged.headers.at("X-Cli"));
    EXPECT_EQ("cli-secret", merged.rpc_secret);
    EXPECT_EQ(6802, merged.rpc_listen_port);
    EXPECT_TRUE(merged.verbose);
    EXPECT_TRUE(merged.quiet);
    EXPECT_FALSE(merged.show_progress);  // quiet implies no progress
    EXPECT_FALSE(merged.resume_enabled);
    EXPECT_FALSE(merged.verify_ssl);
    EXPECT_TRUE(merged.enable_rpc);
    EXPECT_TRUE(merged.rpc_allow_origin_all);
}

TEST_F(ConfigLoaderCoverageTest, MergeConfigsKeepsFileValuesWithDefaultCliArgs) {
    falcon::cli::CliConfig file_config;
    file_config.max_connections = 9;
    file_config.timeout_seconds = 21;
    file_config.max_retries = 6;
    file_config.user_agent = "FileKept/1.0";
    file_config.rpc_listen_port = 6900;
    file_config.resume_enabled = true;
    file_config.verify_ssl = true;
    file_config.show_progress = true;

    falcon::cli::CliConfig cli_args;  // all defaults

    const auto merged = falcon::cli::merge_configs(file_config, cli_args);
    EXPECT_EQ(9, merged.max_connections);
    EXPECT_EQ(21, merged.timeout_seconds);
    EXPECT_EQ(6, merged.max_retries);
    EXPECT_EQ("FileKept/1.0", merged.user_agent);
    EXPECT_EQ(6900, merged.rpc_listen_port);
    EXPECT_TRUE(merged.resume_enabled);
    EXPECT_TRUE(merged.verify_ssl);
    EXPECT_TRUE(merged.show_progress);
}

#endif // FALCON_USE_JSON
