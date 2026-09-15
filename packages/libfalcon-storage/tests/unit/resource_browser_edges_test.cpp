// Falcon Resource Browser Edge Coverage Tests
//
// 收口 resource_browser.cpp / resource_browser_utils.cpp 的 gcov 缺口：
// 格式化器树形/自定义列输出、工厂注册边界、路径工具的 Windows 盘符与
// ".." 折叠分支。注意 FALCON_ENABLE_CRYPTO_STORAGE_BROWSERS 是库的
// PRIVATE 宏（测试 TU 不可见），crypto 浏览器的注册探测一律走运行时
// BrowserFactory::is_supported，环境缺 OpenSSL 时自然跳过对应断言块。

#include <falcon/storage/resource_browser.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

// format_tree 的树枝前缀（"├── " / "└── "）用字节转义，避免源码编码
// 差异（MSVC 无 /utf-8 时）改变字面量字节。
const std::string kBranchTee = "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ";
const std::string kBranchEnd = "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ";

std::vector<falcon::RemoteResource> make_sample_resources() {
    falcon::RemoteResource dir;
    dir.name = "docs";
    dir.path = "/srv/docs";
    dir.type = falcon::ResourceType::Directory;
    dir.modified_time = "2025-12-23 10:00:00";
    dir.owner = "owner";
    dir.group = "staff";
    dir.permissions = falcon::FilePermissions::from_octal(0755);

    falcon::RemoteResource file;
    file.name = "readme.md";
    file.path = "/srv/readme.md";
    file.type = falcon::ResourceType::File;
    file.size = 2048;
    file.modified_time = "2025-12-23 10:00:00";
    file.owner = "owner";
    file.group = "staff";
    file.permissions = falcon::FilePermissions::from_octal(0644);

    falcon::RemoteResource link;
    link.name = "latest";
    link.path = "/srv/latest";
    link.type = falcon::ResourceType::Symlink;
    link.symlink_target = "readme.md";
    link.modified_time = "2025-12-23 10:00:00";
    link.owner = "owner";
    link.group = "staff";
    link.permissions = falcon::FilePermissions::from_octal(0777);

    return {dir, file, link};
}

} // namespace

//==============================================================================
// BrowserFactory：默认浏览器工厂与注册边界
//==============================================================================

TEST(BrowserFactoryEdges, CreatesEveryRegisteredDefaultBrowser) {
    ASSERT_TRUE(falcon::BrowserFactory::is_supported("s3"));
    auto s3 = falcon::BrowserFactory::create_browser("s3");
    ASSERT_NE(s3, nullptr);
    EXPECT_EQ(s3->get_name(), "S3");

    // crypto 浏览器按运行时注册情况探测（PRIVATE 宏对测试 TU 不可见）。
    const char* crypto_protocols[] = {"oss", "cos", "kodo", "qiniu", "upyun"};
    for (const auto* protocol : crypto_protocols) {
        if (!falcon::BrowserFactory::is_supported(protocol)) {
            continue;
        }
        auto browser = falcon::BrowserFactory::create_browser(protocol);
        ASSERT_NE(browser, nullptr) << protocol;
        EXPECT_FALSE(browser->get_name().empty()) << protocol;
    }
}

TEST(BrowserFactoryEdges, AvailableBrowsersAreSortedAndDescribed) {
    const auto browsers = falcon::BrowserFactory::available_browsers();
    ASSERT_FALSE(browsers.empty());

    EXPECT_TRUE(std::is_sorted(browsers.begin(), browsers.end(),
        [](const falcon::BrowserFactory::BrowserInfo& lhs,
           const falcon::BrowserFactory::BrowserInfo& rhs) {
            return lhs.protocol < rhs.protocol;
        }));

    const auto it = std::find_if(browsers.begin(), browsers.end(),
        [](const falcon::BrowserFactory::BrowserInfo& info) {
            return info.protocol == "s3";
        });
    ASSERT_NE(it, browsers.end());
    EXPECT_FALSE(it->display_name.empty());
    EXPECT_FALSE(it->description.empty());
}

TEST(BrowserFactoryEdges, CreateFromUrlDistinguishesProtocolPresence) {
    // "://" 前的空协议与完全无 scheme 都拿不到浏览器。
    EXPECT_EQ(falcon::BrowserFactory::create_from_url("://bucket/key"), nullptr);
    EXPECT_EQ(falcon::BrowserFactory::create_from_url("no-scheme-at-all"), nullptr);

    ASSERT_TRUE(falcon::BrowserFactory::is_supported("s3"));
    EXPECT_NE(falcon::BrowserFactory::create_from_url("s3://bucket/key"), nullptr);
}

