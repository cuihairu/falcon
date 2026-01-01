// Falcon URL Utils Unit Tests
// Copyright (c) 2025 Falcon Project

#include "internal/plugin_manager.hpp"

#include <gtest/gtest.h>

using namespace falcon::internal;

class UrlUtilsTest : public ::testing::Test {};

TEST_F(UrlUtilsTest, ExtractSchemeHttp) {
    EXPECT_EQ(UrlUtils::extract_scheme("http://example.com/file.zip"), "http");
}

TEST_F(UrlUtilsTest, ExtractSchemeHttps) {
    EXPECT_EQ(UrlUtils::extract_scheme("https://example.com/file.zip"), "https");
}

TEST_F(UrlUtilsTest, ExtractSchemeFtp) {
    EXPECT_EQ(UrlUtils::extract_scheme("ftp://ftp.example.com/file.zip"), "ftp");
}

TEST_F(UrlUtilsTest, ExtractSchemeMagnet) {
    EXPECT_EQ(UrlUtils::extract_scheme("magnet:?xt=urn:btih:abc123"), "magnet");
}

TEST_F(UrlUtilsTest, ExtractSchemeUpperCase) {
    // Should convert to lowercase
    EXPECT_EQ(UrlUtils::extract_scheme("HTTPS://EXAMPLE.COM/FILE.ZIP"), "https");
    EXPECT_EQ(UrlUtils::extract_scheme("HTTP://Example.Com/File.zip"), "http");
}

TEST_F(UrlUtilsTest, ExtractSchemeNoScheme) {
    EXPECT_EQ(UrlUtils::extract_scheme("example.com/file.zip"), "");
    EXPECT_EQ(UrlUtils::extract_scheme("/path/to/file"), "");
    EXPECT_EQ(UrlUtils::extract_scheme("file.zip"), "");
}

TEST_F(UrlUtilsTest, ExtractSchemeEmpty) {
    EXPECT_EQ(UrlUtils::extract_scheme(""), "");
}

TEST_F(UrlUtilsTest, ExtractFilenameSimple) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/file.zip"), "file.zip");
}

TEST_F(UrlUtilsTest, ExtractFilenameWithPath) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/path/to/file.zip"),
              "file.zip");
}

TEST_F(UrlUtilsTest, ExtractFilenameWithQuery) {
    EXPECT_EQ(
        UrlUtils::extract_filename("https://example.com/file.zip?token=abc123"),
        "file.zip");
}

TEST_F(UrlUtilsTest, ExtractFilenameWithFragment) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/file.zip#section"),
              "file.zip");
}

TEST_F(UrlUtilsTest, ExtractFilenameWithQueryAndFragment) {
    EXPECT_EQ(UrlUtils::extract_filename(
                  "https://example.com/file.zip?token=abc#section"),
              "file.zip");
}

TEST_F(UrlUtilsTest, ExtractFilenameNoFilename) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/"), "download");
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com"), "download");
}

TEST_F(UrlUtilsTest, ExtractFilenameSpecialChars) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/my-file_v2.0.zip"),
              "my-file_v2.0.zip");
}

TEST_F(UrlUtilsTest, IsValidUrlHttp) {
    EXPECT_TRUE(UrlUtils::is_valid_url("http://example.com/file.zip"));
}

TEST_F(UrlUtilsTest, IsValidUrlHttps) {
    EXPECT_TRUE(UrlUtils::is_valid_url("https://example.com/file.zip"));
}

TEST_F(UrlUtilsTest, IsValidUrlFtp) {
    EXPECT_TRUE(UrlUtils::is_valid_url("ftp://ftp.example.com/file.zip"));
}

TEST_F(UrlUtilsTest, IsValidUrlMagnet) {
    EXPECT_TRUE(UrlUtils::is_valid_url("magnet:?xt=urn:btih:abc123"));
}

TEST_F(UrlUtilsTest, IsValidUrlNoScheme) {
    EXPECT_FALSE(UrlUtils::is_valid_url("example.com/file.zip"));
}

