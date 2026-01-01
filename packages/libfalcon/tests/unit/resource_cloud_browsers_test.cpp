// Falcon Cloud Resource Browsers Unit Tests (requires OpenSSL)

#include <falcon/cos_browser.hpp>
#include <falcon/kodo_browser.hpp>
#include <falcon/oss_browser.hpp>
#include <falcon/upyun_browser.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

//==============================================================================
// OSS (阿里云对象存储) URL 解析测试
//==============================================================================

TEST(OssUrlParser, StandardUrl) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://mybucket.oss-cn-beijing.aliyuncs.com/path/to/object.txt");
    EXPECT_EQ(parsed.bucket, "mybucket");
    EXPECT_EQ(parsed.endpoint, "oss-cn-beijing.aliyuncs.com");
    EXPECT_EQ(parsed.region, "cn-beijing");
    EXPECT_EQ(parsed.key, "path/to/object.txt");
}

TEST(OssUrlParser, DifferentRegions) {
    auto beijing = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/file.txt");
    EXPECT_EQ(beijing.region, "cn-beijing");

    auto hangzhou = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-hangzhou.aliyuncs.com/file.txt");
    EXPECT_EQ(hangzhou.region, "cn-hangzhou");

    auto shanghai = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-shanghai.aliyuncs.com/file.txt");
    EXPECT_EQ(shanghai.region, "cn-shanghai");

    auto qingdao = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-qingdao.aliyuncs.com/file.txt");
    EXPECT_EQ(qingdao.region, "cn-qingdao");

    auto shenzhen = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-shenzhen.aliyuncs.com/file.txt");
    EXPECT_EQ(shenzhen.region, "cn-shenzhen");
}

TEST(OssUrlParser, InternationalRegions) {
    auto hongkong = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-hongkong.aliyuncs.com/file.txt");
    EXPECT_EQ(hongkong.region, "cn-hongkong");

    auto uswest = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-us-west-1.aliyuncs.com/file.txt");
    EXPECT_EQ(uswest.region, "us-west-1");

    auto useast = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-us-east-1.aliyuncs.com/file.txt");
    EXPECT_EQ(useast.region, "us-east-1");

    auto singapore = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-ap-southeast-1.aliyuncs.com/file.txt");
    EXPECT_EQ(singapore.region, "ap-southeast-1");

    auto tokyo = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-ap-northeast-1.aliyuncs.com/file.txt");
    EXPECT_EQ(tokyo.region, "ap-northeast-1");
}

TEST(OssUrlParser, RootPath) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_TRUE(parsed.key == "/" || parsed.key.empty());
}

TEST(OssUrlParser, NoPath) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_TRUE(parsed.key.empty());
}

TEST(OssUrlParser, NestedPath) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/a/b/c/d/e/file.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_EQ(parsed.key, "a/b/c/d/e/file.txt");
}

TEST(OssUrlParser, SpecialCharactersInPath) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/path/to/file_with-special.name.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_NE(parsed.key.find("-"), std::string::npos);
    EXPECT_NE(parsed.key.find("_"), std::string::npos);
    EXPECT_NE(parsed.key.find("."), std::string::npos);
}

TEST(OssUrlParser, ChinesePath) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/路径/文件.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_NE(parsed.key.find("路径"), std::string::npos);
}

TEST(OssUrlParser, QueryParameters) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/file.txt?versionId=123");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_NE(parsed.key.find("file.txt"), std::string::npos);
}

TEST(OssUrlParser, MultipleExtensions) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/archive.tar.gz");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_EQ(parsed.key, "archive.tar.gz");
}

TEST(OssUrlParser, BucketWithNumbers) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://bucket123.oss-cn-beijing.aliyuncs.com/file.txt");
    EXPECT_EQ(parsed.bucket, "bucket123");
}

TEST(OssUrlParser, BucketWithHyphens) {
    auto parsed = falcon::OSSUrlParser::parse(
        "oss://my-bucket.oss-cn-beijing.aliyuncs.com/file.txt");
    EXPECT_EQ(parsed.bucket, "my-bucket");
}

//==============================================================================
// COS (腾讯云对象存储) URL 解析测试
//==============================================================================

