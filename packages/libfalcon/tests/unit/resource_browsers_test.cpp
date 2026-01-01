// Falcon Resource Browser Implementations Unit Tests

#include <falcon/ftp_browser.hpp>
#include <falcon/s3_browser.hpp>

#include <gtest/gtest.h>

//==============================================================================
// URL 解析测试
//==============================================================================

TEST(ResourceBrowsersTest, UrlParsers) {
    auto parsed = falcon::S3UrlParser::parse("s3://my-bucket/path/to/key.txt");
    EXPECT_EQ(parsed.bucket, "my-bucket");
    EXPECT_EQ(parsed.key, "path/to/key.txt");

    auto parsed_bucket_only = falcon::S3UrlParser::parse("s3://bucket-only");
    EXPECT_EQ(parsed_bucket_only.bucket, "bucket-only");
    EXPECT_TRUE(parsed_bucket_only.key.empty());
}

TEST(S3UrlParserTest, ParseComplexKey) {
    auto parsed = falcon::S3UrlParser::parse("s3://my-bucket/path/to/file with spaces.txt");
    EXPECT_EQ(parsed.bucket, "my-bucket");
    EXPECT_FALSE(parsed.key.empty());
}

TEST(S3UrlParserTest, ParseWithQueryParameters) {
    auto parsed = falcon::S3UrlParser::parse("s3://my-bucket/key.txt?versionId=123");
    EXPECT_EQ(parsed.bucket, "my-bucket");
    // 查询参数的处理取决于实现
}

TEST(S3UrlParserTest,ParseEmptyBucket) {
    auto parsed = falcon::S3UrlParser::parse("s3://");
    EXPECT_TRUE(parsed.bucket.empty());
    EXPECT_TRUE(parsed.key.empty());
}

TEST(S3UrlParserTest, ParseNestedPath) {
    auto parsed = falcon::S3UrlParser::parse("s3://my-bucket/a/b/c/d/e/file.txt");
    EXPECT_EQ(parsed.bucket, "my-bucket");
    EXPECT_EQ(parsed.key, "a/b/c/d/e/file.txt");
}

//==============================================================================
// 协议处理测试
//==============================================================================

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

TEST(ResourceBrowsersTest, CanHandleWithPort) {
    falcon::FTPBrowser ftp;
    EXPECT_TRUE(ftp.can_handle("ftp://example.com:21/pub"));
    EXPECT_TRUE(ftp.can_handle("ftp://example.com:2121/pub"));
}

TEST(ResourceBrowsersTest, CanHandleWithCredentials) {
    falcon::FTPBrowser ftp;
    EXPECT_TRUE(ftp.can_handle("ftp://user:pass@example.com/pub"));
    EXPECT_TRUE(ftp.can_handle("ftp://user@example.com/pub"));
}

TEST(ResourceBrowsersTest, CanHandleIPv4) {
    falcon::FTPBrowser ftp;
    EXPECT_TRUE(ftp.can_handle("ftp://192.168.1.1/pub"));
    EXPECT_TRUE(ftp.can_handle("ftp://user@127.0.0.1/pub"));
}

TEST(ResourceBrowsersTest, CanHandleIPv6) {
    falcon::FTPBrowser ftp;
    EXPECT_TRUE(ftp.can_handle("ftp://[::1]/pub"));
    EXPECT_TRUE(ftp.can_handle("ftp://[2001:db8::1]/pub"));
}

//==============================================================================
// 浏览器名称测试
//==============================================================================

TEST(ResourceBrowserName, GetBrowserNames) {
    falcon::FTPBrowser ftp;
    EXPECT_EQ(ftp.get_name(), "FTP");

    falcon::S3Browser s3;
    EXPECT_EQ(s3.get_name(), "S3");
}

//==============================================================================
// 支持的协议测试
//==============================================================================

TEST(SupportedProtocols, FTPBrowserProtocols) {
    falcon::FTPBrowser ftp;
    auto protocols = ftp.get_supported_protocols();

    EXPECT_FALSE(protocols.empty());
    EXPECT_GE(protocols.size(), 1u);

    // 检查是否包含 ftp
    bool has_ftp = false;
    bool has_ftps = false;
    for (const auto& proto : protocols) {
        if (proto == "ftp") has_ftp = true;
        if (proto == "ftps") has_ftps = true;
    }
    EXPECT_TRUE(has_ftp);
}

TEST(SupportedProtocols, S3BrowserProtocols) {
    falcon::S3Browser s3;
    auto protocols = s3.get_supported_protocols();

    EXPECT_FALSE(protocols.empty());

    // S3 浏览器应该支持 s3 和 https
    bool has_s3 = false;
    for (const auto& proto : protocols) {
        if (proto == "s3") has_s3 = true;
    }
    EXPECT_TRUE(has_s3);
}

