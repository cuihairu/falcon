/**
 * @file s3_browser_mock_test.cpp
 * @brief S3 资源浏览器 mock HTTP 测试
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 覆盖 S3Browser 的完整请求路径：connect（自定义 endpoint 生效，path-style
 * 地址）/list_directory（JSON Contents 与 CommonPrefixes 递归、过滤、排序）/
 * get_resource_info/exists/create_directory/remove（含递归）/rename
 * （copy+delete）/get_quota_info，以及 URL 解析与错误路径。
 * 服务器监听 INADDR_ANY、绑定随机端口，离线可运行。
 */

#include <falcon/storage/s3_browser.hpp>

#include <gtest/gtest.h>

#include "mock_http_server.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace falcon;

namespace {

/// S3 兼容 mock：复用共享 MockHttpServer（一连接一请求 + 编程式应答）
using MockS3Server = MockHttpServer;

const char* kBucket = "testbucket";

/// 默认 handler：一切请求返回 200 + 空 JSON 对象
MockS3Server::Response defaultReply(const std::string&, const std::string&) {
    return {200, "{}"};
}

std::string contentsJson(const std::string& keysJson) {
    return "{\"Contents\": [" + keysJson + "]}";
}

/// JSON 内容允许换行空白，raw string 便于阅读
const char* kTwoObjects =
    R"({"Key":"docs/img.png","Size":200,"ETag":"\"e2\""},)"
    R"({"Key":"docs/readme.md","Size":100,)"
    R"("LastModified":"2026-01-01T00:00:00Z","ETag":"\"e1\"",)"
    R"("StorageClass":"STANDARD"})";

} // namespace

//==============================================================================
// URL 解析（纯逻辑）
//==============================================================================

TEST(S3UrlParserTest, SplitsBucketAndKey) {
    auto url = S3UrlParser::parse("s3://mybucket/path/to/file.txt");
    EXPECT_EQ(url.bucket, "mybucket");
    EXPECT_EQ(url.key, "path/to/file.txt");
}

TEST(S3UrlParserTest, BucketOnlyKeepsEmptyKey) {
    auto url = S3UrlParser::parse("s3://mybucket");
    EXPECT_EQ(url.bucket, "mybucket");
    EXPECT_TRUE(url.key.empty());
}

TEST(S3UrlParserTest, NonS3ProtocolYieldsEmpty) {
    auto url = S3UrlParser::parse("https://example.com/bucket/key");
    EXPECT_TRUE(url.bucket.empty());
    EXPECT_TRUE(url.key.empty());
}

//==============================================================================
// 基础属性
//==============================================================================

TEST(S3BrowserBasicTest, NameProtocolsAndCanHandle) {
    S3Browser browser;
    EXPECT_EQ(browser.get_name(), "S3");
    auto protocols = browser.get_supported_protocols();
    ASSERT_GE(protocols.size(), size_t{1});
    EXPECT_EQ(protocols[0], "s3");

    EXPECT_TRUE(browser.can_handle("s3://bucket/key"));
    EXPECT_TRUE(browser.can_handle("http://x.s3.us-east-1.amazonaws.com/y"));
    EXPECT_TRUE(browser.can_handle("https://s3.amazonaws.com/bucket"));
    EXPECT_FALSE(browser.can_handle("https://example.com/file.zip"));
    EXPECT_FALSE(browser.can_handle(""));
}

TEST(S3BrowserBasicTest, DirectoryHelpersAreInMemory) {
    S3Browser browser;
    EXPECT_TRUE(browser.get_current_directory().empty());
    EXPECT_TRUE(browser.change_directory("docs"));
    EXPECT_EQ(browser.get_current_directory(), "docs/");
    EXPECT_EQ(browser.get_root_path(), "/");
    browser.disconnect();  // 无状态协议，无副作用
}

//==============================================================================
// mock 请求路径
//==============================================================================

class S3BrowserMockTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<MockS3Server>(defaultReply);
        ASSERT_TRUE(server_->start());
    }

    bool connectBrowser(S3Browser& browser,
                        std::map<std::string, std::string> options = {}) {
        if (options.find("endpoint") == options.end()) {
            options["endpoint"] = server_->base_url();
        }
        options.emplace("access_key_id", "AKIA_TEST");
        options.emplace("secret_access_key", "secret");
        options.emplace("region", "us-west-2");
        return browser.connect("s3://" + std::string(kBucket), options);
    }

    std::unique_ptr<MockS3Server> server_;
};

