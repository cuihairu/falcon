/**
 * @file cloud_protocols_test.cpp
 * @brief 云存储协议常量单元测试
 * @author Falcon Team
 * @date 2025-12-27
 */

#include <gtest/gtest.h>
#include <falcon/cloud_url_protocols.hpp>

using namespace falcon::cloud;

//==============================================================================
// 协议常量测试
//==============================================================================

TEST(CloudProtocolsTest, ProtocolPrefixes) {
    // 验证协议前缀长度
    EXPECT_EQ(PROTOCOL_S3, "s3://");
    EXPECT_EQ(PROTOCOL_S3.size(), 5);

    EXPECT_EQ(PROTOCOL_OSS, "oss://");
    EXPECT_EQ(PROTOCOL_OSS.size(), 6);

    EXPECT_EQ(PROTOCOL_COS, "cos://");
    EXPECT_EQ(PROTOCOL_COS.size(), 6);

    EXPECT_EQ(PROTOCOL_KODO, "kodo://");
    EXPECT_EQ(PROTOCOL_KODO.size(), 7);

    EXPECT_EQ(PROTOCOL_QINIU, "qiniu://");
    EXPECT_EQ(PROTOCOL_QINIU.size(), 8);

    EXPECT_EQ(PROTOCOL_UPYUN, "upyun://");
    EXPECT_EQ(PROTOCOL_UPYUN.size(), 8);
}

TEST(CloudProtocolsTest, StartsWithProtocol) {
    // 正确的协议
    EXPECT_TRUE(starts_with_protocol("s3://bucket/key", PROTOCOL_S3));
    EXPECT_TRUE(starts_with_protocol("oss://bucket/key", PROTOCOL_OSS));
    EXPECT_TRUE(starts_with_protocol("cos://bucket/key", PROTOCOL_COS));
    EXPECT_TRUE(starts_with_protocol("kodo://bucket/key", PROTOCOL_KODO));
    EXPECT_TRUE(starts_with_protocol("qiniu://bucket/key", PROTOCOL_QINIU));
    EXPECT_TRUE(starts_with_protocol("upyun://bucket/key", PROTOCOL_UPYUN));

    // 错误的协议
    EXPECT_FALSE(starts_with_protocol("s3://bucket/key", PROTOCOL_OSS));
    EXPECT_FALSE(starts_with_protocol("http://bucket/key", PROTOCOL_S3));
    EXPECT_FALSE(starts_with_protocol("bucket/key", PROTOCOL_S3));

    // 空字符串
    EXPECT_FALSE(starts_with_protocol("", PROTOCOL_S3));
}

TEST(CloudProtocolsTest, SkipProtocol) {
    // 正确的协议
    EXPECT_EQ(skip_protocol("s3://bucket/key", PROTOCOL_S3), 5);
    EXPECT_EQ(skip_protocol("oss://bucket/key", PROTOCOL_OSS), 6);
    EXPECT_EQ(skip_protocol("cos://bucket/key", PROTOCOL_COS), 6);
    EXPECT_EQ(skip_protocol("kodo://bucket/key", PROTOCOL_KODO), 7);
    EXPECT_EQ(skip_protocol("qiniu://bucket/key", PROTOCOL_QINIU), 8);
    EXPECT_EQ(skip_protocol("upyun://bucket/key", PROTOCOL_UPYUN), 8);

    // 错误的协议返回 npos
    EXPECT_EQ(skip_protocol("oss://bucket/key", PROTOCOL_S3), std::string_view::npos);
    EXPECT_EQ(skip_protocol("http://bucket/key", PROTOCOL_S3), std::string_view::npos);
}

TEST(CloudProtocolsTest, ParseBucketAndKey) {
    // 简单情况
    {
        auto [bucket, key] = parse_bucket_and_key("s3://mybucket/path/to/file.txt", PROTOCOL_S3);
        EXPECT_EQ(bucket, "mybucket");
        EXPECT_EQ(key, "path/to/file.txt");
    }

    // 只有 bucket，没有 key
    {
        auto [bucket, key] = parse_bucket_and_key("oss://mybucket", PROTOCOL_OSS);
        EXPECT_EQ(bucket, "mybucket");
        EXPECT_TRUE(key.empty());
    }

    // OSS 带端点的格式
    {
        auto [bucket, key] = parse_bucket_and_key(
            "oss://mybucket.oss-cn-beijing.aliyuncs.com/path/to/object.txt",
            PROTOCOL_OSS
        );
        EXPECT_EQ(bucket, "mybucket.oss-cn-beijing.aliyuncs.com");
        EXPECT_EQ(key, "path/to/object.txt");
    }

    // COS 带 region 的格式
    {
        auto [bucket, key] = parse_bucket_and_key(
            "cos://mybucket-ap-guangzhou/path/to/file.txt",
            PROTOCOL_COS
        );
        EXPECT_EQ(bucket, "mybucket-ap-guangzhou");
        EXPECT_EQ(key, "path/to/file.txt");
    }
}

