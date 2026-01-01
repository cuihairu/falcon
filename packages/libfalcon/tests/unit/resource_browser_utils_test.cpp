// Falcon Resource Browser Utils Unit Tests

#include <falcon/resource_browser.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <cstdint>

//==============================================================================
// FilePermissions 转换测试
//==============================================================================

TEST(ResourceBrowserUtilsTest, FilePermissionsConversion) {
    auto perms = falcon::FilePermissions::from_octal(0644);
    EXPECT_EQ(perms.to_string(), "rw-r--r--");

    auto perms2 = falcon::FilePermissions::from_octal(0755);
    EXPECT_EQ(perms2.to_string(), "rwxr-xr-x");
}

TEST(FilePermissionsDetailed, FromOctal000) {
    auto perms = falcon::FilePermissions::from_octal(0000);
    EXPECT_EQ(perms.to_string(), "---------");
}

TEST(FilePermissionsDetailed, FromOctal777) {
    auto perms = falcon::FilePermissions::from_octal(0777);
    EXPECT_EQ(perms.to_string(), "rwxrwxrwx");
}

TEST(FilePermissionsDetailed, FromOctal644) {
    auto perms = falcon::FilePermissions::from_octal(0644);
    EXPECT_EQ(perms.to_string(), "rw-r--r--");
}

TEST(FilePermissionsDetailed, FromOctal755) {
    auto perms = falcon::FilePermissions::from_octal(0755);
    EXPECT_EQ(perms.to_string(), "rwxr-xr-x");
}

TEST(FilePermissionsDetailed, FromOctal600) {
    auto perms = falcon::FilePermissions::from_octal(0600);
    EXPECT_EQ(perms.to_string(), "rw-------");
}

TEST(FilePermissionsDetailed, FromOctal700) {
    auto perms = falcon::FilePermissions::from_octal(0700);
    EXPECT_EQ(perms.to_string(), "rwx------");
}

TEST(FilePermissionsDetailed, FromOctal666) {
    auto perms = falcon::FilePermissions::from_octal(0666);
    EXPECT_EQ(perms.to_string(), "rw-rw-rw-");
}

TEST(FilePermissionsDetailed, FromOctal444) {
    auto perms = falcon::FilePermissions::from_octal(0444);
    EXPECT_EQ(perms.to_string(), "r--r--r--");
}

TEST(FilePermissionsDetailed, FromOctal222) {
    auto perms = falcon::FilePermissions::from_octal(0222);
    EXPECT_EQ(perms.to_string(), "-w--w--w-");
}

TEST(FilePermissionsDetailed, FromOctal111) {
    auto perms = falcon::FilePermissions::from_octal(0111);
    EXPECT_EQ(perms.to_string(), "--x--x--x");
}

//==============================================================================
// RemoteResource 显示和大小格式化测试
//==============================================================================

TEST(ResourceBrowserUtilsTest, RemoteResourceDisplayAndSize) {
    falcon::RemoteResource dir;
    dir.name = "folder";
    dir.type = falcon::ResourceType::Directory;
    EXPECT_EQ(dir.display_name(), "folder/");
    EXPECT_EQ(dir.formatted_size(), "-");

    falcon::RemoteResource file;
    file.name = "a.bin";
    file.type = falcon::ResourceType::File;
    file.size = 1024;
    EXPECT_EQ(file.display_name(), "a.bin");
    EXPECT_EQ(file.formatted_size(), "1.0 KB");

    falcon::RemoteResource link;
    link.name = "latest";
    link.type = falcon::ResourceType::Symlink;
    link.symlink_target = "a.bin";
    EXPECT_EQ(link.display_name(), "latest -> a.bin");
}

TEST(RemoteResourceDisplay, EmptyName) {
    falcon::RemoteResource resource;
    resource.name = "";
    EXPECT_TRUE(resource.display_name().empty() || resource.display_name() == "/");
}

TEST(RemoteResourceDisplay, NameWithSpaces) {
    falcon::RemoteResource resource;
    resource.name = "file with spaces.txt";
    EXPECT_NE(resource.display_name().find(" "), std::string::npos);
}

TEST(RemoteResourceDisplay, NameWithUnicode) {
    falcon::RemoteResource resource;
    resource.name = "文件.txt";
    EXPECT_FALSE(resource.display_name().empty());
}