TEST_F(S3BrowserMockTest, ConnectWithCustomEndpointSucceeds) {
    S3Browser browser;
    EXPECT_TRUE(connectBrowser(browser));

    // test_connection 的 GET 打到自定义 endpoint（path-style bucket 地址）
    auto reqs = server_->requests();
    ASSERT_GE(reqs.size(), size_t{1});
    EXPECT_EQ(reqs[0].first, "GET");
    EXPECT_NE(reqs[0].second.find("/" + std::string(kBucket) + "?max-keys=1"),
              std::string::npos);
}

TEST_F(S3BrowserMockTest, ConnectFailsWhenServerUnreachable) {
    S3Browser browser;
    std::map<std::string, std::string> options;
    options["endpoint"] = "http://127.0.0.1:1";  // 保留端口，必然拒绝
    options["access_key_id"] = "k";
    options["secret_access_key"] = "s";
    EXPECT_FALSE(browser.connect("s3://" + std::string(kBucket), options));
}

TEST_F(S3BrowserMockTest, ListDirectoryParsesJsonContents) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, contentsJson(kTwoObjects)};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    auto resources = browser.list_directory("docs", options);

    // 默认 sort_by="name"：img.png 排在 readme.md 之前
    ASSERT_EQ(resources.size(), size_t{2});
    EXPECT_EQ(resources[0].name, "img.png");
    EXPECT_EQ(resources[0].path, "docs/img.png");
    EXPECT_EQ(resources[0].size, uint64_t{200});
    EXPECT_EQ(resources[1].name, "readme.md");
    EXPECT_EQ(resources[1].path, "docs/readme.md");
    EXPECT_EQ(resources[1].size, uint64_t{100});
    EXPECT_EQ(resources[1].type, ResourceType::File);
    EXPECT_EQ(resources[1].metadata.at("storage_class"), "STANDARD");

    // 请求携带 list-type 与 prefix（非 '/' 结尾时补斜杠）
    auto reqs = server_->requests();
    bool saw_list = false;
    for (auto& [method, path] : reqs) {
        if (path.find("list-type=2") != std::string::npos) {
            saw_list = true;
            EXPECT_NE(path.find("prefix=docs/"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_list);
}

TEST_F(S3BrowserMockTest, ListDirectoryFiltersHiddenAndSortsByName) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, contentsJson(
                    R"({"Key":"b.txt","Size":1},)"
                    R"({"Key":".hidden","Size":2},)"
                    R"({"Key":"a.txt","Size":3})")};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    options.sort_by = "name";
    auto resources = browser.list_directory("", options);

    // 隐藏文件被过滤，其余按名称升序
    ASSERT_EQ(resources.size(), size_t{2});
    EXPECT_EQ(resources[0].name, "a.txt");
    EXPECT_EQ(resources[1].name, "b.txt");
}

TEST_F(S3BrowserMockTest, ListDirectoryRecursiveDescendsCommonPrefixes) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("prefix=docs/") == std::string::npos &&
                path.find("list-type=2") != std::string::npos) {
                // 顶层：仅一个子目录前缀
                return MockS3Server::Response{200,
                    R"({"CommonPrefixes": [{"Prefix":"docs/"}]})"};
            }
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, contentsJson(
                    R"({"Key":"docs/deep.bin","Size":9})")};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    options.recursive = true;
    auto resources = browser.list_directory("", options);

    ASSERT_EQ(resources.size(), size_t{1});
    EXPECT_EQ(resources[0].path, "docs/deep.bin");

    // 两次列表请求：顶层 + 子前缀
    EXPECT_GE(server_->requests().size(), size_t{3});  // connect + 2 次 list
}

TEST_F(S3BrowserMockTest, ListDirectoryEmptyOnMalformedResponse) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, "<not-json>"};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    EXPECT_TRUE(browser.list_directory("", options).empty());
}

TEST_F(S3BrowserMockTest, ListDirectoryEmptyOnHttpError) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                // 状态码错误（如 AccessDenied）：业务失败不解析 body
                return MockS3Server::Response{403, R"(<?xml><Error/></xml>)"};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    EXPECT_TRUE(browser.list_directory("", options).empty());
}

