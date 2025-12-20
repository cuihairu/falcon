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
