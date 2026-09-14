/**
 * @file ftp_browser_mock_test.cpp
 * @brief FTPBrowser 对编程式 mock FTP 服务器的行为测试
 *
 * 覆盖：连接与认证、endpoint 选项、Unix 列表解析（权限位/所有者/
 * 修改时间/符号链接）、隐藏与通配符过滤、排序、get_resource_info
 * 与 exists 的两种来源、MKD/DELE/RMD/递归删除/RNFR+RNTO 的命令
 * 契约（此前 rename 把整串命令塞 CUSTOMREQUEST、目录操作让 curl
 * 先 CWD 进目标路径，均为协议性缺陷）。
 */

#include <gtest/gtest.h>

#include <falcon/storage/ftp_browser.hpp>

#include "mock_ftp_server.hpp"

namespace falcon {

using test::MockFtpServer;

namespace {

class FTPBrowserMockTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_.set_listing("/",
                            "total 8\r\n"
                            "-rw-r--r--   1 alice    staff        2048 Jan 02 2024 readme.txt\r\n"
                            "drwxr-xr-x   2 root     root         4096 Mar 03 09:15 subdir\r\n");
        server_.start();
        browser_ = std::make_unique<FTPBrowser>();
    }

    void TearDown() override {
        browser_.reset();
        server_.stop();
    }

    bool connect_default() {
        return browser_->connect("ftp://127.0.0.1:" + std::to_string(server_.port()) + "/",
                                 {{"username", "testuser"}, {"password", "testpass"}});
    }

    MockFtpServer server_;
    std::unique_ptr<FTPBrowser> browser_;
};

// ---- 连接 ----

TEST_F(FTPBrowserMockTest, ConnectSucceedsAndSendsCredentials) {
    ASSERT_TRUE(connect_default());
    const auto cmds = server_.commands();
    bool saw_user = false, saw_pass = false;
    for (const auto& c : cmds) {
        if (c == "USER testuser") saw_user = true;
        if (c == "PASS testpass") saw_pass = true;
    }
    EXPECT_TRUE(saw_user);
    EXPECT_TRUE(saw_pass);
}

TEST_F(FTPBrowserMockTest, ConnectFailsOnWrongPassword) {
    MockFtpServer strict("realuser", "realpass");
    strict.start();
    FTPBrowser browser;
    bool ok = browser.connect("ftp://127.0.0.1:" + std::to_string(strict.port()) + "/",
                              {{"username", "testuser"}, {"password", "testpass"}});
    EXPECT_FALSE(ok);
    strict.stop();
}

TEST_F(FTPBrowserMockTest, ConnectFailsOnUnreachableEndpoint) {
    bool ok = browser_->connect("ftp://127.0.0.1:1/",
                                {{"username", "u"}, {"password", "p"}});
    EXPECT_FALSE(ok);
}

TEST_F(FTPBrowserMockTest, ConnectHonorsEndpointOption) {
    // decoy 主机不可解析：只有 endpoint 选项生效连接才会成功
    bool ok = browser_->connect(
        "ftp://decoy.invalid/",
        {{"username", "testuser"},
         {"password", "testpass"},
         {"endpoint", "127.0.0.1:" + std::to_string(server_.port())}});
    EXPECT_TRUE(ok);
}

// ---- 列表解析 ----

TEST_F(FTPBrowserMockTest, ListParsesUnixListingFields) {
    ASSERT_TRUE(connect_default());
    auto resources = browser_->list_directory("", {});
    ASSERT_EQ(resources.size(), 2u);

    const auto& file = resources[0];
    EXPECT_EQ(file.name, "readme.txt");
    EXPECT_EQ(file.path, "/readme.txt");
    EXPECT_EQ(file.type, ResourceType::File);
    EXPECT_FALSE(file.is_directory());
    EXPECT_EQ(file.size, 2048u);
    EXPECT_EQ(file.owner, "alice");
    EXPECT_EQ(file.group, "staff");
    EXPECT_TRUE(file.permissions.owner_read);
    EXPECT_FALSE(file.permissions.owner_execute);
    EXPECT_TRUE(file.permissions.group_read);
    EXPECT_TRUE(file.permissions.other_read);

    const auto& dir = resources[1];
    EXPECT_EQ(dir.name, "subdir");
    EXPECT_EQ(dir.path, "/subdir");
    EXPECT_EQ(dir.type, ResourceType::Directory);
    EXPECT_TRUE(dir.is_directory());
    EXPECT_EQ(dir.size, 4096u);
}