TEST(RemoteResourceDisplay, VeryLongName) {
    falcon::RemoteResource resource;
    resource.name = std::string(300, 'a') + ".txt";
    EXPECT_GT(resource.display_name().length(), 100);
}

//==============================================================================
// 文件大小格式化测试
//==============================================================================

TEST(FileSizeFormatting, Bytes) {
    falcon::RemoteResource file;
    file.name = "small.txt";
    file.type = falcon::ResourceType::File;
    file.size = 0;
    EXPECT_EQ(file.formatted_size(), "0 B");

    file.size = 512;
    EXPECT_NE(file.formatted_size().find("B"), std::string::npos);
}

TEST(FileSizeFormatting, Kilobytes) {
    falcon::RemoteResource file;
    file.name = "file.txt";
    file.type = falcon::ResourceType::File;
    file.size = 1024;
    EXPECT_NE(file.formatted_size().find("KB"), std::string::npos);

    file.size = 1536;
    EXPECT_NE(file.formatted_size().find("KB"), std::string::npos);
}

TEST(FileSizeFormatting, Megabytes) {
    falcon::RemoteResource file;
    file.name = "file.bin";
    file.type = falcon::ResourceType::File;
    file.size = 1024 * 1024;
    EXPECT_NE(file.formatted_size().find("MB"), std::string::npos);

    file.size = 5 * 1024 * 1024;
    EXPECT_NE(file.formatted_size().find("MB"), std::string::npos);
}

TEST(FileSizeFormatting, Gigabytes) {
    falcon::RemoteResource file;
    file.name = "large.bin";
    file.type = falcon::ResourceType::File;
    file.size = 1024ULL * 1024ULL * 1024ULL;
    EXPECT_NE(file.formatted_size().find("GB"), std::string::npos);

    file.size = 10ULL * 1024ULL * 1024ULL * 1024ULL;
    EXPECT_NE(file.formatted_size().find("GB"), std::string::npos);
}

TEST(FileSizeFormatting, Terabytes) {
    falcon::RemoteResource file;
    file.name = "huge.bin";
    file.type = falcon::ResourceType::File;
    file.size = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    EXPECT_NE(file.formatted_size().find("TB"), std::string::npos);
}

TEST(FileSizeFormatting, MaxSize) {
    falcon::RemoteResource file;
    file.name = "max.bin";
    file.type = falcon::ResourceType::File;
    file.size = UINT64_MAX;
    EXPECT_FALSE(file.formatted_size().empty());
}

//==============================================================================
// BrowserFormatter 格式化输出测试
//==============================================================================

TEST(ResourceBrowserUtilsTest, BrowserFormatterOutputs) {
    falcon::RemoteResource a;
    a.name = "a";
    a.type = falcon::ResourceType::File;
    a.size = 12;
    a.modified_time = "2025-12-23 10:00:00";
    a.owner = "user";
    a.group = "staff";
    a.permissions = falcon::FilePermissions::from_octal(0644);

    falcon::RemoteResource b;
    b.name = "b";
    b.type = falcon::ResourceType::Directory;
    b.modified_time = "2025-12-23 10:00:00";
    b.owner = "user";
    b.group = "staff";
    b.permissions = falcon::FilePermissions::from_octal(0755);

    std::vector<falcon::RemoteResource> resources{a, b};

    auto short_format = falcon::BrowserFormatter::format_short(resources);
    EXPECT_NE(short_format.find("total 2"), std::string::npos);
    EXPECT_NE(short_format.find("a"), std::string::npos);
    EXPECT_NE(short_format.find("b/"), std::string::npos);

    auto long_format = falcon::BrowserFormatter::format_long(resources);
    EXPECT_NE(long_format.find("Permissions"), std::string::npos);

    auto table_format = falcon::BrowserFormatter::format_table(resources);
    EXPECT_NE(table_format.find("a"), std::string::npos);
    EXPECT_NE(table_format.find("b/"), std::string::npos);
}

TEST(BrowserFormatterDetailed, EmptyList) {
    std::vector<falcon::RemoteResource> empty;

    auto short_format = falcon::BrowserFormatter::format_short(empty);
    auto long_format = falcon::BrowserFormatter::format_long(empty);
    auto table_format = falcon::BrowserFormatter::format_table(empty);

    EXPECT_FALSE(short_format.empty());
    EXPECT_FALSE(long_format.empty());
    EXPECT_FALSE(table_format.empty());
}

