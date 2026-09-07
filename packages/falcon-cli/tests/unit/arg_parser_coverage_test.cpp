/**
 * @file arg_parser_coverage_test.cpp
 * @brief Additional unit tests covering untested branches of arg_parser.cpp
 */

#include "arg_parser.hpp"

#include <gtest/gtest.h>

#include <falcon/types.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using falcon::cli::CliArgs;
using falcon::cli::parse_args;
using falcon::cli::read_urls_from_file;

namespace {

CliArgs parse_from(const std::vector<std::string>& args) {
    std::vector<std::string> storage = {"falcon-cli"};
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& a : storage) {
        argv.push_back(a.data());
    }

    return parse_args(static_cast<int>(argv.size()), argv.data());
}

std::filesystem::path write_temp_file(const std::string& content) {
    static int counter = 0;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? info->name() : "file";
    name += "_";
    name += std::to_string(counter++);
    name += ".txt";

    const auto path = std::filesystem::path(::testing::TempDir()) / name;
    std::ofstream of(path);
    of << content;
    return path;
}

} // namespace

// ============================================================================
// Help / version flags
// ============================================================================

TEST(ArgParserCoverage, HelpAndVersionShortAndLongFlags) {
    auto h1 = parse_from({"-h"});
    EXPECT_TRUE(h1.show_help);
    auto h2 = parse_from({"--help"});
    EXPECT_TRUE(h2.show_help);
    auto v1 = parse_from({"-V"});
    EXPECT_TRUE(v1.show_version);
    auto v2 = parse_from({"--version"});
    EXPECT_TRUE(v2.show_version);
    EXPECT_FALSE(v2.show_help);
}

// ============================================================================
// Value options: exercised aliases and trailing (value-less) forms
// ============================================================================

TEST(ArgParserCoverage, InputFileAliases) {
    EXPECT_EQ(parse_from({"-i", "a.txt"}).input_file, "a.txt");
    EXPECT_EQ(parse_from({"--input-file", "b.txt"}).input_file, "b.txt");
    EXPECT_EQ(parse_from({"--input", "c.txt"}).input_file, "c.txt");
    EXPECT_TRUE(parse_from({"-i"}).input_file.empty());
    EXPECT_TRUE(parse_from({"--input-file"}).input_file.empty());
}

TEST(ArgParserCoverage, OutputAndDirectoryAliases) {
    EXPECT_EQ(parse_from({"-o", "out.zip"}).output_file, "out.zip");
    EXPECT_EQ(parse_from({"--output", "out2.zip"}).output_file, "out2.zip");
    EXPECT_EQ(parse_from({"-d", "/tmp/d1"}).output_dir, "/tmp/d1");
    EXPECT_EQ(parse_from({"--directory", "d2"}).output_dir, "d2");
    EXPECT_EQ(parse_from({"--dir", "d3"}).output_dir, "d3");
    EXPECT_TRUE(parse_from({"-o"}).output_file.empty());
    EXPECT_TRUE(parse_from({"-d"}).output_dir.empty());
}

TEST(ArgParserCoverage, ConnectionsAliases) {
    EXPECT_EQ(parse_from({"-c", "8"}).connections, 8);
    EXPECT_EQ(parse_from({"--connections", "3"}).connections, 3);
    EXPECT_EQ(parse_from({"-x", "16"}).connections, 16);
    EXPECT_EQ(parse_from({"--max-connections", "2"}).connections, 2);
    EXPECT_EQ(parse_from({"--max-connection-per-server", "5"}).connections, 5);
    EXPECT_EQ(parse_from({"-s", "7"}).connections, 7);
    EXPECT_EQ(parse_from({"--split", "9"}).connections, 9);
    EXPECT_EQ(parse_from({"-c"}).connections, 4);  // trailing: default kept
}

TEST(ArgParserCoverage, MaxConcurrentDownloadsAlias) {
    EXPECT_EQ(parse_from({"-j", "4"}).max_concurrent_downloads, 4);
    EXPECT_EQ(parse_from({"--max-concurrent-downloads", "6"}).max_concurrent_downloads, 6);
    EXPECT_EQ(parse_from({"-j"}).max_concurrent_downloads, 1);
}