TEST_F(FTPBrowserMockTest, ListFillsModifiedTimeFromListing) {
    server_.set_listing("/d", "-rw-r--r-- 1 u g 10 Jan 02 2024 old.txt\r\n");
    ASSERT_TRUE(connect_default());
    auto resources = browser_->list_directory("d", {});
    ASSERT_EQ(resources.size(), 1u);
    // Jan 02 2024 00:00 UTC，此前该字段恒为空
    EXPECT_EQ(resources[0].modified_time, "1704153600");
}

TEST_F(FTPBrowserMockTest, ListSortsByModifiedTime) {
    server_.set_listing(
        "/",
        "-rw-r--r-- 1 u g 1 Jan 02 2024 newest.txt\r\n"
        "-rw-r--r-- 1 u g 1 Jan 02 2022 oldest.txt\r\n"
        "-rw-r--r-- 1 u g 1 Jan 02 2023 middle.txt\r\n");
    ASSERT_TRUE(connect_default());

    ListOptions options;
    options.sort_by = "modified_time";
    auto resources = browser_->list_directory("", options);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "oldest.txt");
    EXPECT_EQ(resources[1].name, "middle.txt");
    EXPECT_EQ(resources[2].name, "newest.txt");
}

TEST_F(FTPBrowserMockTest, ListSortsByNameDescending) {
    ASSERT_TRUE(connect_default());
    ListOptions options;
    options.sort_desc = true;
    auto resources = browser_->list_directory("", options);
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_EQ(resources[0].name, "subdir");
    EXPECT_EQ(resources[1].name, "readme.txt");
}

TEST_F(FTPBrowserMockTest, ListAppliesHiddenFilterAndWildcard) {
    server_.set_listing(
        "/hidden",
        "-rw-r--r-- 1 u g 1 Jan 02 2024 a.txt\r\n"
        "-rw-r--r-- 1 u g 1 Jan 02 2024 .secret\r\n"
        "-rw-r--r-- 1 u g 1 Jan 02 2024 b.log\r\n");
    ASSERT_TRUE(connect_default());

    auto resources = browser_->list_directory("hidden", {});
    ASSERT_EQ(resources.size(), 2u); // .secret 被过滤

    ListOptions show_hidden;
    show_hidden.show_hidden = true;
    resources = browser_->list_directory("hidden", show_hidden);
    ASSERT_EQ(resources.size(), 3u);

    ListOptions wildcard;
    wildcard.show_hidden = true;
    wildcard.filter = "*.txt";
    resources = browser_->list_directory("hidden", wildcard);
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].name, "a.txt");
}

TEST_F(FTPBrowserMockTest, ListParsesSymlinkTarget) {
    server_.set_listing(
        "/links",
        "lrwxrwxrwx 1 u g 7 Jan 02 2024 link.txt -> real.txt\r\n");
    ASSERT_TRUE(connect_default());
    auto resources = browser_->list_directory("links", {});
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].type, ResourceType::Symlink);
    EXPECT_EQ(resources[0].name, "link.txt");
    EXPECT_EQ(resources[0].symlink_target, "real.txt");
    EXPECT_EQ(resources[0].path, "/links/link.txt");
}

TEST_F(FTPBrowserMockTest, ListFailsCleanOnServerError) {
    server_.fail_list("/secret");
    ASSERT_TRUE(connect_default());
    auto resources = browser_->list_directory("secret", {});
    EXPECT_TRUE(resources.empty());
}

// ---- 资源信息与存在性 ----

TEST_F(FTPBrowserMockTest, GetResourceInfoFromParentListing) {
    server_.set_listing("/docs",
                        "-rw-r--r-- 1 alice staff 264 Jan 02 2024 readme.txt\r\n");
    ASSERT_TRUE(connect_default());

    auto info = browser_->get_resource_info("/docs/readme.txt");
    EXPECT_EQ(info.name, "readme.txt");
    EXPECT_EQ(info.path, "/docs/readme.txt");
    EXPECT_EQ(info.type, ResourceType::File);
    EXPECT_EQ(info.size, 264u);
    // libcurl multicwd 模式：CWD 进父目录后发无参 LIST
    EXPECT_TRUE(server_.any_command("CWD docs"));
    EXPECT_TRUE(server_.any_command("LIST"));
}