TEST(CosUrlParser, StandardUrl) {
    auto parsed = falcon::COSUrlParser::parse("cos://mybucket-ap-guangzhou/a/b.txt");
    EXPECT_EQ(parsed.bucket, "mybucket");
    EXPECT_EQ(parsed.region, "ap-guangzhou");
    EXPECT_EQ(parsed.key, "a/b.txt");
}

TEST(CosUrlParser, DifferentRegions) {
    auto guangzhou = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/file.txt");
    EXPECT_EQ(guangzhou.region, "ap-guangzhou");

    auto beijing = falcon::COSUrlParser::parse("cos://bucket-ap-beijing/file.txt");
    EXPECT_EQ(beijing.region, "ap-beijing");

    auto shanghai = falcon::COSUrlParser::parse("cos://bucket-ap-shanghai/file.txt");
    EXPECT_EQ(shanghai.region, "ap-shanghai");

    auto chengdu = falcon::COSUrlParser::parse("cos://bucket-ap-chengdu/file.txt");
    EXPECT_EQ(chengdu.region, "ap-chengdu");

    auto hongkong = falcon::COSUrlParser::parse("cos://bucket-ap-hongkong/file.txt");
    EXPECT_EQ(hongkong.region, "ap-hongkong");

    auto singapore = falcon::COSUrlParser::parse("cos://bucket-ap-singapore/file.txt");
    EXPECT_EQ(singapore.region, "ap-singapore");

    auto tokyo = falcon::COSUrlParser::parse("cos://bucket-ap-tokyo/file.txt");
    EXPECT_EQ(tokyo.region, "ap-tokyo");

    auto frankfurt = falcon::COSUrlParser::parse("cos://bucket-eu-frankfurt/file.txt");
    EXPECT_EQ(frankfurt.region, "eu-frankfurt");

    auto virginia = falcon::COSUrlParser::parse("cos://bucket-na-virginia/file.txt");
    EXPECT_EQ(virginia.region, "na-virginia");

    auto toronto = falcon::COSUrlParser::parse("cos://bucket-na-toronto/file.txt");
    EXPECT_EQ(toronto.region, "na-toronto");
}

TEST(CosUrlParser, RootPath) {
    auto parsed = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_TRUE(parsed.key == "/" || parsed.key.empty());
}

TEST(CosUrlParser, NoPath) {
    auto parsed = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_TRUE(parsed.key.empty());
}

TEST(CosUrlParser, NestedPath) {
    auto parsed = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/a/b/c/d/e/file.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_EQ(parsed.key, "a/b/c/d/e/file.txt");
}

TEST(CosUrlParser, SpecialCharactersInPath) {
    auto parsed = falcon::COSUrlParser::parse(
        "cos://bucket-ap-guangzhou/path/to/file_with-special.name.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_NE(parsed.key.find("-"), std::string::npos);
    EXPECT_NE(parsed.key.find("_"), std::string::npos);
}

TEST(CosUrlParser, ChinesePath) {
    auto parsed = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/路径/文件.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_NE(parsed.key.find("路径"), std::string::npos);
}

TEST(CosUrlParser, BucketWithNumbers) {
    auto parsed = falcon::COSUrlParser::parse("cos://bucket123-ap-guangzhou/file.txt");
    EXPECT_EQ(parsed.bucket, "bucket123");
}

TEST(CosUrlParser, BucketWithHyphens) {
    auto parsed = falcon::COSUrlParser::parse("cos://my-bucket-ap-guangzhou/file.txt");
    EXPECT_EQ(parsed.bucket, "my-bucket");
}

TEST(CosUrlParser, MultipleExtensions) {
    auto parsed = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/archive.tar.gz");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_EQ(parsed.key, "archive.tar.gz");
}

//==============================================================================
// Kodo (七牛云存储) URL 解析测试
//==============================================================================

TEST(KodoUrlParser, StandardUrl) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://mybucket/path/to/key");
    EXPECT_EQ(parsed.bucket, "mybucket");
    EXPECT_EQ(parsed.key, "path/to/key");
}

TEST(KodoUrlParser, AliasUrl) {
    auto parsed_alias = falcon::KodoUrlParser::parse("qiniu://bucket2/obj");
    EXPECT_EQ(parsed_alias.bucket, "bucket2");
    EXPECT_EQ(parsed_alias.key, "obj");
}