TEST(CloudProtocolsTest, ExtractBucket) {
    EXPECT_EQ(extract_bucket("s3://mybucket/path/to/file.txt", PROTOCOL_S3), "mybucket");
    EXPECT_EQ(extract_bucket("oss://mybucket/path/to/file.txt", PROTOCOL_OSS), "mybucket");
    EXPECT_EQ(extract_bucket("cos://mybucket/path/to/file.txt", PROTOCOL_COS), "mybucket");
    EXPECT_EQ(extract_bucket("kodo://mybucket/path/to/file.txt", PROTOCOL_KODO), "mybucket");
    EXPECT_EQ(extract_bucket("upyun://myspace/path/to/file.txt", PROTOCOL_UPYUN), "myspace");
}

TEST(CloudProtocolsTest, ExtractKey) {
    EXPECT_EQ(extract_key("s3://mybucket/path/to/file.txt", PROTOCOL_S3), "path/to/file.txt");
    EXPECT_EQ(extract_key("oss://mybucket/a/b/c.txt", PROTOCOL_OSS), "a/b/c.txt");
    EXPECT_EQ(extract_key("cos://mybucket/file.txt", PROTOCOL_COS), "file.txt");
    EXPECT_EQ(extract_key("kodo://mybucket/key", PROTOCOL_KODO), "key");
    EXPECT_EQ(extract_key("upyun://myspace/path/to/file", PROTOCOL_UPYUN), "path/to/file");
}

TEST(CloudProtocolsTest, DetectProtocol) {
    EXPECT_EQ(detect_protocol("s3://bucket/key"), PROTOCOL_S3);
    EXPECT_EQ(detect_protocol("oss://bucket/key"), PROTOCOL_OSS);
    EXPECT_EQ(detect_protocol("cos://bucket/key"), PROTOCOL_COS);
    EXPECT_EQ(detect_protocol("kodo://bucket/key"), PROTOCOL_KODO);
    EXPECT_EQ(detect_protocol("qiniu://bucket/key"), PROTOCOL_QINIU);
    EXPECT_EQ(detect_protocol("upyun://bucket/key"), PROTOCOL_UPYUN);

    // 未知协议
    EXPECT_EQ(detect_protocol("http://bucket/key"), "");
    EXPECT_EQ(detect_protocol("ftp://bucket/key"), "");
    EXPECT_EQ(detect_protocol("bucket/key"), "");
}

//==============================================================================
// 边界情况测试
//==============================================================================

TEST(CloudProtocolsTest, EdgeCases) {
    // URL 太短
    EXPECT_EQ(skip_protocol("s3:/", PROTOCOL_S3), std::string_view::npos);
    EXPECT_EQ(skip_protocol("s3://", PROTOCOL_S3), 5);  // 协议本身
    EXPECT_EQ(skip_protocol("oss://", PROTOCOL_OSS), 6);

    // 协议匹配但不包含 bucket
    EXPECT_EQ(skip_protocol("s3://", PROTOCOL_S3), 5);
    EXPECT_EQ(skip_protocol("oss://", PROTOCOL_OSS), 6);

    // 协议后直接跟 /
    EXPECT_EQ(skip_protocol("s3:///", PROTOCOL_S3), 5);
    EXPECT_EQ(skip_protocol("oss:///", PROTOCOL_OSS), 6);
}

TEST(CloudProtocolsTest, ComplexUrls) {
    // 嵌套路径
    {
        auto [bucket, key] = parse_bucket_and_key(
            "s3://mybucket/path/to/deep/nested/file.txt",
            PROTOCOL_S3
        );
        EXPECT_EQ(bucket, "mybucket");
        EXPECT_EQ(key, "path/to/deep/nested/file.txt");
    }

    // 特殊字符
    {
        auto [bucket, key] = parse_bucket_and_key(
            "oss://mybucket/path/with spaces/file.txt",
            PROTOCOL_OSS
        );
        EXPECT_EQ(bucket, "mybucket");
        EXPECT_EQ(key, "path/with spaces/file.txt");
    }

    // 多个连续的 /
    {
        auto [bucket, key] = parse_bucket_and_key(
            "s3://mybucket///key",
            PROTOCOL_S3
        );
        EXPECT_EQ(bucket, "mybucket");
        EXPECT_EQ(key, "//key");
    }
}