TEST(BrowserFactoryEdges, RegisterBrowserRejectsInvalidEntries) {
    const char* probe = "falcon-edge-invalid-proto";

    falcon::BrowserFactory::BrowserInfo empty_protocol;
    empty_protocol.protocol = "";
    empty_protocol.display_name = "Invalid";
    falcon::BrowserFactory::register_browser(
        empty_protocol, [] { return std::unique_ptr<falcon::IResourceBrowser>(); });
    EXPECT_FALSE(falcon::BrowserFactory::is_supported(""));

    falcon::BrowserFactory::BrowserInfo no_factory;
    no_factory.protocol = probe;
    no_factory.display_name = "NoFactory";
    falcon::BrowserFactory::register_browser(no_factory, nullptr);
    EXPECT_FALSE(falcon::BrowserFactory::is_supported(probe));

    // 工厂非空即注册成功（即便工厂产出 nullptr 实例）。
    falcon::BrowserFactory::BrowserInfo custom;
    custom.protocol = "falcon-edge-custom";
    custom.display_name = "Custom";
    custom.description = "test factory";
    falcon::BrowserFactory::register_browser(custom, [] {
        return std::unique_ptr<falcon::IResourceBrowser>();
    });
    EXPECT_TRUE(falcon::BrowserFactory::is_supported("falcon-edge-custom"));
    EXPECT_EQ(falcon::BrowserFactory::create_browser("falcon-edge-custom"), nullptr);
}

//==============================================================================
// BrowserFormatter：format_tree
//==============================================================================

TEST(BrowserFormatterEdges, FormatTreePrintsBaseAndBranches) {
    const auto resources = make_sample_resources();

    const auto tree = falcon::BrowserFormatter::format_tree(resources, "/srv", 0);
    // 首行是 base_path，随后每资源一行树枝前缀 + display_name；
    // 最后一个资源用收尾分支。
    EXPECT_EQ(tree.compare(0, 4, "/srv"), 0);
    EXPECT_NE(tree.find(kBranchTee + "docs/"), std::string::npos);
    EXPECT_NE(tree.find(kBranchTee + "readme.md"), std::string::npos);
    EXPECT_NE(tree.find(kBranchEnd + "latest -> readme.md"), std::string::npos);
}

TEST(BrowserFormatterEdges, FormatTreeRespectsDepthLimit) {
    const auto resources = make_sample_resources();

    // max_depth>0 时首层（depth 0 < limit）仍然打印。
    const auto limited = falcon::BrowserFormatter::format_tree(resources, "/srv", 1);
    EXPECT_NE(limited.find("docs/"), std::string::npos);
}

//==============================================================================
// BrowserFormatter：format_table 的类型标识与 format_custom 列选择
//==============================================================================

TEST(BrowserFormatterEdges, FormatTableMarksSymlinkWithL) {
    const auto resources = make_sample_resources();
    const auto table = falcon::BrowserFormatter::format_table(resources);

    // 行首类型标识与权限粘连（ls 风格）：目录 d / 文件 - / 符号链接 l。
    EXPECT_EQ(table.compare(0, 1, "d"), 0) << table;
    EXPECT_NE(table.find("\n-rw-r--r--"), std::string::npos) << table;
    EXPECT_NE(table.find("\nlrwxrwxrwx"), std::string::npos) << table;
}

TEST(BrowserFormatterEdges, FormatCustomRendersKnownColumns) {
    falcon::RemoteResource file;
    file.name = "data.bin";
    file.type = falcon::ResourceType::File;
    file.size = 4096;
    file.modified_time = "2025-12-23 10:00:00";
    file.owner = "alice";

    const auto out = falcon::BrowserFormatter::format_custom(
        {file}, {"name", "size", "type", "modified", "owner"});

    EXPECT_NE(out.find("name"), std::string::npos);
    EXPECT_NE(out.find("data.bin"), std::string::npos);
    EXPECT_NE(out.find("file"), std::string::npos);
    EXPECT_NE(out.find("2025-12-23 10:0"), std::string::npos);
    EXPECT_NE(out.find("alice"), std::string::npos);
    // 分隔线：每列 15 个 '-'。
    EXPECT_NE(out.find("---------------"), std::string::npos);
}