TEST_F(UrlUtilsTest, IsValidUrlEmpty) {
    EXPECT_FALSE(UrlUtils::is_valid_url(""));
}

TEST_F(UrlUtilsTest, IsValidUrlRelativePath) {
    EXPECT_FALSE(UrlUtils::is_valid_url("/path/to/file.zip"));
}

//==============================================================================
// 私有协议方案测试
//==============================================================================

TEST(PrivateProtocolSchemes, ExtractSchemeThunder) {
    EXPECT_EQ(UrlUtils::extract_scheme("thunder://abc123"), "thunder");
}

TEST(PrivateProtocolSchemes, ExtractSchemeQQDL) {
    EXPECT_EQ(UrlUtils::extract_scheme("qqlink://abc123"), "qqlink");
}

TEST(PrivateProtocolSchemes, ExtractSchemeFlashget) {
    EXPECT_EQ(UrlUtils::extract_scheme("flashget://abc123"), "flashget");
}

TEST(PrivateProtocolSchemes, ExtractSchemeED2K) {
    EXPECT_EQ(UrlUtils::extract_scheme("ed2k://abc123"), "ed2k");
}

TEST(PrivateProtocolSchemes, IsValidUrlThunder) {
    EXPECT_TRUE(UrlUtils::is_valid_url("thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAWMTIzNDU2Nzg5YWJjZGVmA2Yz"));
}

TEST(PrivateProtocolSchemes, IsValidUrlQQDL) {
    EXPECT_TRUE(UrlUtils::is_valid_url("qqlink://abc123"));
}

TEST(PrivateProtocolSchemes, IsValidUrlFlashget) {
    EXPECT_TRUE(UrlUtils::is_valid_url("flashget://abc123"));
}

TEST(PrivateProtocolSchemes, IsValidUrlED2K) {
    EXPECT_TRUE(UrlUtils::is_valid_url("ed2k://|file|example.zip|12345|abc123|/"));
}

//==============================================================================
// URL 解码测试
//==============================================================================

// Disabled: url_decode not available in UrlUtils
// TEST(UrlDecoding, DecodeSimpleString) {
//     std::string encoded = "hello%20world";
//     std::string decoded = UrlUtils::url_decode(encoded);
//     EXPECT_EQ(decoded, "hello world");
// }

// TEST(UrlDecoding, DecodeSpecialCharacters) {
//     std::string encoded = "hello%3A%20%2F%2F";
//     std::string decoded = UrlUtils::url_decode(encoded);
//     EXPECT_EQ(decoded, "hello: //");
// }

// TEST(UrlDecoding, DecodeSpaces) {
//     std::string encoded = "a+b+c";
//     std::string decoded = UrlUtils::url_decode(encoded);
//     EXPECT_EQ(decoded, "a b c");
// }

// TEST(UrlDecoding, DecodePercentEncoding) {
//     std::string encoded = "%41%42%43";
//     std::string decoded = UrlUtils::url_decode(encoded);
//     EXPECT_EQ(decoded, "ABC");
// }

// TEST(UrlDecoding, DecodeMixed) {
//     std::string encoded = "hello%20world%21";
//     std::string decoded = UrlUtils::url_decode(encoded);
//     EXPECT_EQ(decoded, "hello world!");
// }

// TEST(UrlDecoding, DecodeEmpty) {
//     std::string encoded = "";
//     std::string decoded = UrlUtils::url_decode(encoded);
//     EXPECT_TRUE(decoded.empty());
// }

// TEST(UrlDecoding, DecodeNoEncoding) {
//     std::string encoded = "hello world";
//     std::string decoded = UrlUtils::url_decode(encoded);
//     EXPECT_EQ(decoded, "hello world");
// }

//==============================================================================
// URL 编码测试
//==============================================================================

// TEST(UrlEncoding, EncodeSimpleString) {
//     std::string plain = "hello world";
//     std::string encoded = UrlUtils::url_encode(plain);
//     EXPECT_EQ(encoded, "hello%20world");
// }