//==============================================================================
// FilePermissions 测试
//==============================================================================

TEST(FilePermissions, ToString) {
    falcon::FilePermissions perms;
    perms.owner_read = true;
    perms.owner_write = true;
    perms.owner_execute = true;
    perms.group_read = true;
    perms.other_read = true;

    std::string str = perms.to_string();
    EXPECT_EQ(str, "rwxr--r--");
}

TEST(FilePermissions, ToStringAllPermissions) {
    falcon::FilePermissions perms;
    perms.owner_read = true;
    perms.owner_write = true;
    perms.owner_execute = true;
    perms.group_read = true;
    perms.group_write = true;
    perms.group_execute = true;
    perms.other_read = true;
    perms.other_write = true;
    perms.other_execute = true;

    std::string str = perms.to_string();
    EXPECT_EQ(str, "rwxrwxrwx");
}

TEST(FilePermissions, ToStringNoPermissions) {
    falcon::FilePermissions perms;

    std::string str = perms.to_string();
    EXPECT_EQ(str, "---------");
}

TEST(FilePermissions, FromOctal) {
    auto perms = falcon::FilePermissions::from_octal(755);

    EXPECT_TRUE(perms.owner_read);
    EXPECT_TRUE(perms.owner_write);
    EXPECT_TRUE(perms.owner_execute);
    EXPECT_TRUE(perms.group_read);
    EXPECT_TRUE(perms.group_execute);
    EXPECT_TRUE(perms.other_read);
    EXPECT_FALSE(perms.other_write);
    EXPECT_FALSE(perms.other_execute);
}

TEST(FilePermissions, FromOctal644) {
    auto perms = falcon::FilePermissions::from_octal(644);

    EXPECT_TRUE(perms.owner_read);
    EXPECT_TRUE(perms.owner_write);
    EXPECT_FALSE(perms.owner_execute);
    EXPECT_TRUE(perms.group_read);
    EXPECT_FALSE(perms.group_write);
    EXPECT_FALSE(perms.group_execute);
    EXPECT_TRUE(perms.other_read);
    EXPECT_FALSE(perms.other_write);
    EXPECT_FALSE(perms.other_execute);
}

TEST(FilePermissions, FromOctal777) {
    auto perms = falcon::FilePermissions::from_octal(777);

    EXPECT_TRUE(perms.owner_read);
    EXPECT_TRUE(perms.owner_write);
    EXPECT_TRUE(perms.owner_execute);
    EXPECT_TRUE(perms.group_read);
    EXPECT_TRUE(perms.group_write);
    EXPECT_TRUE(perms.group_execute);
    EXPECT_TRUE(perms.other_read);
    EXPECT_TRUE(perms.other_write);
    EXPECT_TRUE(perms.other_execute);
}

TEST(FilePermissions, FromOctal000) {
    auto perms = falcon::FilePermissions::from_octal(0);

    EXPECT_FALSE(perms.owner_read);
    EXPECT_FALSE(perms.owner_write);
    EXPECT_FALSE(perms.owner_execute);
    EXPECT_FALSE(perms.group_read);
    EXPECT_FALSE(perms.group_write);
    EXPECT_FALSE(perms.group_execute);
    EXPECT_FALSE(perms.other_read);
    EXPECT_FALSE(perms.other_write);
    EXPECT_FALSE(perms.other_execute);
}

//==============================================================================
// RemoteResource 测试
//==============================================================================

TEST(RemoteResource, DefaultConstruction) {
    falcon::RemoteResource resource;

    EXPECT_TRUE(resource.name.empty());
    EXPECT_TRUE(resource.path.empty());
    EXPECT_EQ(resource.type, falcon::ResourceType::Unknown);
    EXPECT_EQ(resource.size, 0u);
}

TEST(RemoteResource, IsDirectory) {
    falcon::RemoteResource resource;
    resource.type = falcon::ResourceType::Directory;

    EXPECT_TRUE(resource.is_directory());
    EXPECT_FALSE(resource.is_file());
}

TEST(RemoteResource, IsFile) {
    falcon::RemoteResource resource;
    resource.type = falcon::ResourceType::File;

    EXPECT_TRUE(resource.is_file());
    EXPECT_FALSE(resource.is_directory());
}

TEST(RemoteResource, IsSymlink) {
    falcon::RemoteResource resource;
    resource.type = falcon::ResourceType::Symlink;

    EXPECT_FALSE(resource.is_directory());
    EXPECT_FALSE(resource.is_file());
}

TEST(RemoteResource, DisplayName) {
    falcon::RemoteResource resource;
    resource.name = "test.txt";

    EXPECT_EQ(resource.display_name(), "test.txt");
}

TEST(RemoteResource, DisplayNameWithDirectory) {
    falcon::RemoteResource resource;
    resource.name = "documents";
    resource.type = falcon::ResourceType::Directory;

    std::string display = resource.display_name();
    EXPECT_FALSE(display.empty());
}

