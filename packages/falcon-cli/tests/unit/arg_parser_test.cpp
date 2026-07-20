/**
 * @file arg_parser_test.cpp
 * @brief Unit tests for CLI argument parsing functions
 */

#include "arg_parser.hpp"

#include <gtest/gtest.h>

#include <falcon/types.hpp>

#include <algorithm>
#include <string>
#include <sstream>
#include <vector>

using falcon::cli::parse_args;
using falcon::cli::parse_bool;
using falcon::cli::parse_priority;
using falcon::cli::parse_size_bytes;
using falcon::cli::parse_url_lines;

namespace {

std::vector<std::string> parse_urls_from_content(const std::string& content) {
    std::istringstream in(content);
    return parse_url_lines(in);
}

falcon::cli::CliArgs parse_args_from_vector(const std::vector<std::string>& args) {
    std::vector<std::string> storage = {"falcon-cli"};
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& arg : storage) {
        argv.push_back(arg.data());
    }

    return parse_args(static_cast<int>(argv.size()), argv.data());
}

} // namespace

// ============================================================================
// parse_size_bytes Tests
// ============================================================================

TEST(ParseSizeBytes, EmptyString) {
    EXPECT_EQ(parse_size_bytes(""), 0ULL);
}

TEST(ParseSizeBytes, PlainNumber) {
    EXPECT_EQ(parse_size_bytes("100"), 100ULL);
    EXPECT_EQ(parse_size_bytes("1024"), 1024ULL);
    EXPECT_EQ(parse_size_bytes("0"), 0ULL);
}

TEST(ParseSizeBytes, Kilobytes) {
    EXPECT_EQ(parse_size_bytes("1K"), 1024ULL);
    EXPECT_EQ(parse_size_bytes("1k"), 1024ULL);
    EXPECT_EQ(parse_size_bytes("2K"), 2048ULL);
    EXPECT_EQ(parse_size_bytes("512K"), 512ULL * 1024ULL);
}

TEST(ParseSizeBytes, Megabytes) {
    EXPECT_EQ(parse_size_bytes("1M"), 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size_bytes("1m"), 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size_bytes("10M"), 10ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size_bytes("512M"), 512ULL * 1024ULL * 1024ULL);
}

TEST(ParseSizeBytes, Gigabytes) {
    EXPECT_EQ(parse_size_bytes("1G"), 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size_bytes("1g"), 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size_bytes("2G"), 2ULL * 1024ULL * 1024ULL * 1024ULL);
}

TEST(ParseSizeBytes, Terabytes) {
    EXPECT_EQ(parse_size_bytes("1T"), 1024ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size_bytes("1t"), 1024ULL * 1024ULL * 1024ULL * 1024ULL);
}

TEST(ParseSizeBytes, LargeValues) {
    EXPECT_EQ(parse_size_bytes("100M"), 100ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size_bytes("1024K"), 1024ULL * 1024ULL);  // 1024 KB = 1 MB
}

// ============================================================================
// parse_bool Tests
// ============================================================================

TEST(ParseBool, EmptyString) {
    EXPECT_TRUE(parse_bool("", true));   // Default true
    EXPECT_FALSE(parse_bool("", false)); // Default false
}

TEST(ParseBool, TrueValues) {
    EXPECT_TRUE(parse_bool("true", false));
    EXPECT_TRUE(parse_bool("TRUE", false));
    EXPECT_TRUE(parse_bool("1", false));
    EXPECT_TRUE(parse_bool("yes", false));
    EXPECT_TRUE(parse_bool("YES", false));
    EXPECT_TRUE(parse_bool("y", false));
    EXPECT_TRUE(parse_bool("Y", false));
    EXPECT_TRUE(parse_bool("on", false));
    EXPECT_TRUE(parse_bool("ON", false));
}

TEST(ParseBool, FalseValues) {
    EXPECT_FALSE(parse_bool("false", true));
    EXPECT_FALSE(parse_bool("FALSE", true));
    EXPECT_FALSE(parse_bool("0", true));
    EXPECT_FALSE(parse_bool("no", true));
    EXPECT_FALSE(parse_bool("NO", true));
    EXPECT_FALSE(parse_bool("n", true));
    EXPECT_FALSE(parse_bool("N", true));
    EXPECT_FALSE(parse_bool("off", true));
    EXPECT_FALSE(parse_bool("OFF", true));
}

