// Falcon Resource Browser Implementations Unit Tests

#include <falcon/ftp_browser.hpp>
#include <falcon/s3_browser.hpp>

#include <gtest/gtest.h>

TEST(ResourceBrowsersTest, UrlParsers) {
    auto parsed = falcon::S3UrlParser::parse("s3://my-bucket/path/to/key.txt");
    EXPECT_EQ(parsed.bucket, "my-bucket");
    EXPECT_EQ(parsed.key, "path/to/key.txt");

    auto parsed_bucket_only = falcon::S3UrlParser::parse("s3://bucket-only");
    EXPECT_EQ(parsed_bucket_only.bucket, "bucket-only");
    EXPECT_TRUE(parsed_bucket_only.key.empty());
}

TEST(ResourceBrowsersTest, CanHandleSchemes) {
    falcon::FTPBrowser ftp;
    EXPECT_TRUE(ftp.can_handle("ftp://example.com/pub"));
    EXPECT_TRUE(ftp.can_handle("ftps://example.com/pub"));
    EXPECT_FALSE(ftp.can_handle("https://example.com/pub"));

    falcon::S3Browser s3;
    EXPECT_TRUE(s3.can_handle("s3://bucket/key"));
    EXPECT_TRUE(s3.can_handle("https://bucket.s3.us-east-1.amazonaws.com/key"));
    EXPECT_FALSE(s3.can_handle("ftp://example.com/pub"));
}