TEST_F(S3BrowserMockTest, GetResourceInfoHeadParsesResponseHeaders) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string& method, const std::string& path) {
            if (method == "HEAD") {
                MockS3Server::Response resp;
                resp.body = "";  // HEAD 无响应体，元数据在头里
                resp.headers = {{"Content-Length", "123"},
                                {"ETag", "\"abc123\""},
                                {"Last-Modified", "Wed, 01 Jan 2026 00:00:00 GMT"},
                                {"Content-Type", "text/plain"}};
                return resp;
            }
            return defaultReply(method, path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    auto info = browser.get_resource_info("docs/readme.md");
    EXPECT_EQ(info.name, "readme.md");
    EXPECT_EQ(info.path, "docs/readme.md");
    EXPECT_EQ(info.size, uint64_t{123});
    EXPECT_EQ(info.etag, "\"abc123\"");
    EXPECT_EQ(info.modified_time, "Wed, 01 Jan 2026 00:00:00 GMT");
    EXPECT_EQ(info.mime_type, "text/plain");

    // HEAD 请求打到对象 path-style 地址（key 中的 '/' 保留不编码）
    bool saw_head = false;
    for (auto& [method, path] : server_->requests()) {
        if (method == "HEAD") {
            saw_head = true;
            EXPECT_NE(path.find("/" + std::string(kBucket) + "/docs/readme.md"),
                      std::string::npos);
        }
    }
    EXPECT_TRUE(saw_head);
}

TEST_F(S3BrowserMockTest, ExistsFollowsHttpStatusCode) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string& method, const std::string& path) {
            if (method == "HEAD" && path.find("missing") != std::string::npos) {
                return MockS3Server::Response{404, ""};
            }
            return defaultReply(method, path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.exists("docs/readme.md"));   // 200 → 存在
    EXPECT_FALSE(browser.exists("missing.obj"));     // 404 → 不存在
}

TEST_F(S3BrowserMockTest, CreateDirectoryPutsDirectoryMarker) {
    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.create_directory("newdir"));
    bool saw_put_slash = false;
    for (auto& [method, path] : server_->requests()) {
        if (method == "PUT" &&
            path.find("/" + std::string(kBucket) + "/newdir/") != std::string::npos) {
            saw_put_slash = true;
        }
    }
    EXPECT_TRUE(saw_put_slash);  // 目录标记以 '/' 结尾
}

TEST_F(S3BrowserMockTest, RemoveObjectSendsDelete) {
    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.remove("docs/readme.md", false));
    bool saw_delete = false;
    for (auto& [method, path] : server_->requests()) {
        if (method == "DELETE" &&
            path.find("/" + std::string(kBucket) + "/docs/readme.md") != std::string::npos) {
            saw_delete = true;
        }
    }
    EXPECT_TRUE(saw_delete);
}

TEST_F(S3BrowserMockTest, RemoveRecursiveDeletesChildrenThenTarget) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string& method, const std::string& path) {
            if (method == "GET" && path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, contentsJson(
                    R"({"Key":"dir/aa.txt"},{"Key":"dir/bb.txt"})")};
            }
            return defaultReply(method, path);  // DELETE 一律成功
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.remove("dir", true));

    std::vector<std::string> deleted;
    for (auto& [method, path] : server_->requests()) {
        if (method == "DELETE") {
            deleted.push_back(path);
        }
    }
    // 子对象按路径降序（最深优先）+ 目标目录本身
    ASSERT_EQ(deleted.size(), size_t{3});
    EXPECT_NE(deleted[0].find("dir/bb.txt"), std::string::npos);
    EXPECT_NE(deleted[1].find("dir/aa.txt"), std::string::npos);
    EXPECT_NE(deleted[2].find("/" + std::string(kBucket) + "/dir"), std::string::npos);
}

TEST_F(S3BrowserMockTest, RenameCopiesThenDeletes) {
    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.rename("old.txt", "new.txt"));

    bool saw_put = false;
    bool saw_delete = false;
    for (auto& [method, path] : server_->requests()) {
        if (method == "PUT") saw_put = true;
        if (method == "DELETE") saw_delete = true;
    }
    EXPECT_TRUE(saw_put);
    EXPECT_TRUE(saw_delete);
}

TEST_F(S3BrowserMockTest, GetQuotaInfoParsesStorageBytes) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("quota") != std::string::npos) {
                return MockS3Server::Response{200,
                    R"({"Quota": {"StorageBytes": 123456}})"};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    auto quota = browser.get_quota_info();
    ASSERT_EQ(quota.count("total"), size_t{1});
    EXPECT_EQ(quota["total"], uint64_t{123456});
}

TEST_F(S3BrowserMockTest, GetQuotaInfoEmptyOnBadResponse) {
    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    auto quota = browser.get_quota_info();  // 默认 handler 应答 "{}"
    EXPECT_TRUE(quota.empty());
}
