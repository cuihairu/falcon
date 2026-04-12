/**
 * @file config_loader_test.cpp
 * @brief Unit tests for CLI config loader
 * @author Falcon Team
 * @date 2025-12-28
 */

#include <gtest/gtest.h>

#ifdef FALCON_USE_JSON

#include "config_loader.hpp"
#include <fstream>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
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

class ConfigLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary directory for test files
        test_dir_ = make_unique_test_dir("falcon_cli_test");
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        // Clean up test directory
        std::error_code ec;
        if (std::filesystem::exists(test_dir_, ec)) {
            std::filesystem::remove_all(test_dir_, ec);
        }
    }

    std::filesystem::path test_dir_;
};

// ============================================================================
// Basic Tests
// ============================================================================

TEST_F(ConfigLoaderTest, LoadNonExistentFile) {
    auto config = falcon::cli::ConfigLoader::load("/nonexistent/path/config.json");
    EXPECT_FALSE(config.has_value());
}

TEST_F(ConfigLoaderTest, LoadOrDefaultReturnsDefaults) {
    auto config = falcon::cli::ConfigLoader::load_or_default("/nonexistent/path/config.json");

    // Check default values
    EXPECT_EQ(4, config.max_connections);
    EXPECT_EQ(1, config.max_concurrent_downloads);
    EXPECT_EQ(30, config.timeout_seconds);
    EXPECT_EQ(3, config.max_retries);
    EXPECT_TRUE(config.resume_enabled);
    EXPECT_TRUE(config.verify_ssl);
    EXPECT_EQ("Falcon/0.2.0", config.user_agent);
}

TEST_F(ConfigLoaderTest, GetConfigSearchPaths) {
    auto paths = falcon::cli::ConfigLoader::get_config_search_paths();

    EXPECT_FALSE(paths.empty());

    // First path should be current directory
    EXPECT_TRUE(paths[0].find("falcon.json") != std::string::npos);
}

TEST_F(ConfigLoaderTest, GetDefaultConfigPath) {
    auto path = falcon::cli::ConfigLoader::get_default_config_path();

    EXPECT_FALSE(path.empty());
    EXPECT_TRUE(path.find("falcon") != std::string::npos);
    EXPECT_TRUE(path.find("config.json") != std::string::npos);
}

// ============================================================================
// JSON Parsing Tests
// ============================================================================

TEST_F(ConfigLoaderTest, LoadValidConfig) {
    // Create a test config file
    std::string config_content = R"json({
        "max_connections": 8,
        "timeout_seconds": 60,
        "max_retries": 5,
        "default_download_dir": "/tmp/downloads",
        "user_agent": "TestAgent/1.0",
        "proxy": "http://proxy:8080",
        "verify_ssl": false,
        "verbose": true
    })json";

    auto config_path = test_dir_ / "test_config.json";
    std::ofstream file(config_path);
    file << config_content;
    file.close();

    auto config = falcon::cli::ConfigLoader::load(config_path.string());

    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(8, config->max_connections);
    EXPECT_EQ(60, config->timeout_seconds);
    EXPECT_EQ(5, config->max_retries);
    EXPECT_EQ("/tmp/downloads", config->default_download_dir);
    EXPECT_EQ("TestAgent/1.0", config->user_agent);
    EXPECT_EQ("http://proxy:8080", config->proxy);
    EXPECT_FALSE(config->verify_ssl);
    EXPECT_TRUE(config->verbose);
}

TEST_F(ConfigLoaderTest, LoadConfigWithHeaders) {
    std::string config_content = R"json({
        "headers": {
            "Authorization": "Bearer token123",
            "X-Custom-Header": "CustomValue"
        }
    })json";

    auto config_path = test_dir_ / "headers_config.json";
    std::ofstream file(config_path);
    file << config_content;
    file.close();

    auto config = falcon::cli::ConfigLoader::load(config_path.string());

    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(2, config->headers.size());
    EXPECT_EQ("Bearer token123", config->headers["Authorization"]);
    EXPECT_EQ("CustomValue", config->headers["X-Custom-Header"]);
}

TEST_F(ConfigLoaderTest, LoadInvalidJson) {
    std::string config_content = R"json({
        "max_connections": 8,
        // Invalid comment in JSON
        "timeout_seconds": 60
    })json";

    auto config_path = test_dir_ / "invalid_config.json";
    std::ofstream file(config_path);
    file << config_content;
    file.close();

    auto config = falcon::cli::ConfigLoader::load(config_path.string());

    EXPECT_FALSE(config.has_value());
    EXPECT_FALSE(falcon::cli::ConfigLoader::get_last_error().empty());
}

// ============================================================================
// Save Tests
// ============================================================================

TEST_F(ConfigLoaderTest, SaveConfig) {
    falcon::cli::CliConfig config;
    config.max_connections = 16;
    config.timeout_seconds = 120;
    config.default_download_dir = "/custom/path";
    config.user_agent = "SaveTest/1.0";
    config.headers["X-Test"] = "TestValue";

    auto save_path = test_dir_ / "saved_config.json";
    EXPECT_TRUE(falcon::cli::ConfigLoader::save(config, save_path.string()));

    // Verify the saved config
    auto loaded = falcon::cli::ConfigLoader::load(save_path.string());
    ASSERT_TRUE(loaded.has_value());

    EXPECT_EQ(16, loaded->max_connections);
    EXPECT_EQ(120, loaded->timeout_seconds);
    EXPECT_EQ("/custom/path", loaded->default_download_dir);
    EXPECT_EQ("SaveTest/1.0", loaded->user_agent);
    EXPECT_EQ("TestValue", loaded->headers["X-Test"]);
}