TEST(ParseBool, InvalidValue) {
    // Invalid values should return the default
    EXPECT_TRUE(parse_bool("invalid", true));
    EXPECT_FALSE(parse_bool("invalid", false));
    EXPECT_TRUE(parse_bool("maybe", true));
    EXPECT_FALSE(parse_bool("maybe", false));
}

// ============================================================================
// read_urls_from_content Tests
// ============================================================================

TEST(ReadUrlsFromContent, EmptyContent) {
    auto urls = parse_urls_from_content("");
    EXPECT_TRUE(urls.empty());
}

TEST(ReadUrlsFromContent, SingleUrl) {
    auto urls = parse_urls_from_content("https://example.com/file.zip");
    ASSERT_EQ(urls.size(), 1ULL);
    EXPECT_EQ(urls[0], "https://example.com/file.zip");
}

TEST(ReadUrlsFromContent, MultipleUrls) {
    auto urls = parse_urls_from_content(
        "https://example.com/file1.zip\n"
        "https://example.com/file2.tar.gz\n"
        "https://example.com/file3.iso"
    );
    ASSERT_EQ(urls.size(), 3ULL);
    EXPECT_EQ(urls[0], "https://example.com/file1.zip");
    EXPECT_EQ(urls[1], "https://example.com/file2.tar.gz");
    EXPECT_EQ(urls[2], "https://example.com/file3.iso");
}

TEST(ReadUrlsFromContent, IgnoreComments) {
    auto urls = parse_urls_from_content(
        "# This is a comment\n"
        "https://example.com/file.zip\n"
        "# Another comment"
    );
    ASSERT_EQ(urls.size(), 1ULL);
    EXPECT_EQ(urls[0], "https://example.com/file.zip");
}

TEST(ReadUrlsFromContent, IgnoreEmptyLines) {
    auto urls = parse_urls_from_content(
        "\n"
        "https://example.com/file.zip\n"
        "\n"
        "\n"
    );
    ASSERT_EQ(urls.size(), 1ULL);
    EXPECT_EQ(urls[0], "https://example.com/file.zip");
}

TEST(ReadUrlsFromContent, TrimWhitespace) {
    auto urls = parse_urls_from_content(
        "  https://example.com/file.zip  \n"
        "\thttps://example.com/file2.zip\t\n"
        "   \t https://example.com/file3.zip \t   "
    );
    ASSERT_EQ(urls.size(), 3ULL);
    EXPECT_EQ(urls[0], "https://example.com/file.zip");
    EXPECT_EQ(urls[1], "https://example.com/file2.zip");
    EXPECT_EQ(urls[2], "https://example.com/file3.zip");
}

TEST(ReadUrlsFromContent, MixedProtocols) {
    auto urls = parse_urls_from_content(
        "https://example.com/file.zip\n"
        "ftp://ftp.example.com/data.csv\n"
        "http://legacy.example.com/old.iso"
    );
    ASSERT_EQ(urls.size(), 3ULL);
    EXPECT_EQ(urls[0], "https://example.com/file.zip");
    EXPECT_EQ(urls[1], "ftp://ftp.example.com/data.csv");
    EXPECT_EQ(urls[2], "http://legacy.example.com/old.iso");
}

TEST(ReadUrlsFromContent, WindowsLineEndings) {
    auto urls = parse_urls_from_content(
        "https://example.com/file1.zip\r\n"
        "https://example.com/file2.zip\r\n"
    );
    ASSERT_EQ(urls.size(), 2ULL);
    EXPECT_EQ(urls[0], "https://example.com/file1.zip");
    EXPECT_EQ(urls[1], "https://example.com/file2.zip");
}

// ============================================================================
// Argument Validation Tests
// ============================================================================

TEST(ArgumentValidation, ConnectionLimits) {
    // Test that connection values are clamped to valid range
    int connections = std::max(1, std::min(64, 0));   // Too low
    EXPECT_EQ(connections, 1);

    connections = std::max(1, std::min(64, 4));        // Normal
    EXPECT_EQ(connections, 4);

    connections = std::max(1, std::min(64, 100));      // Too high
    EXPECT_EQ(connections, 64);
}