TEST(KodoUrlParser, RootPath) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://bucket/");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_TRUE(parsed.key == "/" || parsed.key.empty());
}

TEST(KodoUrlParser, NoPath) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://bucket");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_TRUE(parsed.key.empty());
}

TEST(KodoUrlParser, NestedPath) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://bucket/a/b/c/d/e/file.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_EQ(parsed.key, "a/b/c/d/e/file.txt");
}

TEST(KodoUrlParser, SpecialCharactersInPath) {
    auto parsed = falcon::KodoUrlParser::parse(
        "kodo://bucket/path/to/file_with-special.name.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_NE(parsed.key.find("-"), std::string::npos);
    EXPECT_NE(parsed.key.find("_"), std::string::npos);
}

TEST(KodoUrlParser, ChinesePath) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://bucket/路径/文件.txt");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_NE(parsed.key.find("路径"), std::string::npos);
}

TEST(KodoUrlParser, BucketWithNumbers) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://bucket123/file.txt");
    EXPECT_EQ(parsed.bucket, "bucket123");
}

TEST(KodoUrlParser, BucketWithHyphens) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://my-bucket/file.txt");
    EXPECT_EQ(parsed.bucket, "my-bucket");
}

TEST(KodoUrlParser, MultipleExtensions) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://bucket/archive.tar.gz");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_EQ(parsed.key, "archive.tar.gz");
}

TEST(KodoUrlParser, QueryParameters) {
    auto parsed = falcon::KodoUrlParser::parse("kodo://bucket/file.txt?versionId=123");
    EXPECT_EQ(parsed.bucket, "bucket");
    EXPECT_NE(parsed.key.find("file.txt"), std::string::npos);
}

//==============================================================================
// Upyun (又拍云存储) URL 解析测试
//==============================================================================

TEST(UpyunUrlParser, StandardUrl) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://myspace/path/to/file");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_EQ(parsed.key, "path/to/file");
}

TEST(UpyunUrlParser, RootPath) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://myspace/");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_TRUE(parsed.key == "/" || parsed.key.empty());
}

TEST(UpyunUrlParser, NoPath) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://myspace");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_TRUE(parsed.key.empty());
}

TEST(UpyunUrlParser, NestedPath) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://myspace/a/b/c/d/e/file.txt");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_EQ(parsed.key, "a/b/c/d/e/file.txt");
}

TEST(UpyunUrlParser, SpecialCharactersInPath) {
    auto parsed = falcon::UpyunUrlParser::parse(
        "upyun://myspace/path/to/file_with-special.name.txt");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_NE(parsed.key.find("-"), std::string::npos);
    EXPECT_NE(parsed.key.find("_"), std::string::npos);
}

TEST(UpyunUrlParser, ChinesePath) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://myspace/路径/文件.txt");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_NE(parsed.key.find("路径"), std::string::npos);
}

TEST(UpyunUrlParser, BucketWithNumbers) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://space123/file.txt");
    EXPECT_EQ(parsed.bucket, "space123");
}

TEST(UpyunUrlParser, BucketWithHyphens) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://my-space/file.txt");
    EXPECT_EQ(parsed.bucket, "my-space");
}

TEST(UpyunUrlParser, MultipleExtensions) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://myspace/archive.tar.gz");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_EQ(parsed.key, "archive.tar.gz");
}