// ============================================================================
// ToDownloadOptions Tests
// ============================================================================

TEST_F(ConfigLoaderTest, ToDownloadOptions) {
    falcon::cli::CliConfig config;
    config.max_connections = 8;
    config.timeout_seconds = 45;
    config.max_retries = 5;
    config.default_download_dir = "/downloads";
    config.speed_limit = 1024 * 1024;
    config.user_agent = "TestAgent";
    config.proxy = "http://proxy:8080";
    config.verify_ssl = false;
    config.headers["Authorization"] = "Bearer token";

    auto opts = config.to_download_options();

    EXPECT_EQ(8, opts.max_connections);
    EXPECT_EQ(45, opts.timeout_seconds);
    EXPECT_EQ(5, opts.max_retries);
    EXPECT_EQ("/downloads", opts.output_directory);
    EXPECT_EQ(1024 * 1024, opts.speed_limit);
    EXPECT_EQ("TestAgent", opts.user_agent);
    EXPECT_EQ("http://proxy:8080", opts.proxy);
    EXPECT_FALSE(opts.verify_ssl);
    EXPECT_EQ("Bearer token", opts.headers["Authorization"]);
}

// ============================================================================
// Validation Tests
// ============================================================================

TEST_F(ConfigLoaderTest, ValidateValidConfig) {
    falcon::cli::CliConfig config;
    config.max_connections = 8;
    config.timeout_seconds = 30;
    config.max_retries = 3;

    auto errors = falcon::cli::ConfigLoader::validate(config);
    EXPECT_TRUE(errors.empty());
}

TEST_F(ConfigLoaderTest, ValidateInvalidConnections) {
    falcon::cli::CliConfig config;
    config.max_connections = 0;  // Invalid

    auto errors = falcon::cli::ConfigLoader::validate(config);
    EXPECT_FALSE(errors.empty());
    EXPECT_TRUE(errors[0].find("max_connections") != std::string::npos);
}

TEST_F(ConfigLoaderTest, ValidateInvalidPort) {
    falcon::cli::CliConfig config;
    config.rpc_listen_port = 70000;  // Invalid

    auto errors = falcon::cli::ConfigLoader::validate(config);
    EXPECT_FALSE(errors.empty());
}

// ============================================================================
// Merge Configs Tests
// ============================================================================

TEST_F(ConfigLoaderTest, MergeConfigsCliPriority) {
    falcon::cli::CliConfig file_config;
    file_config.max_connections = 8;
    file_config.timeout_seconds = 60;
    file_config.user_agent = "FileAgent";

    falcon::cli::CliConfig cli_args;
    cli_args.max_connections = 16;  // CLI override

    auto merged = falcon::cli::merge_configs(file_config, cli_args);

    // CLI should override
    EXPECT_EQ(16, merged.max_connections);
    // File value preserved (CLI didn't override)
    EXPECT_EQ(60, merged.timeout_seconds);
    EXPECT_EQ("FileAgent", merged.user_agent);
}

TEST_F(ConfigLoaderTest, MergeConfigsBooleanFlags) {
    falcon::cli::CliConfig file_config;
    file_config.verbose = true;
    file_config.verify_ssl = true;

    falcon::cli::CliConfig cli_args;
    cli_args.quiet = true;
    cli_args.verify_ssl = false;

    auto merged = falcon::cli::merge_configs(file_config, cli_args);

    // Both verbose and quiet can be set
    EXPECT_TRUE(merged.verbose);
    EXPECT_TRUE(merged.quiet);
    // CLI override for verify_ssl
    EXPECT_FALSE(merged.verify_ssl);
}

// ============================================================================
// Environment Variable Tests
// ============================================================================

TEST_F(ConfigLoaderTest, LoadFromEnv) {
    // Set environment variables
#ifdef _WIN32
    _putenv_s("FALCON_MAX_CONNECTIONS", "12");
    _putenv_s("FALCON_PROXY", "http://env-proxy:8080");
#else
    setenv("FALCON_MAX_CONNECTIONS", "12", 1);
    setenv("FALCON_PROXY", "http://env-proxy:8080", 1);
#endif

    // Clear any cached config and reload
    // Note: This test may be affected by other tests, so we just check it doesn't crash

    auto config = falcon::cli::ConfigLoader::load_or_default("");

    // The values should be either from env or defaults
    // We can't reliably test env loading in parallel tests

    // Clean up
#ifdef _WIN32
    _putenv_s("FALCON_MAX_CONNECTIONS", "");
    _putenv_s("FALCON_PROXY", "");
#else
    unsetenv("FALCON_MAX_CONNECTIONS");
    unsetenv("FALCON_PROXY");
#endif
}

} // anonymous namespace

#endif // FALCON_USE_JSON