// Disabled: url_encode not available in UrlUtils
// TEST(UrlEncoding, EncodeSpecialCharacters) {
//     std::string plain = "hello: //";
//     std::string encoded = UrlUtils::url_encode(plain);
//     EXPECT_NE(encoded.find("%3A"), std::string::npos);
// }

// TEST(UrlEncoding, EncodeSpacesAsPlus) {
//     std::string plain = "a b c";
//     std::string encoded = UrlUtils::url_encode(plain);
//     EXPECT_EQ(encoded, "a+b+c");
// }

// TEST(UrlEncoding, EncodeReservedChars) {
//     std::string plain = "!@#$%^&*()";
//     std::string encoded = UrlUtils::url_encode(plain);
//     EXPECT_NE(encoded, plain);
// }

// TEST(UrlEncoding, EncodeEmpty) {
//     std::string plain = "";
//     std::string encoded = UrlUtils::url_encode(plain);
//     EXPECT_TRUE(encoded.empty());
// }

//==============================================================================
// URL 参数解析测试
//==============================================================================

// Disabled: parse_query_params not available in UrlUtils
// TEST(UrlParameters, ExtractQueryParams) {
//     std::string url = "http://example.com/file?key1=value1&key2=value2";
//     auto params = UrlUtils::parse_query_params(url);
//
//     EXPECT_GE(params.size(), 1u);
// }

// TEST(UrlParameters, ExtractEmptyQuery) {
//     std::string url = "http://example.com/file";
//     auto params = UrlUtils::parse_query_params(url);
//
//     EXPECT_TRUE(params.empty());
// }

// TEST(UrlParameters, ExtractSingleParam) {
//     std::string url = "http://example.com/file?key=value";
//     auto params = UrlUtils::parse_query_params(url);
//
//     EXPECT_GE(params.size(), 1u);
// }

//==============================================================================
// URL 边界条件测试
//==============================================================================

TEST(UrlBoundary, VeryLongUrl) {
    std::string long_url = "http://example.com/" + std::string(10000, 'a') + "/file.zip";
    EXPECT_TRUE(UrlUtils::is_valid_url(long_url));
}

TEST(UrlBoundary, UrlWithOnlyScheme) {
    EXPECT_EQ(UrlUtils::extract_scheme("http://"), "http");
}

TEST(UrlBoundary, UrlWithPort) {
    std::string url = "http://example.com:8080/file.zip";
    EXPECT_TRUE(UrlUtils::is_valid_url(url));
}

TEST(UrlBoundary, UrlWithCredentials) {
    std::string url = "http://user:pass@example.com/file.zip";
    EXPECT_TRUE(UrlUtils::is_valid_url(url));
}

TEST(UrlBoundary, UrlWithIPv4) {
    std::string url = "http://192.168.1.1/file.zip";
    EXPECT_TRUE(UrlUtils::is_valid_url(url));
}

TEST(UrlBoundary, UrlWithIPv6) {
    std::string url = "http://[::1]/file.zip";
    EXPECT_TRUE(UrlUtils::is_valid_url(url));
}

TEST(UrlBoundary, UrlWithFragment) {
    std::string url = "http://example.com/file.zip#section";
    EXPECT_TRUE(UrlUtils::is_valid_url(url));
}

//==============================================================================
// 文件名提取增强测试
//==============================================================================

TEST(FilenameExtraction, ExtractFilenameWithoutExtension) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/file"), "file");
}

TEST(FilenameExtraction, ExtractFilenameMultipleDots) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/file.name.with.dots.zip"),
              "file.name.with.dots.zip");
}

TEST(FilenameExtraction, ExtractFilenameFromRoot) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/"), "download");
}

TEST(FilenameExtraction, ExtractFilenameWithTrailingSlash) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/path/"), "path");
}

TEST(FilenameExtraction, ExtractFilenameWithSpecialCharacters) {
    EXPECT_EQ(UrlUtils::extract_filename("https://example.com/file%20name.zip"),
              "file%20name.zip");
}

//==============================================================================
// 主函数
//==============================================================================