TEST(UpyunUrlParser, QueryParameters) {
    auto parsed = falcon::UpyunUrlParser::parse("upyun://myspace/file.txt?versionId=123");
    EXPECT_EQ(parsed.bucket, "myspace");
    EXPECT_NE(parsed.key.find("file.txt"), std::string::npos);
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(CloudStorageBoundaryConditions, EmptyBucket) {
    // OSS
    auto oss = falcon::OSSUrlParser::parse("oss://.oss-cn-beijing.aliyuncs.com/file.txt");
    EXPECT_TRUE(oss.bucket.empty());

    // COS
    auto cos = falcon::COSUrlParser::parse("cos://-ap-guangzhou/file.txt");
    EXPECT_TRUE(cos.bucket.empty());

    // Kodo
    auto kodo = falcon::KodoUrlParser::parse("kodo:///file.txt");
    EXPECT_TRUE(kodo.bucket.empty());

    // Upyun
    auto upyun = falcon::UpyunUrlParser::parse("upyun:///file.txt");
    EXPECT_TRUE(upyun.bucket.empty());
}

TEST(CloudStorageBoundaryConditions, VeryLongPath) {
    std::string long_path = "/";
    for (int i = 0; i < 100; i++) {
        long_path += "verylongdirectoryname/";
    }
    long_path += "file.txt";

    // OSS
    auto oss = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com" + long_path);
    EXPECT_EQ(oss.bucket, "bucket");
    EXPECT_GT(oss.key.length(), 1000);

    // COS
    auto cos = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou" + long_path);
    EXPECT_EQ(cos.bucket, "bucket");
    EXPECT_GT(cos.key.length(), 1000);

    // Kodo
    auto kodo = falcon::KodoUrlParser::parse("kodo://bucket" + long_path);
    EXPECT_EQ(kodo.bucket, "bucket");
    EXPECT_GT(kodo.key.length(), 1000);

    // Upyun
    auto upyun = falcon::UpyunUrlParser::parse("upyun://bucket" + long_path);
    EXPECT_EQ(upyun.bucket, "bucket");
    EXPECT_GT(upyun.key.length(), 1000);
}

TEST(CloudStorageBoundaryConditions, PathWithSlashes) {
    // Multiple consecutive slashes
    auto oss = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com///path///to///file.txt");
    EXPECT_EQ(oss.bucket, "bucket");

    auto cos = falcon::COSUrlParser::parse(
        "cos://bucket-ap-guangzhou///path///to///file.txt");
    EXPECT_EQ(cos.bucket, "bucket");

    auto kodo = falcon::KodoUrlParser::parse("kodo://bucket///path///to///file.txt");
    EXPECT_EQ(kodo.bucket, "bucket");

    auto upyun = falcon::UpyunUrlParser::parse("upyun://bucket///path///to///file.txt");
    EXPECT_EQ(upyun.bucket, "bucket");
}

TEST(CloudStorageBoundaryConditions, UrlWithSpaces) {
    auto oss = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/path/to/file%20with%20spaces.txt");
    EXPECT_EQ(oss.bucket, "bucket");
    EXPECT_NE(oss.key.find("%20"), std::string::npos);
}

//==============================================================================
// 云存储特性测试
//==============================================================================

TEST(CloudStorageFeatures, OssDifferentEndpoints) {
    auto standard = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing.aliyuncs.com/file.txt");
    EXPECT_EQ(standard.endpoint, "oss-cn-beijing.aliyuncs.com");

    auto internal = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-cn-beijing-internal.aliyuncs.com/file.txt");
    EXPECT_EQ(internal.endpoint, "oss-cn-beijing-internal.aliyuncs.com");

    auto accelerate = falcon::OSSUrlParser::parse(
        "oss://bucket.oss-accelerate.aliyuncs.com/file.txt");
    EXPECT_EQ(accelerate.endpoint, "oss-accelerate.aliyuncs.com");
}

TEST(CloudStorageFeatures, CosDifferentAppIds) {
    auto cos1 = falcon::COSUrlParser::parse("cos://1234567890-ap-guangzhou/my-bucket/file.txt");
    EXPECT_EQ(cos1.bucket, "1234567890-my-bucket");

    auto cos2 = falcon::COSUrlParser::parse("cos://mybucket-1234567890-ap-guangzhou/file.txt");
    EXPECT_NE(cos2.bucket.find("1234567890"), std::string::npos);
}

TEST(CloudStorageFeatures, KodoDomainAliases) {
    auto kodo = falcon::KodoUrlParser::parse("kodo://bucket/file.txt");
    EXPECT_EQ(kodo.bucket, "bucket");

    auto qiniu = falcon::KodoUrlParser::parse("qiniu://bucket/file.txt");
    EXPECT_EQ(qiniu.bucket, "bucket");
}

TEST(CloudStorageFeatures, UpyunServiceTypes) {
    auto telecomb = falcon::UpyunUrlParser::parse("upyun://bucket/file.txt");
    EXPECT_EQ(telecomb.bucket, "bucket");

    auto cmcc = falcon::UpyunUrlParser::parse("upyun://bucket/file.txt");
    EXPECT_EQ(cmcc.bucket, "bucket");

    auto unicom = falcon::UpyunUrlParser::parse("upyun://bucket/file.txt");
    EXPECT_EQ(unicom.bucket, "bucket");
}

//==============================================================================
// 文件类型测试
//==============================================================================

TEST(CloudStorageFileTypes, Images) {
    auto oss = falcon::OSSUrlParser::parse("oss://bucket.oss-cn-beijing.aliyuncs.com/image.jpg");
    EXPECT_EQ(oss.key, "image.jpg");

    auto cos = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/image.png");
    EXPECT_EQ(cos.key, "image.png");

    auto kodo = falcon::KodoUrlParser::parse("kodo://bucket/image.gif");
    EXPECT_EQ(kodo.key, "image.gif");

    auto upyun = falcon::UpyunUrlParser::parse("upyun://bucket/image.webp");
    EXPECT_EQ(upyun.key, "image.webp");
}

TEST(CloudStorageFileTypes, Videos) {
    auto oss = falcon::OSSUrlParser::parse("oss://bucket.oss-cn-beijing.aliyuncs.com/video.mp4");
    EXPECT_EQ(oss.key, "video.mp4");

    auto cos = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/video.avi");
    EXPECT_EQ(cos.key, "video.avi");

    auto kodo = falcon::KodoUrlParser::parse("kodo://bucket/video.mkv");
    EXPECT_EQ(kodo.key, "video.mkv");

    auto upyun = falcon::UpyunUrlParser::parse("upyun://bucket/video.mov");
    EXPECT_EQ(upyun.key, "video.mov");
}

TEST(CloudStorageFileTypes, Archives) {
    auto oss = falcon::OSSUrlParser::parse("oss://bucket.oss-cn-beijing.aliyuncs.com/archive.zip");
    EXPECT_EQ(oss.key, "archive.zip");

    auto cos = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/archive.tar.gz");
    EXPECT_EQ(cos.key, "archive.tar.gz");

    auto kodo = falcon::KodoUrlParser::parse("kodo://bucket/archive.7z");
    EXPECT_EQ(kodo.key, "archive.7z");

    auto upyun = falcon::UpyunUrlParser::parse("upyun://bucket/archive.rar");
    EXPECT_EQ(upyun.key, "archive.rar");
}

TEST(CloudStorageFileTypes, Documents) {
    auto oss = falcon::OSSUrlParser::parse("oss://bucket.oss-cn-beijing.aliyuncs.com/doc.pdf");
    EXPECT_EQ(oss.key, "doc.pdf");

    auto cos = falcon::COSUrlParser::parse("cos://bucket-ap-guangzhou/doc.docx");
    EXPECT_EQ(cos.key, "doc.docx");

    auto kodo = falcon::KodoUrlParser::parse("kodo://bucket/doc.xlsx");
    EXPECT_EQ(kodo.key, "doc.xlsx");

    auto upyun = falcon::UpyunUrlParser::parse("upyun://bucket/doc.pptx");
    EXPECT_EQ(upyun.key, "doc.pptx");
}

//==============================================================================
// 错误处理测试
//==============================================================================

TEST(CloudStorageErrorHandling, InvalidUrls) {
    // Missing protocol
    EXPECT_THROW(falcon::OSSUrlParser::parse("bucket.oss-cn-beijing.aliyuncs.com/file.txt"),
                 std::exception);

    // Empty URL
    EXPECT_THROW(falcon::COSUrlParser::parse(""), std::exception);

    // Only protocol
    EXPECT_THROW(falcon::KodoUrlParser::parse("kodo://"), std::exception);

    // Missing bucket
    EXPECT_THROW(falcon::UpyunUrlParser::parse("upyun:///file.txt"), std::exception);
}

TEST(CloudStorageErrorHandling, MalformedUrls) {
    // Invalid characters
    EXPECT_THROW(falcon::OSSUrlParser::parse("oss://bucket@oss-cn-beijing.aliyuncs.com/file.txt"),
                 std::exception);

    // Missing region
    EXPECT_THROW(falcon::COSUrlParser::parse("cos://bucket-/file.txt"), std::exception);
}

//==============================================================================
// 主函数
//==============================================================================