TEST(RemoteResource, FormattedSize) {
    falcon::RemoteResource resource;
    resource.size = 1024;

    std::string formatted = resource.formatted_size();
    EXPECT_FALSE(formatted.empty());
    EXPECT_NE(formatted.find("1024"), std::string::npos);
}

TEST(RemoteResource, FormattedSizeZero) {
    falcon::RemoteResource resource;
    resource.size = 0;

    std::string formatted = resource.formatted_size();
    EXPECT_FALSE(formatted.empty());
}

TEST(RemoteResource, FormattedSizeLargeFile) {
    falcon::RemoteResource resource;
    resource.size = 1024ULL * 1024ULL * 1024ULL;  // 1GB

    std::string formatted = resource.formatted_size();
    EXPECT_FALSE(formatted.empty());
}

//==============================================================================
// ResourceType 测试
//==============================================================================

TEST(ResourceType, AllTypes) {
    falcon::RemoteResource file;
    file.type = falcon::ResourceType::File;
    EXPECT_TRUE(file.is_file());

    falcon::RemoteResource dir;
    dir.type = falcon::ResourceType::Directory;
    EXPECT_TRUE(dir.is_directory());

    falcon::RemoteResource symlink;
    symlink.type = falcon::ResourceType::Symlink;
    EXPECT_FALSE(symlink.is_file());
    EXPECT_FALSE(symlink.is_directory());

    falcon::RemoteResource block;
    block.type = falcon::ResourceType::BlockDevice;
    EXPECT_FALSE(block.is_file());

    falcon::RemoteResource chardev;
    chardev.type = falcon::ResourceType::CharDevice;
    EXPECT_FALSE(chardev.is_file());

    falcon::RemoteResource fifo;
    fifo.type = falcon::ResourceType::FIFO;
    EXPECT_FALSE(fifo.is_file());

    falcon::RemoteResource socket;
    socket.type = falcon::ResourceType::Socket;
    EXPECT_FALSE(socket.is_file());
}

//==============================================================================
// ListOptions 测试
//==============================================================================

TEST(ListOptions, DefaultConstruction) {
    falcon::ListOptions options;

    EXPECT_FALSE(options.show_hidden);
    EXPECT_FALSE(options.recursive);
    EXPECT_EQ(options.max_depth, 0);
    EXPECT_EQ(options.sort_by, "name");
    EXPECT_FALSE(options.sort_desc);
    EXPECT_FALSE(options.include_metadata);
    EXPECT_TRUE(options.filter.empty());
}

TEST(ListOptions, CustomOptions) {
    falcon::ListOptions options;
    options.show_hidden = true;
    options.recursive = true;
    options.max_depth = 5;
    options.sort_by = "size";
    options.sort_desc = true;
    options.include_metadata = true;
    options.filter = "*.txt";

    EXPECT_TRUE(options.show_hidden);
    EXPECT_TRUE(options.recursive);
    EXPECT_EQ(options.max_depth, 5);
    EXPECT_EQ(options.sort_by, "size");
    EXPECT_TRUE(options.sort_desc);
    EXPECT_TRUE(options.include_metadata);
    EXPECT_EQ(options.filter, "*.txt");
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(ResourceBrowserBoundary, EmptyUrl) {
    falcon::FTPBrowser ftp;
    EXPECT_FALSE(ftp.can_handle(""));
}

TEST(ResourceBrowserBoundary, InvalidUrl) {
    falcon::FTPBrowser ftp;
    EXPECT_FALSE(ftp.can_handle("not a url"));
    EXPECT_FALSE(ftp.can_handle("http://"));
    EXPECT_FALSE(ftp.can_handle("://example.com"));
}

TEST(ResourceBrowserBoundary, VeryLongUrl) {
    falcon::FTPBrowser ftp;
    std::string long_url = "ftp://example.com/" + std::string(10000, 'a') + "/file.txt";
    EXPECT_TRUE(ftp.can_handle(long_url));
}

TEST(S3UrlParserBoundary, EmptyUrl) {
    auto parsed = falcon::S3UrlParser::parse("");
    EXPECT_TRUE(parsed.bucket.empty());
    EXPECT_TRUE(parsed.key.empty());
}

TEST(S3UrlParserBoundary, InvalidScheme) {
    auto parsed = falcon::S3UrlParser::parse("http://bucket/key");
    EXPECT_TRUE(parsed.bucket.empty());
}

TEST(S3UrlParserBoundary, OnlyScheme) {
    auto parsed = falcon::S3UrlParser::parse("s3://");
    EXPECT_TRUE(parsed.bucket.empty());
    EXPECT_TRUE(parsed.key.empty());
}

//==============================================================================
// 主函数
//==============================================================================