TEST(BrowserFormatterDetailed, SingleFile) {
    falcon::RemoteResource file;
    file.name = "test.txt";
    file.type = falcon::ResourceType::File;
    file.size = 100;

    std::vector<falcon::RemoteResource> resources{file};

    auto short_format = falcon::BrowserFormatter::format_short(resources);
    EXPECT_NE(short_format.find("test.txt"), std::string::npos);
}

TEST(BrowserFormatterDetailed, SingleDirectory) {
    falcon::RemoteResource dir;
    dir.name = "testdir";
    dir.type = falcon::ResourceType::Directory;

    std::vector<falcon::RemoteResource> resources{dir};

    auto short_format = falcon::BrowserFormatter::format_short(resources);
    EXPECT_NE(short_format.find("testdir/"), std::string::npos);
}

TEST(BrowserFormatterDetailed, MultipleFiles) {
    std::vector<falcon::RemoteResource> resources;
    for (int i = 0; i < 10; i++) {
        falcon::RemoteResource file;
        file.name = "file" + std::to_string(i) + ".txt";
        file.type = falcon::ResourceType::File;
        file.size = i * 100;
        resources.push_back(file);
    }

    auto format = falcon::BrowserFormatter::format_short(resources);
    EXPECT_NE(format.find("total"), std::string::npos);
}

TEST(BrowserFormatterDetailed, MixedResources) {
    std::vector<falcon::RemoteResource> resources;

    falcon::RemoteResource dir;
    dir.name = "dir";
    dir.type = falcon::ResourceType::Directory;
    resources.push_back(dir);

    falcon::RemoteResource file;
    file.name = "file.txt";
    file.type = falcon::ResourceType::File;
    file.size = 100;
    resources.push_back(file);

    falcon::RemoteResource link;
    link.name = "link";
    link.type = falcon::ResourceType::Symlink;
    link.symlink_target = "file.txt";
    resources.push_back(link);

    auto format = falcon::BrowserFormatter::format_short(resources);
    EXPECT_NE(format.find("dir/"), std::string::npos);
    EXPECT_NE(format.find("file.txt"), std::string::npos);
    EXPECT_NE(format.find("link ->"), std::string::npos);
}

//==============================================================================
// ResourceType 测试
//==============================================================================

TEST(ResourceTypeUtils, IsDirectory) {
    falcon::RemoteResource dir;
    dir.type = falcon::ResourceType::Directory;
    EXPECT_TRUE(dir.is_directory());
    EXPECT_FALSE(dir.is_file());
}

TEST(ResourceTypeUtils, IsFile) {
    falcon::RemoteResource file;
    file.type = falcon::ResourceType::File;
    EXPECT_TRUE(file.is_file());
    EXPECT_FALSE(file.is_directory());
}

TEST(ResourceTypeUtils, IsSymlink) {
    falcon::RemoteResource link;
    link.type = falcon::ResourceType::Symlink;
    EXPECT_FALSE(link.is_directory());
    EXPECT_FALSE(link.is_file());
}

TEST(ResourceTypeUtils, IsBlockDevice) {
    falcon::RemoteResource dev;
    dev.type = falcon::ResourceType::BlockDevice;
    EXPECT_FALSE(dev.is_file());
    EXPECT_FALSE(dev.is_directory());
}

TEST(ResourceTypeUtils, IsCharDevice) {
    falcon::RemoteResource dev;
    dev.type = falcon::ResourceType::CharDevice;
    EXPECT_FALSE(dev.is_file());
    EXPECT_FALSE(dev.is_directory());
}

TEST(ResourceTypeUtils, IsFIFO) {
    falcon::RemoteResource fifo;
    fifo.type = falcon::ResourceType::FIFO;
    EXPECT_FALSE(fifo.is_file());
    EXPECT_FALSE(fifo.is_directory());
}

TEST(ResourceTypeUtils, IsSocket) {
    falcon::RemoteResource socket;
    socket.type = falcon::ResourceType::Socket;
    EXPECT_FALSE(socket.is_file());
    EXPECT_FALSE(socket.is_directory());
}

//==============================================================================
// 资源列表选项测试
//==============================================================================

TEST(ListOptions, DefaultOptions) {
    falcon::ListOptions options;
    EXPECT_FALSE(options.show_hidden);
    EXPECT_FALSE(options.recursive);
    EXPECT_EQ(options.max_depth, 0);
    EXPECT_TRUE(options.sort_by.empty() || options.sort_by == "name");
    EXPECT_FALSE(options.sort_desc);
}