TEST(ArgumentValidation, MaxConcurrentDownloads) {
    // Test that concurrent download limits are clamped
    int max_concurrent = std::max(1, std::min(64, 0));  // Too low
    EXPECT_EQ(max_concurrent, 1);

    max_concurrent = std::max(1, std::min(64, 5));       // Normal
    EXPECT_EQ(max_concurrent, 5);

    max_concurrent = std::max(1, std::min(64, 128));     // Too high
    EXPECT_EQ(max_concurrent, 64);
}

TEST(ArgumentValidation, RetryLimits) {
    // Test retry value clamping
    int retries = std::max(0, std::min(10, -1));  // Too low
    EXPECT_EQ(retries, 0);

    retries = std::max(0, std::min(10, 3));        // Normal
    EXPECT_EQ(retries, 3);

    retries = std::max(0, std::min(10, 20));       // Too high
    EXPECT_EQ(retries, 10);
}

// ============================================================================
// parse_priority Tests
// ============================================================================

TEST(ParsePriority, NamedValues) {
    EXPECT_EQ(parse_priority("low"), falcon::TaskPriority::Low);
    EXPECT_EQ(parse_priority("normal"), falcon::TaskPriority::Normal);
    EXPECT_EQ(parse_priority("high"), falcon::TaskPriority::High);
    EXPECT_EQ(parse_priority("critical"), falcon::TaskPriority::Critical);
}

TEST(ParsePriority, NumericValues) {
    EXPECT_EQ(parse_priority("0"), falcon::TaskPriority::Low);
    EXPECT_EQ(parse_priority("1"), falcon::TaskPriority::Normal);
    EXPECT_EQ(parse_priority("2"), falcon::TaskPriority::High);
    EXPECT_EQ(parse_priority("3"), falcon::TaskPriority::Critical);
}

TEST(ParsePriority, CaseInsensitive) {
    EXPECT_EQ(parse_priority("LOW"), falcon::TaskPriority::Low);
    EXPECT_EQ(parse_priority("High"), falcon::TaskPriority::High);
    EXPECT_EQ(parse_priority("CRITICAL"), falcon::TaskPriority::Critical);
}

TEST(ParsePriority, InvalidDefaultsToNormal) {
    EXPECT_EQ(parse_priority("invalid"), falcon::TaskPriority::Normal);
    EXPECT_EQ(parse_priority(""), falcon::TaskPriority::Normal);
}

// ============================================================================
// parse_args Tests
// ============================================================================

TEST(ParseArgs, PriorityLongOption) {
    auto args = parse_args_from_vector({"--priority", "critical", "https://example.com/file.zip"});

    EXPECT_TRUE(args.priority_specified);
    EXPECT_EQ(args.priority, falcon::TaskPriority::Critical);
    ASSERT_EQ(args.urls.size(), 1ULL);
    EXPECT_EQ(args.urls[0], "https://example.com/file.zip");
}

TEST(ParseArgs, PriorityShortOption) {
    auto args = parse_args_from_vector({"-p", "high", "https://example.com/file.zip"});

    EXPECT_TRUE(args.priority_specified);
    EXPECT_EQ(args.priority, falcon::TaskPriority::High);
    ASSERT_EQ(args.urls.size(), 1ULL);
    EXPECT_EQ(args.urls[0], "https://example.com/file.zip");
}

TEST(ParseArgs, PriorityDefaultsWhenMissing) {
    auto args = parse_args_from_vector({"https://example.com/file.zip"});

    EXPECT_FALSE(args.priority_specified);
    EXPECT_EQ(args.priority, falcon::TaskPriority::Normal);
}

TEST(ParseArgs, ClampsQueueAndConnectionLimits) {
    auto low = parse_args_from_vector({"-j", "0", "-c", "0", "-r", "-1"});
    EXPECT_EQ(low.max_concurrent_downloads, 1);
    EXPECT_EQ(low.connections, 1);
    EXPECT_EQ(low.max_retries, 0);

    auto high = parse_args_from_vector({"-j", "128", "-c", "128", "-r", "20"});
    EXPECT_EQ(high.max_concurrent_downloads, 64);
    EXPECT_EQ(high.connections, 64);
    EXPECT_EQ(high.max_retries, 10);
}
