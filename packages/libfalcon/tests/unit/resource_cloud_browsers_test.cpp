// Falcon Cloud Resource Browsers Unit Tests (requires OpenSSL)

#include <falcon/cos_browser.hpp>
#include <falcon/kodo_browser.hpp>
#include <falcon/oss_browser.hpp>
#include <falcon/upyun_browser.hpp>

#include <gtest/gtest.h>

TEST(ResourceCloudBrowsersTest, OssUrlParser) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://mybucket.oss-cn-beijing.aliyuncs.com/path/to/object.txt");
    EXPECT_EQ(parsed.bucket, "mybucket");
    EXPECT_EQ(parsed.endpoint, "oss-cn-beijing.aliyuncs.com");
    EXPECT_EQ(parsed.region, "cn-beijing");
    EXPECT_EQ(parsed.key, "path/to/object.txt");
}

TEST(ResourceCloudBrowsersTest, CosUrlParser) {
    auto parsed = falcon::COSUrlParser::parse("cos://mybucket-ap-guangzhou/a/b.txt");
    EXPECT_EQ(parsed.bucket, "mybucket");
    EXPECT_EQ(parsed.region, "ap-guangzhou");
    EXPECT_EQ(parsed.key, "a/b.txt");
}

TEST(ResourceCloudBrowsersTest, KodoUrlParser) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://mybucket/path/to/key");
    EXPECT_EQ(parsed.bucket, "mybucket");
    EXPECT_EQ(parsed.key, "path/to/key");

    auto parsed_alias = falcon::KodoUrlParser::parse("qiniu://bucket2/obj");
    EXPECT_EQ(parsed_alias.bucket, "bucket2");
    EXPECT_EQ(parsed_alias.key, "obj");
}

TEST(ResourceCloudBrowsersTest, UpyunUrlParser) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://myspace/path/to/file");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_EQ(parsed.key, "path/to/file");
}