TEST(ListOptions, ShowHidden) {
    falcon::ListOptions options;
    options.show_hidden = true;
    EXPECT_TRUE(options.show_hidden);
}

TEST(ListOptions, Recursive) {
    falcon::ListOptions options;
    options.recursive = true;
    EXPECT_TRUE(options.recursive);
}

TEST(ListOptions, MaxDepth) {
    falcon::ListOptions options;
    options.max_depth = 10;
    EXPECT_EQ(options.max_depth, 10);
}

TEST(ListOptions, SortByName) {
    falcon::ListOptions options;
    options.sort_by = "name";
    EXPECT_EQ(options.sort_by, "name");
}

TEST(ListOptions, SortBySize) {
    falcon::ListOptions options;
    options.sort_by = "size";
    EXPECT_EQ(options.sort_by, "size");
}

TEST(ListOptions, SortByTime) {
    falcon::ListOptions options;
    options.sort_by = "time";
    EXPECT_EQ(options.sort_by, "time");
}

TEST(ListOptions, SortDescending) {
    falcon::ListOptions options;
    options.sort_desc = true;
    EXPECT_TRUE(options.sort_desc);
}

TEST(ListOptions, Filter) {
    falcon::ListOptions options;
    options.filter = "*.txt";
    EXPECT_EQ(options.filter, "*.txt");
}

//==============================================================================
// 资源浏览器工具测试
//==============================================================================

TEST(ResourceBrowserUtils, ValidatePath) {
    EXPECT_TRUE(falcon::ResourceBrowserUtils::is_valid_path("/path/to/file"));
    EXPECT_TRUE(falcon::ResourceBrowserUtils::is_valid_path("relative/path"));
}

TEST(ResourceBrowserUtils, NormalizePath) {
    std::string path = "/path/to/./file";
    auto normalized = falcon::ResourceBrowserUtils::normalize_path(path);
    EXPECT_FALSE(normalized.empty());
}

TEST(ResourceBrowserUtils, JoinPath) {
    std::string base = "/path/to";
    std::string name = "file.txt";
    auto joined = falcon::ResourceBrowserUtils::join_path(base, name);
    EXPECT_NE(joined.find("file.txt"), std::string::npos);
}

TEST(ResourceBrowserUtils, GetParentPath) {
    std::string path = "/path/to/file.txt";
    auto parent = falcon::ResourceBrowserUtils::get_parent_path(path);
    EXPECT_NE(parent.find("/path/to"), std::string::npos);
}

TEST(ResourceBrowserUtils, GetFileName) {
    std::string path = "/path/to/file.txt";
    auto name = falcon::ResourceBrowserUtils::get_filename(path);
    EXPECT_EQ(name, "file.txt");
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(BoundaryConditions, EmptyPath) {
    std::string path = "";
    auto normalized = falcon::ResourceBrowserUtils::normalize_path(path);
    EXPECT_TRUE(normalized.empty() || normalized == ".");
}

TEST(BoundaryConditions, RootPath) {
    std::string path = "/";
    auto normalized = falcon::ResourceBrowserUtils::normalize_path(path);
    EXPECT_EQ(normalized, "/");
}

TEST(BoundaryConditions, TrailingSlash) {
    std::string path = "/path/to/dir/";
    auto normalized = falcon::ResourceBrowserUtils::normalize_path(path);
    EXPECT_EQ(normalized.back(), '/');
}

TEST(BoundaryConditions, MultipleSlashes) {
    std::string path = "path///to////file";
    auto normalized = falcon::ResourceBrowserUtils::normalize_path(path);
    EXPECT_EQ(normalized.find("///"), std::string::npos);
}

TEST(BoundaryConditions, VeryLongPath) {
    std::string path = "/";
    for (int i = 0; i < 100; i++) {
        path += "verylongdirectoryname/";
    }
    EXPECT_GT(path.length(), 1000);
}

TEST(BoundaryConditions, SpecialCharacters) {
    std::string path = "/path/to/file with spaces & special-chars_123.txt";
    EXPECT_NE(path.find(" "), std::string::npos);
    EXPECT_NE(path.find("&"), std::string::npos);
}

//==============================================================================
// 主函数
//==============================================================================