TEST_F(FTPBrowserMockTest, GetResourceInfoFallsBackToSizeCommand) {
    server_.set_file_size("/docs/ghost.bin", 512);
    ASSERT_TRUE(connect_default());

    auto info = browser_->get_resource_info("/docs/ghost.bin");
    EXPECT_EQ(info.name, "ghost.bin");
    EXPECT_EQ(info.type, ResourceType::File);
    EXPECT_EQ(info.size, 512u);
    EXPECT_TRUE(server_.any_command("SIZE"));
}

TEST_F(FTPBrowserMockTest, ExistsFollowsResourceLookup) {
    server_.set_listing("/docs",
                        "-rw-r--r-- 1 alice staff 264 Jan 02 2024 readme.txt\r\n");
    ASSERT_TRUE(connect_default());

    EXPECT_TRUE(browser_->exists("/docs/readme.txt"));
    EXPECT_FALSE(browser_->exists("/docs/ghost.txt"));
}

// ---- 写操作 ----

TEST_F(FTPBrowserMockTest, CreateDirectorySendsMkd) {
    ASSERT_TRUE(connect_default());
    EXPECT_TRUE(browser_->create_directory("/docs/newdir"));
    EXPECT_EQ(server_.count_command("MKD /docs/newdir"), 1u);
}

TEST_F(FTPBrowserMockTest, CreateDirectoryFailsWhenServerRejects) {
    server_.fail_command("MKD");
    ASSERT_TRUE(connect_default());
    EXPECT_FALSE(browser_->create_directory("/docs/newdir"));
}

TEST_F(FTPBrowserMockTest, RemoveFileSendsDele) {
    server_.set_listing("/docs",
                        "-rw-r--r-- 1 alice staff 100 Jan 02 2024 a.txt\r\n");
    ASSERT_TRUE(connect_default());
    EXPECT_TRUE(browser_->remove("/docs/a.txt"));
    EXPECT_EQ(server_.count_command("DELE /docs/a.txt"), 1u);
    EXPECT_FALSE(server_.any_command("RMD"));
}

TEST_F(FTPBrowserMockTest, RemoveDirectorySendsRmd) {
    server_.set_listing("/docs", "drwxr-xr-x 2 root root 4096 Jan 02 2024 sub\r\n");
    ASSERT_TRUE(connect_default());
    EXPECT_TRUE(browser_->remove("/docs/sub"));
    EXPECT_EQ(server_.count_command("RMD /docs/sub"), 1u);
    EXPECT_FALSE(server_.any_command("DELE"));
}

TEST_F(FTPBrowserMockTest, RemoveTopLevelFileWithoutParentSlash) {
    server_.set_listing("/",
                        "-rw-r--r-- 1 alice staff 100 Jan 02 2024 top.txt\r\n");
    ASSERT_TRUE(connect_default());
    // 无父目录段的路径：此前 substr(0, npos) 把整串路径当父目录
    EXPECT_TRUE(browser_->remove("top.txt"));
    EXPECT_EQ(server_.count_command("DELE /top.txt"), 1u);
}

TEST_F(FTPBrowserMockTest, RemoveRecursiveDeletesDeepTree) {
    server_.set_listing("/",
                        "drwxr-xr-x 2 root root 4096 Jan 02 2024 docs\r\n");
    server_.set_listing("/docs",
                        "-rw-r--r-- 1 u g 1 Jan 02 2024 a.txt\r\n"
                        "drwxr-xr-x 2 root root 4096 Jan 02 2024 sub\r\n");
    server_.set_listing("/docs/sub",
                        "-rw-r--r-- 1 u g 1 Jan 02 2024 b.txt\r\n");
    ASSERT_TRUE(connect_default());

    EXPECT_TRUE(browser_->remove("/docs", true));
    EXPECT_EQ(server_.count_command("DELE /docs/a.txt"), 1u);
    EXPECT_EQ(server_.count_command("DELE /docs/sub/b.txt"), 1u);
    EXPECT_EQ(server_.count_command("RMD /docs/sub"), 1u);
    EXPECT_EQ(server_.count_command("RMD /docs"), 1u);
    EXPECT_EQ(server_.count_command("DELE"), 2u);
    for (const auto& c : server_.commands()) {
        EXPECT_EQ(c.find("//"), std::string::npos) << "double slash: " << c;
    }
}