TEST(BrowserFormatterEdges, FormatCustomOtherTypeUnknownColumnAndEmptyList) {
    falcon::RemoteResource link;
    link.name = "alias";
    link.type = falcon::ResourceType::Symlink;
    link.modified_time = "2025-12-23 10:00:00";
    link.owner = "bob";

    const auto out = falcon::BrowserFormatter::format_custom(
        {link}, {"type", "bogus"});

    // symlink 落到 type 三元链的 "other"，未知列落 "-"。
    EXPECT_NE(out.find("other"), std::string::npos);
    EXPECT_NE(out.find("bogus"), std::string::npos);

    // 空列表仍输出表头与分隔线。
    const auto empty = falcon::BrowserFormatter::format_custom(
        {}, {"name", "size"});
    EXPECT_NE(empty.find("name"), std::string::npos);
    EXPECT_EQ(empty.find("alias"), std::string::npos);
}

//==============================================================================
// ResourceBrowserUtils：路径工具边界
//==============================================================================

TEST(BrowserUtilsEdges, IsValidPathRejectsControlCharsButKeepsTab) {
    EXPECT_FALSE(falcon::ResourceBrowserUtils::is_valid_path(""));
    EXPECT_TRUE(falcon::ResourceBrowserUtils::is_valid_path("/ok/path"));
    EXPECT_TRUE(falcon::ResourceBrowserUtils::is_valid_path("tab\tinside"));
    EXPECT_FALSE(falcon::ResourceBrowserUtils::is_valid_path("bad\x01char"));
    // DEL(0x7f) 不在 <32 检查范围内，只有控制区字符被拒。
    EXPECT_FALSE(falcon::ResourceBrowserUtils::is_valid_path("\x1f"));
}

TEST(BrowserUtilsEdges, NormalizePathWindowsDriveLetter) {
    EXPECT_EQ(falcon::ResourceBrowserUtils::normalize_path("C:/a/../b"), "C:/b");
    // 盘符根目录：绝对判定 + 尾斜杠保留。
    EXPECT_EQ(falcon::ResourceBrowserUtils::normalize_path("C:\\"), "C:/");
}

TEST(BrowserUtilsEdges, NormalizePathKeepsLeadingDotDotForRelativePaths) {
    // 首个 ".." 无处可弹时保留（不越出相对根）；相对路径组件
    // 同样带前导分隔符（见 NormalizePathTrailingDotDotCollapses 注释）。
    EXPECT_EQ(falcon::ResourceBrowserUtils::normalize_path("../x"), "/../x");
    EXPECT_EQ(falcon::ResourceBrowserUtils::normalize_path("a/../.."), "/..");
}

TEST(BrowserUtilsEdges, NormalizePathTrailingDotDotCollapses) {
    // 实现语义：相对路径的组件也带前导分隔符（109 行对所有组件前置 "/"）。
    EXPECT_EQ(falcon::ResourceBrowserUtils::normalize_path("a/b/.."), "/a");
    EXPECT_EQ(falcon::ResourceBrowserUtils::normalize_path("a/.."), ".");
}

TEST(BrowserUtilsEdges, JoinPathEmptySidesAndAbsoluteName) {
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("", "n"), "n");
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("b", ""), "b");
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("/b", "/abs"), "/abs");
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("b", "C:/f"), "C:/f");
}

TEST(BrowserUtilsEdges, JoinPathWindowsDriveRootTakesNoExtraSeparator) {
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("C:", "file"), "C:/file");
    EXPECT_EQ(falcon::ResourceBrowserUtils::join_path("C:/", "file"), "C:/file");
}

TEST(BrowserUtilsEdges, GetParentPathTrailingSlashBareNameAndRoots) {
    // 尾斜杠先剥离，父目录保留分隔符结尾。
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_parent_path("a/b/"), "a/");
    // 裸文件名的父目录是当前目录。
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_parent_path("file"), ".");
    // 单层绝对路径的父目录是根。
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_parent_path("/a"), "/");
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_parent_path("/"), "");
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_parent_path("."), "");
}

TEST(BrowserUtilsEdges, GetFilenameStripsTrailingSlash) {
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_filename(""), "");
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_filename("dir/"), "dir");
    EXPECT_EQ(falcon::ResourceBrowserUtils::get_filename("C:/dir/file.txt"), "file.txt");
}
