// Falcon Resource Browser Utils Unit Tests

#include <falcon/resource_browser.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(ResourceBrowserUtilsTest, FilePermissionsConversion) {
    auto perms = falcon::FilePermissions::from_octal(0644);
    EXPECT_EQ(perms.to_string(), "rw-r--r--");

    auto perms2 = falcon::FilePermissions::from_octal(0755);
    EXPECT_EQ(perms2.to_string(), "rwxr-xr-x");
}

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