TEST_F(FTPBrowserMockTest, RenameIssuesRnfrThenRnto) {
    server_.set_listing("/docs", "drwxr-xr-x 2 root root 4096 Jan 02 2024 docs\r\n"
                                 "-rw-r--r-- 1 u g 1 Jan 02 2024 a.txt\r\n");
    ASSERT_TRUE(connect_default());

    EXPECT_TRUE(browser_->rename("/docs/a.txt", "/docs/b.txt"));

    const auto cmds = server_.commands();
    size_t rnfr_pos = cmds.size(), rnto_pos = cmds.size();
    size_t rnfr_count = 0, rnto_count = 0;
    for (size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i] == "RNFR /docs/a.txt") { rnfr_pos = i; ++rnfr_count; }
        if (cmds[i] == "RNTO /docs/b.txt") { rnto_pos = i; ++rnto_count; }
        // 旧缺陷回归：两条命令被拼进一条 CUSTOMREQUEST
        EXPECT_EQ(cmds[i].find("\r\n"), std::string::npos) << "joined command: " << cmds[i];
    }
    EXPECT_EQ(rnfr_count, 1u);
    EXPECT_EQ(rnto_count, 1u);
    ASSERT_NE(rnfr_pos, cmds.size());
    ASSERT_NE(rnto_pos, cmds.size());
    EXPECT_LT(rnfr_pos, rnto_pos);
}

TEST_F(FTPBrowserMockTest, RenameFailsWhenServerRejects) {
    server_.fail_command("RNTO");
    server_.set_listing("/docs", "drwxr-xr-x 2 root root 4096 Jan 02 2024 docs\r\n"
                                 "-rw-r--r-- 1 u g 1 Jan 02 2024 a.txt\r\n");
    ASSERT_TRUE(connect_default());
    EXPECT_FALSE(browser_->rename("/docs/a.txt", "/docs/b.txt"));
}

// ---- 目录与杂项 ----

TEST_F(FTPBrowserMockTest, ChangeDirectoryAffectsSubsequentListing) {
    server_.set_listing("/docs",
                        "-rw-r--r-- 1 alice staff 264 Jan 02 2024 readme.txt\r\n");
    ASSERT_TRUE(connect_default());

    EXPECT_TRUE(browser_->change_directory("/docs"));
    EXPECT_EQ(browser_->get_current_directory(), "/docs");

    auto resources = browser_->list_directory("", {});
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].name, "readme.txt");
    EXPECT_EQ(resources[0].path, "/docs/readme.txt");
}

TEST_F(FTPBrowserMockTest, CopyIsNotSupported) {
    ASSERT_TRUE(connect_default());
    EXPECT_FALSE(browser_->copy("/docs/a.txt", "/docs/b.txt"));
}

TEST_F(FTPBrowserMockTest, QuotaInfoIsEmpty) {
    ASSERT_TRUE(connect_default());
    EXPECT_TRUE(browser_->get_quota_info().empty());
    EXPECT_EQ(browser_->get_root_path(), "/");
    EXPECT_EQ(browser_->get_name(), "FTP");
    EXPECT_TRUE(browser_->can_handle("ftp://host/file"));
    EXPECT_TRUE(browser_->can_handle("ftps://host/file"));
    EXPECT_FALSE(browser_->can_handle("http://host/file"));
}

TEST_F(FTPBrowserMockTest, ConnectParsesUrlWithPathAndCredentials) {
    // URL 自带子路径：connect 后 list_directory("") 应列该子目录
    server_.set_listing("/docs",
                        "-rw-r--r-- 1 alice staff 264 Jan 02 2024 readme.txt\r\n");
    bool ok = browser_->connect(
        "ftp://testuser:testpass@127.0.0.1:" + std::to_string(server_.port()) + "/docs",
        {});
    ASSERT_TRUE(ok);

    auto resources = browser_->list_directory("", {});
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].path, "/docs/readme.txt");
}

} // namespace
} // namespace falcon
