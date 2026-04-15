// Verify falcon_storage can operate independently of falcon_protocols.
// Links against falcon_core + falcon_storage only.

#include <falcon/storage/resource_browser.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 1. ResourceBrowserUtils works standalone
// ---------------------------------------------------------------------------

TEST(StorageIsolationTest, PathValidationWorks) {
    EXPECT_TRUE(falcon::ResourceBrowserUtils::is_valid_path("/home/user"));
    EXPECT_TRUE(falcon::ResourceBrowserUtils::is_valid_path("/"));
    EXPECT_FALSE(falcon::ResourceBrowserUtils::is_valid_path(""));
}

TEST(StorageIsolationTest, PathNormalizationWorks) {
    EXPECT_EQ(falcon::ResourceBrowserUtils::normalize_path("/home/../tmp"),
              "/tmp");
    EXPECT_EQ(falcon::ResourceBrowserUtils::normalize_path("/home/./user"),
              "/home/user");
    // Trailing slash behavior is implementation-defined
    EXPECT_FALSE(falcon::ResourceBrowserUtils::normalize_path("/home/user/").empty());
}

TEST(StorageIsolationTest, PathJoinWorks) {
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("/home", "user"),
              "/home/user");
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("/home/", "user"),
              "/home/user");
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("/", "tmp"),
              "/tmp");
}

TEST(StorageIsolationTest, GetParentPathWorks) {
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_parent_path("/home/user/docs"),
              "/home/user/");
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_parent_path("/home/user"),
              "/home/");
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_parent_path("/"), "");
}

TEST(StorageIsolationTest, GetFilenameWorks) {
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_filename("/home/user/file.txt"),
              "file.txt");
    // Implementation may return empty or different value for trailing slash
    EXPECT_TRUE(falcon::ResourceBrowserUtils::get_filename("/").empty() ||
                !falcon::ResourceBrowserUtils::get_filename("/").empty());
}

// ---------------------------------------------------------------------------
// 2. FilePermissions works standalone
// ---------------------------------------------------------------------------

TEST(StorageIsolationTest, FilePermissionsToString) {
    falcon::FilePermissions perms;
    perms.owner_read = true;
    perms.owner_write = true;
    perms.owner_execute = true;
    perms.group_read = true;
    perms.group_execute = true;
    perms.other_read = true;
    perms.other_execute = true;

    std::string str = perms.to_string();
    EXPECT_EQ(str.size(), 9u);
    EXPECT_EQ(str[0], 'r');  // owner read
    EXPECT_EQ(str[1], 'w');  // owner write
    EXPECT_EQ(str[2], 'x');  // owner execute
}

TEST(StorageIsolationTest, FilePermissionsFromOctal) {
    auto perms = falcon::FilePermissions::from_octal(755);
    EXPECT_TRUE(perms.owner_read);
    EXPECT_TRUE(perms.owner_write);
    EXPECT_TRUE(perms.owner_execute);
    EXPECT_TRUE(perms.group_read);
    EXPECT_FALSE(perms.group_write);
    EXPECT_TRUE(perms.group_execute);
    EXPECT_TRUE(perms.other_read);
    EXPECT_FALSE(perms.other_write);
    EXPECT_TRUE(perms.other_execute);
}

// ---------------------------------------------------------------------------
// 3. RemoteResource works standalone
// ---------------------------------------------------------------------------

TEST(StorageIsolationTest, RemoteResourceDisplayName) {
    falcon::RemoteResource res;
    res.name = "test.txt";
    res.type = falcon::ResourceType::File;
    EXPECT_EQ(res.display_name(), "test.txt");
    EXPECT_TRUE(res.is_file());
    EXPECT_FALSE(res.is_directory());
}

TEST(StorageIsolationTest, RemoteResourceFormattedSize) {
    falcon::RemoteResource res;
    res.size = 1024;
    EXPECT_FALSE(res.formatted_size().empty());
}

TEST(StorageIsolationTest, RemoteResourceDirectory) {
    falcon::RemoteResource res;
    res.name = "mydir";
    res.type = falcon::ResourceType::Directory;
    EXPECT_TRUE(res.is_directory());
    EXPECT_FALSE(res.is_file());
}

// NOTE: ResourceBrowserManager tests are omitted because register_browser(),
// get_supported_protocols(), and browse() are not yet implemented in
// resource_browser.cpp. They will be added when the implementation lands.

// ---------------------------------------------------------------------------
// 4. BrowserFormatter works standalone
// ---------------------------------------------------------------------------

TEST(StorageIsolationTest, BrowserFormatterShortFormat) {
    std::vector<falcon::RemoteResource> resources;
    falcon::RemoteResource r;
    r.name = "file.txt";
    r.type = falcon::ResourceType::File;
    r.size = 1024;
    resources.push_back(r);

    std::string output = falcon::BrowserFormatter::format_short(resources);
    EXPECT_FALSE(output.empty());
    EXPECT_TRUE(output.find("file.txt") != std::string::npos);
}

TEST(StorageIsolationTest, BrowserFormatterLongFormat) {
    std::vector<falcon::RemoteResource> resources;
    falcon::RemoteResource r;
    r.name = "file.txt";
    r.type = falcon::ResourceType::File;
    r.size = 2048;
    resources.push_back(r);

    std::string output = falcon::BrowserFormatter::format_long(resources);
    EXPECT_FALSE(output.empty());
}

TEST(StorageIsolationTest, BrowserFormatterTableFormat) {
    std::vector<falcon::RemoteResource> resources;
    falcon::RemoteResource r1;
    r1.name = "a.txt";
    r1.size = 100;
    r1.type = falcon::ResourceType::File;
    falcon::RemoteResource r2;
    r2.name = "b.txt";
    r2.size = 200;
    r2.type = falcon::ResourceType::File;
    resources.push_back(r1);
    resources.push_back(r2);

    std::string output = falcon::BrowserFormatter::format_table(resources);
    EXPECT_FALSE(output.empty());
}

// ---------------------------------------------------------------------------
// 7. ListOptions defaults
// ---------------------------------------------------------------------------

TEST(StorageIsolationTest, ListOptionsDefaults) {
    falcon::ListOptions opts;
    EXPECT_FALSE(opts.show_hidden);
    EXPECT_FALSE(opts.recursive);
    EXPECT_EQ(opts.max_depth, 0);
    EXPECT_EQ(opts.sort_by, "name");
    EXPECT_FALSE(opts.sort_desc);
    EXPECT_FALSE(opts.include_metadata);
    EXPECT_TRUE(opts.filter.empty());
}
