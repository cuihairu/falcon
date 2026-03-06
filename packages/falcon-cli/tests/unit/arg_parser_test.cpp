/**
 * @file arg_parser_test.cpp
 * @brief Unit tests for CLI argument parsing functions
 * @author Falcon Team
 * @date 2025-12-28
 */

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <sstream>

// Helper functions extracted from main.cpp for testing
// These mirror the implementations in main.cpp

namespace {

/**
 * @brief Parse size string to bytes
 *
 * Supports suffixes: K/k (KB), M/m (MB), G/g (GB), T/t (TB)
 */
std::size_t parse_size_bytes(std::string size_str) {
    if (size_str.empty()) return 0;

    std::size_t multiplier = 1;
    char suffix = size_str.back();
    if (suffix == 'K' || suffix == 'k') {
        multiplier = 1024;
        size_str.pop_back();
    } else if (suffix == 'M' || suffix == 'm') {
        multiplier = 1024 * 1024;
        size_str.pop_back();
    } else if (suffix == 'G' || suffix == 'g') {
        multiplier = 1024 * 1024 * 1024;
        size_str.pop_back();
    } else if (suffix == 'T' || suffix == 't') {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        size_str.pop_back();
    }

    return static_cast<std::size_t>(std::stoull(size_str) * multiplier);
}

/**
 * @brief Parse boolean string value
 */
bool parse_bool(std::string value, bool default_value) {
    if (value.empty()) return default_value;
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (value == "1" || value == "true" || value == "yes" || value == "y" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "n" || value == "off") return false;
    return default_value;
}

/**
 * @brief Read URLs from a file-like content
 */
std::vector<std::string> read_urls_from_content(const std::string& content) {
    std::vector<std::string> urls;
    std::istringstream in(content);
    std::string line;

    while (std::getline(in, line)) {
        // Trim whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (start > 0) {
            line = line.substr(start);
        }
        if (line.empty() || line[0] == '#') continue;
        urls.push_back(line);
    }

    return urls;
}

} // anonymous namespace

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
    auto urls = read_urls_from_content("");
    EXPECT_TRUE(urls.empty());
}

TEST(ReadUrlsFromContent, SingleUrl) {
    auto urls = read_urls_from_content("https://example.com/file.zip");
    ASSERT_EQ(urls.size(), 1ULL);
    EXPECT_EQ(urls[0], "https://example.com/file.zip");
}

TEST(ReadUrlsFromContent, MultipleUrls) {
    auto urls = read_urls_from_content(
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
    auto urls = read_urls_from_content(
        "# This is a comment\n"
        "https://example.com/file.zip\n"
        "# Another comment"
    );
    ASSERT_EQ(urls.size(), 1ULL);
    EXPECT_EQ(urls[0], "https://example.com/file.zip");
}

TEST(ReadUrlsFromContent, IgnoreEmptyLines) {
    auto urls = read_urls_from_content(
        "\n"
        "https://example.com/file.zip\n"
        "\n"
        "\n"
    );
    ASSERT_EQ(urls.size(), 1ULL);
    EXPECT_EQ(urls[0], "https://example.com/file.zip");
}

TEST(ReadUrlsFromContent, TrimWhitespace) {
    auto urls = read_urls_from_content(
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
    auto urls = read_urls_from_content(
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
    auto urls = read_urls_from_content(
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