TEST(ArgParserCoverage, MinSegmentSizeAliases) {
    EXPECT_EQ(parse_from({"-k", "512K"}).min_segment_size, 512ULL * 1024ULL);
    EXPECT_EQ(parse_from({"--min-split-size", "2M"}).min_segment_size, 2ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(parse_from({"--min-segment-size", "1G"}).min_segment_size,
              1ULL * 1024ULL * 1024ULL * 1024ULL);
}

TEST(ArgParserCoverage, SpeedLimitAliases) {
    EXPECT_EQ(parse_from({"--limit", "1M"}).speed_limit, 1024ULL * 1024ULL);
    EXPECT_EQ(parse_from({"--max-download-limit", "128K"}).speed_limit, 128ULL * 1024ULL);
}

TEST(ArgParserCoverage, TimeoutAndRetryWaitValues) {
    EXPECT_EQ(parse_from({"-t", "60"}).timeout, 60);
    EXPECT_EQ(parse_from({"--timeout", "15"}).timeout, 15);
    EXPECT_EQ(parse_from({"-r", "5"}).max_retries, 5);
    EXPECT_EQ(parse_from({"--retry", "0"}).max_retries, 0);
    EXPECT_EQ(parse_from({"--retry-wait", "2"}).retry_wait, 2);
    EXPECT_EQ(parse_from({"-t"}).timeout, 30);
    EXPECT_EQ(parse_from({"-r"}).max_retries, 3);
    EXPECT_EQ(parse_from({"--retry-wait"}).retry_wait, 5);
}

TEST(ArgParserCoverage, ContinueFlagVariants) {
    EXPECT_FALSE(parse_from({"--no-continue"}).continue_download);
    EXPECT_TRUE(parse_from({"--continue"}).continue_download);
    EXPECT_TRUE(parse_from({"--continue", "true"}).continue_download);
    EXPECT_FALSE(parse_from({"--continue", "false"}).continue_download);
    EXPECT_TRUE(parse_from({"--continue", "bogus"}).continue_download);  // default
}

TEST(ArgParserCoverage, SslFlagVariants) {
    EXPECT_FALSE(parse_from({"--no-verify-ssl"}).verify_ssl);
    EXPECT_TRUE(parse_from({"--check-certificate"}).verify_ssl);
    EXPECT_TRUE(parse_from({"--check-certificate", "true"}).verify_ssl);
    EXPECT_FALSE(parse_from({"--check-certificate", "false"}).verify_ssl);
    EXPECT_TRUE(parse_from({"--check-certificate", "maybe"}).verify_ssl);  // default
}

TEST(ArgParserCoverage, AdaptiveFlag) {
    EXPECT_FALSE(parse_from({"--no-adaptive"}).adaptive_sizing);
    EXPECT_TRUE(parse_from({}).adaptive_sizing);
}

TEST(ArgParserCoverage, NetworkCredentialOptions) {
    auto a = parse_from({"--proxy", "http://p:8080",
                         "--proxy-user", "puser",
                         "--proxy-passwd", "ppass",
                         "-U", "UA/1",
                         "--user-agent", "UA/2",
                         "--http-user", "huser",
                         "--http-passwd", "hpass",
                         "--referer", "http://referer/",
                         "--load-cookies", "cookies.txt",
                         "--save-cookies", "jar.txt"});
    EXPECT_EQ(a.proxy, "http://p:8080");
    EXPECT_EQ(a.proxy_user, "puser");
    EXPECT_EQ(a.proxy_passwd, "ppass");
    EXPECT_EQ(a.user_agent, "UA/2");
    EXPECT_EQ(a.http_user, "huser");
    EXPECT_EQ(a.http_passwd, "hpass");
    EXPECT_EQ(a.referer, "http://referer/");
    EXPECT_EQ(a.cookie_file, "cookies.txt");
    EXPECT_EQ(a.save_cookies, "jar.txt");
}

TEST(ArgParserCoverage, NetworkCredentialTrailingDefaults) {
    auto a = parse_from({"--proxy", "-U", "--referer", "--load-cookies",
                         "--save-cookies", "--http-user", "--http-passwd",
                         "--proxy-user", "--proxy-passwd"});
    // Each option consumes the following flag-looking token as its value,
    // except when it is the final token in argv.
    EXPECT_EQ(a.proxy, "-U");
    EXPECT_EQ(a.referer, "--load-cookies");
    EXPECT_EQ(a.save_cookies, "--http-user");
    EXPECT_EQ(a.http_passwd, "--proxy-user");
    EXPECT_TRUE(a.cookie_file.empty());
    EXPECT_TRUE(a.http_user.empty());
    EXPECT_TRUE(a.proxy_user.empty());
    EXPECT_TRUE(a.proxy_passwd.empty());
}

TEST(ArgParserCoverage, HeaderParsingVariants) {
    auto a = parse_from({
        "-H", "X-A: v1",
        "--header", "X-B:v2",
        "-H", "NoColonHere",
        "-H", "Trailing:",
        "-H", "X-C:    spaced-value",
        "-H",
    });
    ASSERT_EQ(a.headers.size(), 3ULL);
    EXPECT_EQ(a.headers[0].first, "X-A");
    EXPECT_EQ(a.headers[0].second, "v1");
    EXPECT_EQ(a.headers[1].first, "X-B");
    EXPECT_EQ(a.headers[1].second, "v2");
    EXPECT_EQ(a.headers[2].first, "X-C");
    EXPECT_EQ(a.headers[2].second, "spaced-value");
}

TEST(ArgParserCoverage, MiscBooleanFlags) {
    auto a = parse_from({"--use-head",
                         "--conditional-download",
                         "--auto-file-renaming",
                         "--rpc-allow-origin-all",
                         "--no-color",
                         "-v",
                         "-q",
                         "--show-config-path",
                         "--create-default-config"});
    EXPECT_TRUE(a.use_head);
    EXPECT_TRUE(a.conditional_download);
    EXPECT_TRUE(a.auto_renaming);
    EXPECT_TRUE(a.rpc_allow_origin_all);
    EXPECT_TRUE(a.no_color);
    EXPECT_TRUE(a.verbose);
    EXPECT_TRUE(a.quiet);
    EXPECT_TRUE(a.show_config_path);
    EXPECT_TRUE(a.create_default_config);
}

TEST(ArgParserCoverage, RpcOptions) {
    auto a = parse_from({"--rpc-secret", "s3cret", "--rpc-listen-port", "7000"});
    EXPECT_EQ(a.rpc_secret, "s3cret");
    EXPECT_EQ(a.rpc_listen_port, 7000);

    auto trailing = parse_from({"--rpc-secret", "--rpc-listen-port"});
    EXPECT_EQ(trailing.rpc_secret, "--rpc-listen-port");
    EXPECT_EQ(trailing.rpc_listen_port, 6800);
}

TEST(ArgParserCoverage, ConfigOptionAliases) {
    EXPECT_EQ(parse_from({"-C", "a.json"}).config_file, "a.json");
    EXPECT_EQ(parse_from({"--config", "b.json"}).config_file, "b.json");
    EXPECT_TRUE(parse_from({"--config"}).config_file.empty());
}

TEST(ArgParserCoverage, PriorityWithoutValueKeepsDefault) {
    auto a = parse_from({"-p"});
    EXPECT_FALSE(a.priority_specified);
    EXPECT_EQ(a.priority, falcon::TaskPriority::Normal);
}

// ============================================================================
// Positional URL handling
// ============================================================================

TEST(ArgParserCoverage, PositionalUrlsIgnoreEmptyAndOptionTokens) {
    auto a = parse_from({"--unknown-flag", "http://a.example/f1", "", "http://b.example/f2"});
    ASSERT_EQ(a.urls.size(), 2ULL);
    EXPECT_EQ(a.urls[0], "http://a.example/f1");
    EXPECT_EQ(a.urls[1], "http://b.example/f2");
}

TEST(ArgParserCoverage, EmptyArgvYieldsDefaults) {
    auto a = parse_from({});
    EXPECT_TRUE(a.urls.empty());
    EXPECT_TRUE(a.input_file.empty());
    EXPECT_EQ(a.connections, 4);
    EXPECT_EQ(a.timeout, 30);
    EXPECT_TRUE(a.continue_download);
    EXPECT_TRUE(a.verify_ssl);
}

TEST(ArgParserCoverage, ManyOptionsCombined) {
    auto a = parse_from({"http://a.example/file.zip",
                         "-o", "renamed.zip",
                         "-d", "/tmp/dl",
                         "-c", "8",
                         "-j", "2",
                         "-k", "4M",
                         "--limit", "2M",
                         "-t", "9",
                         "-r", "1",
                         "--retry-wait", "3",
                         "-H", "Accept: application/*",
                         "--proxy", "http://proxy:3128",
                         "-U", "Combined/1.0",
                         "-p", "high",
                         "--no-continue",
                         "--no-verify-ssl",
                         "--no-adaptive",
                         "-v"});
    ASSERT_EQ(a.urls.size(), 1ULL);
    EXPECT_EQ(a.output_file, "renamed.zip");
    EXPECT_EQ(a.output_dir, "/tmp/dl");
    EXPECT_EQ(a.connections, 8);
    EXPECT_EQ(a.max_concurrent_downloads, 2);
    EXPECT_EQ(a.min_segment_size, 4ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(a.speed_limit, 2ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(a.timeout, 9);
    EXPECT_EQ(a.max_retries, 1);
    EXPECT_EQ(a.retry_wait, 3);
    ASSERT_EQ(a.headers.size(), 1ULL);
    EXPECT_EQ(a.headers[0].first, "Accept");
    EXPECT_EQ(a.headers[0].second, "application/*");
    EXPECT_EQ(a.proxy, "http://proxy:3128");
    EXPECT_EQ(a.user_agent, "Combined/1.0");
    EXPECT_TRUE(a.priority_specified);
    EXPECT_EQ(a.priority, falcon::TaskPriority::High);
    EXPECT_FALSE(a.continue_download);
    EXPECT_FALSE(a.verify_ssl);
    EXPECT_FALSE(a.adaptive_sizing);
    EXPECT_TRUE(a.verbose);
}

// ============================================================================
// read_urls_from_file
// ============================================================================

TEST(ReadUrlsFromFileCoverage, ReadsRealFileWithCommentsAndBlanks) {
    const auto path = write_temp_file(
        "# leading comment\n"
        "  http://a.example/one.zip  \r\n"
        "\n"
        "http://a.example/two.zip\n"
        "   # indented comment\n"
        "\thttp://a.example/three.zip\t\n");
    auto urls = read_urls_from_file(path.string());
    ASSERT_EQ(urls.size(), 3ULL);
    EXPECT_EQ(urls[0], "http://a.example/one.zip");
    EXPECT_EQ(urls[1], "http://a.example/two.zip");
    EXPECT_EQ(urls[2], "http://a.example/three.zip");
}

TEST(ReadUrlsFromFileCoverage, MissingFileReturnsEmpty) {
    auto urls = read_urls_from_file("/nonexistent/falcon-dir/urls.txt");
    EXPECT_TRUE(urls.empty());
}

TEST(ReadUrlsFromFileCoverage, DashReadsFromStdin) {
    const auto path = write_temp_file("http://a.example/stdin-1.zip\n"
                                      "# comment\n"
                                      "http://a.example/stdin-2.zip\n");
    FILE* reopened = std::freopen(path.string().c_str(), "r", stdin);
    if (reopened == nullptr) {
        GTEST_SKIP() << "cannot redirect stdin for this test";
    }

    auto urls = read_urls_from_file("-");
    ASSERT_EQ(urls.size(), 2ULL);
    EXPECT_EQ(urls[0], "http://a.example/stdin-1.zip");
    EXPECT_EQ(urls[1], "http://a.example/stdin-2.zip");
}
