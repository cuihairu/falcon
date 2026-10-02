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

/// ListBucketResult XML 包装（ListObjectsV2 协议面：真 S3/MinIO/R2/B2/
/// Wasabi/GCS 一律应答 XML——历史 mock 的 {"Contents":[...]} JSON 形态
/// 与真实协议不符，已于 S3 XML list 收口批次替换）
std::string listResultXml(const std::string& inner = "") {
    return "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
           + inner + "</ListBucketResult>";
}

/// <Contents> 条目构造（字段按需携带，模拟实现侧字段裁剪）
std::string objXml(const std::string& key, uint64_t size = 0,
                   const std::string& last_modified = "",
                   const std::string& etag = "",
                   const std::string& storage_class = "") {
    std::string s = "<Contents><Key>" + key + "</Key><Size>"
                    + std::to_string(size) + "</Size>";
    if (!last_modified.empty()) {
        s += "<LastModified>" + last_modified + "</LastModified>";
    }
    if (!etag.empty()) {
        s += "<ETag>" + etag + "</ETag>";
    }
    if (!storage_class.empty()) {
        s += "<StorageClass>" + storage_class + "</StorageClass>";
    }
    s += "</Contents>";
    return s;
}

/// 顶层两个对象（img.png / readme.md）
const std::string kTwoObjects =
    objXml("docs/img.png", 200) +
    objXml("docs/readme.md", 100, "2026-01-01T00:00:00Z", "", "STANDARD");

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

TEST_F(S3BrowserMockTest, ListDirectoryParsesXmlContents) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, listResultXml(kTwoObjects)};
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
                return MockS3Server::Response{200, listResultXml(
                    objXml("b.txt") + objXml(".hidden", 2) + objXml("a.txt", 3))};
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

    ListOptions desc;
    desc.sort_by = "name";
    desc.sort_desc = true;
    resources = browser.list_directory("", desc);
    ASSERT_EQ(resources.size(), size_t{2});
    EXPECT_EQ(resources[0].name, "b.txt");
    EXPECT_EQ(resources[1].name, "a.txt");
}

TEST_F(S3BrowserMockTest, ListDirectoryRecursiveDescendsCommonPrefixes) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("prefix=docs/") == std::string::npos &&
                path.find("list-type=2") != std::string::npos) {
                // 顶层：仅一个子目录前缀
                return MockS3Server::Response{200, listResultXml(
                    "<CommonPrefixes><Prefix>docs/</Prefix></CommonPrefixes>")};
            }
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200,
                    listResultXml(objXml("docs/deep.bin", 9))};
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

TEST_F(S3BrowserMockTest, ListDirectoryNonRecursiveShowsPrefixesAsDirectories) {
    // 非递归模式：CommonPrefixes 作为目录条目可见（此前目录条目在任何
    // 模式下都不出现，文件浏览器视角根目录恒空）
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, listResultXml(
                    objXml("root.txt", 1) +
                    "<CommonPrefixes><Prefix>docs/</Prefix></CommonPrefixes>" +
                    "<CommonPrefixes><Prefix>img/</Prefix></CommonPrefixes>")};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;  // recursive 默认 false
    auto resources = browser.list_directory("", options);

    ASSERT_EQ(resources.size(), size_t{3});
    EXPECT_EQ(resources[0].name, "docs");
    EXPECT_EQ(resources[0].type, ResourceType::Directory);
    EXPECT_EQ(resources[0].path, "docs");
    EXPECT_EQ(resources[1].name, "img");
    EXPECT_EQ(resources[1].type, ResourceType::Directory);
    EXPECT_EQ(resources[2].name, "root.txt");
    EXPECT_EQ(resources[2].type, ResourceType::File);
}

TEST_F(S3BrowserMockTest, ListRequestsDelimiterOnlyInNonRecursiveMode) {
    // delimiter 语义：非递归必须带 delimiter=/ 才能拿到 CommonPrefixes，
    // 否则真 S3/RustFS 平铺全部嵌套对象、目录不可见（RustFS 真面走查
    // 实证）；递归不带 delimiter，服务端一次返回全层级
    std::vector<std::string> list_queries;  // 仅记录顶层列表请求
    server_ = std::make_unique<MockS3Server>(
        [&](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                if (path.find("prefix=") == std::string::npos) {
                    list_queries.push_back(path);
                    return MockS3Server::Response{200, listResultXml(
                        "<CommonPrefixes><Prefix>docs/</Prefix></CommonPrefixes>")};
                }
                return MockS3Server::Response{200,
                    listResultXml(objXml("docs/deep.bin", 3))};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions non_recursive;  // recursive 默认 false
    browser.list_directory("", non_recursive);

    ListOptions recursive;
    recursive.recursive = true;
    browser.list_directory("", recursive);

    ASSERT_EQ(list_queries.size(), size_t{2});
    EXPECT_NE(list_queries[0].find("delimiter=/"), std::string::npos);
    EXPECT_EQ(list_queries[1].find("delimiter="), std::string::npos);
}

TEST_F(S3BrowserMockTest, ListDirectoryDecodesXmlEntitiesInKeys) {
    // key 中的 XML 实体必须还原（&amp; 是 S3 对 '&' 的强制转义）
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200,
                    listResultXml(objXml("docs/a&amp;b.txt", 5))};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    auto resources = browser.list_directory("docs", options);
    ASSERT_EQ(resources.size(), size_t{1});
    EXPECT_EQ(resources[0].path, "docs/a&b.txt");
    EXPECT_EQ(resources[0].name, "a&b.txt");
}

TEST_F(S3BrowserMockTest, ListDirectoryToleratesNamespacedTags) {
    // 防御性：带命名空间前缀的标签按局部名匹配（部分 S3 兼容实现会加前缀）
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200,
                    "<ns:ListBucketResult>"
                    "<ns:Contents><ns:Key>ns.txt</ns:Key>"
                    "<ns:Size>7</ns:Size></ns:Contents>"
                    "</ns:ListBucketResult>"};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    auto resources = browser.list_directory("", options);
    ASSERT_EQ(resources.size(), size_t{1});
    EXPECT_EQ(resources[0].path, "ns.txt");
    EXPECT_EQ(resources[0].size, uint64_t{7});
}

TEST_F(S3BrowserMockTest, ListDirectoryEmptyBucketXmlYieldsEmptyWithoutJsonFallback) {
    // 真实空桶：ListBucketResult 无 Contents/CommonPrefixes —— 零条目成功，
    // 不得落入 JSON 兼容路径报解析错误
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, listResultXml()};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    EXPECT_TRUE(browser.list_directory("", options).empty());
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
                return MockS3Server::Response{200,
                    listResultXml(objXml("dir/aa.txt") + objXml("dir/bb.txt"))};
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
    // 子对象按路径降序（最深优先）+ 目标目录本身 + 尾斜杠标记兜底删除
    // （S3 对不存在 key 的 DELETE 也回 2xx，目录标记必须显式补发——
    // RustFS 真面走查实证：只删无斜杠 key 时标记静默残留）
    ASSERT_EQ(deleted.size(), size_t{4});
    EXPECT_NE(deleted[0].find("dir/bb.txt"), std::string::npos);
    EXPECT_NE(deleted[1].find("dir/aa.txt"), std::string::npos);
    EXPECT_NE(deleted[2].find("/" + std::string(kBucket) + "/dir"), std::string::npos);
    EXPECT_EQ(deleted[3].back(), '/');
}

TEST_F(S3BrowserMockTest, CopyDirectoryMarkerFallsBackToTrailingSlash) {
    // S3 目录 = 尾斜杠 0 字节标记对象：对无斜杠目标 PUT 复制回 404
    // （NoSuchKey），须回退带尾斜杠重试（RustFS 真面走查实证）
    server_ = std::make_unique<MockS3Server>(
        [](const std::string& method, const std::string& path) {
            if (method == "PUT" &&
                path.find("/" + std::string(kBucket) + "/dst") != std::string::npos &&
                path.back() != '/') {
                return MockS3Server::Response{404,
                    "<Error><Code>NoSuchKey</Code></Error>"};
            }
            return defaultReply(method, path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.copy("src", "dst"));

    std::vector<std::string> put_paths;
    for (auto& [method, path] : server_->requests()) {
        if (method == "PUT") {
            put_paths.push_back(path);
        }
    }
    // 首试无斜杠 404 → 回退尾斜杠 200
    ASSERT_EQ(put_paths.size(), size_t{2});
    EXPECT_NE(put_paths[0].find("/" + std::string(kBucket) + "/dst"),
              std::string::npos);
    EXPECT_EQ(put_paths[1].back(), '/');
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

TEST_F(S3BrowserMockTest, GetQuotaInfoToleratesMalformedJson) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("quota") != std::string::npos) {
                // 200 + 非法 JSON：解析异常必须被吞掉，quota 保持空
                return MockS3Server::Response{200, "{corrupted"};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    auto quota = browser.get_quota_info();
    EXPECT_TRUE(quota.empty());
}

//==============================================================================
// 过滤/排序/URL 边缘补充
//==============================================================================

TEST_F(S3BrowserMockTest, ListSortsBySizeAscendingAndDescending) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, listResultXml(
                    objXml("c.bin", 300) + objXml("a.bin", 100) +
                    objXml("b.bin", 200))};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions asc;
    asc.sort_by = "size";
    auto resources = browser.list_directory("", asc);
    ASSERT_EQ(resources.size(), size_t{3});
    EXPECT_EQ(resources[0].name, "a.bin");
    EXPECT_EQ(resources[2].name, "c.bin");

    ListOptions desc;
    desc.sort_by = "size";
    desc.sort_desc = true;
    resources = browser.list_directory("", desc);
    ASSERT_EQ(resources.size(), size_t{3});
    EXPECT_EQ(resources[0].name, "c.bin");
    EXPECT_EQ(resources[2].name, "a.bin");
}

TEST_F(S3BrowserMockTest, ListSortsByModifiedTime) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, listResultXml(
                    objXml("new.txt", 1, "2026-05-01T00:00:00Z") +
                    objXml("old.txt", 1, "2026-01-01T00:00:00Z"))};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions asc;
    asc.sort_by = "modified_time";
    auto resources = browser.list_directory("", asc);
    ASSERT_EQ(resources.size(), size_t{2});
    EXPECT_EQ(resources[0].name, "old.txt");

    ListOptions desc;
    desc.sort_by = "modified_time";
    desc.sort_desc = true;
    resources = browser.list_directory("", desc);
    ASSERT_EQ(resources.size(), size_t{2});
    EXPECT_EQ(resources[0].name, "new.txt");
}

TEST_F(S3BrowserMockTest, ListFilterWildcardPrefixSuffixExactAndStar) {
    server_ = std::make_unique<MockS3Server>(
        [](const std::string&, const std::string& path) {
            if (path.find("list-type=2") != std::string::npos) {
                return MockS3Server::Response{200, listResultXml(
                    objXml("a.txt") + objXml("pre_x.txt") +
                    objXml("exact.log") + objXml("other.bin"))};
            }
            return defaultReply("", path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));

    // 后缀通配
    ListOptions suffix;
    suffix.filter = "*.txt";
    auto resources = browser.list_directory("", suffix);
    ASSERT_EQ(resources.size(), size_t{2});
    EXPECT_EQ(resources[0].name, "a.txt");
    EXPECT_EQ(resources[1].name, "pre_x.txt");

    // 前缀+后缀
    ListOptions both;
    both.filter = "pre_*.txt";
    resources = browser.list_directory("", both);
    ASSERT_EQ(resources.size(), size_t{1});
    EXPECT_EQ(resources[0].name, "pre_x.txt");

    // 无通配符按精确名匹配
    ListOptions exact;
    exact.filter = "exact.log";
    resources = browser.list_directory("", exact);
    ASSERT_EQ(resources.size(), size_t{1});
    EXPECT_EQ(resources[0].name, "exact.log");

    // 单独 "*" 匹配一切
    ListOptions all;
    all.filter = "*";
    resources = browser.list_directory("", all);
    ASSERT_EQ(resources.size(), size_t{4});
}

TEST_F(S3BrowserMockTest, ConnectSucceedsWithoutCredentials) {
    // 无凭据只告警不拒绝（公开只读 bucket 场景）
    S3Browser browser;
    std::map<std::string, std::string> options;
    options["endpoint"] = server_->base_url();
    EXPECT_TRUE(browser.connect("s3://" + std::string(kBucket), options));
}

TEST_F(S3BrowserMockTest, ConnectStripsEndpointTrailingSlash) {
    // endpoint 带尾斜杠：不得产生 "endpoint//bucket" 双斜杠路径
    S3Browser browser;
    std::map<std::string, std::string> options;
    options["endpoint"] = server_->base_url() + "/";
    options["access_key_id"] = "AKIA_TEST";
    options["secret_access_key"] = "secret";
    options["region"] = "us-west-2";
    ASSERT_TRUE(browser.connect("s3://" + std::string(kBucket), options));

    auto reqs = server_->requests();
    ASSERT_FALSE(reqs.empty());
    for (const auto& [method, path] : reqs) {
        EXPECT_EQ(path.find("//"), std::string::npos) << path;
    }
}

TEST_F(S3BrowserMockTest, KeySpecialCharactersPercentEncoded) {
    // key 中的空格逐段编码（'/' 保留）
    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));
    EXPECT_TRUE(browser.remove("docs/a b.txt"));

    bool saw_encoded = false;
    for (const auto& [method, path] : server_->requests()) {
        if (method == "DELETE" && path.find("a%20b.txt") != std::string::npos) {
            saw_encoded = true;
        }
        EXPECT_EQ(path.find("a b.txt"), std::string::npos) << path;
    }
    EXPECT_TRUE(saw_encoded);
}

TEST_F(S3BrowserMockTest, RenameFailsWhenCopyFails) {
    // copy（PUT 带 copy 语义）被服务器拒绝 → rename 失败且不删除源对象
    server_ = std::make_unique<MockS3Server>(
        [](const std::string& method, const std::string& path) {
            if (method == "PUT" && path.find("dest.txt") != std::string::npos) {
                return MockS3Server::Response{403, R"(<Error><Code>AccessDenied</Code></Error>)"};
            }
            return defaultReply(method, path);
        });
    ASSERT_TRUE(server_->start());

    S3Browser browser;
    ASSERT_TRUE(connectBrowser(browser));
    EXPECT_FALSE(browser.rename("docs/src.txt", "docs/dest.txt"));

    for (const auto& [method, path] : server_->requests()) {
        if (method == "DELETE") {
            ADD_FAILURE() << "rename 不得删除源对象: " << path;
        }
    }
}

/// 批次 W：无自定义 endpoint——build_url 落到 S3 virtual-host 官方域名
/// 分支（{bucket}.s3.{region}.amazonaws.com）。region "us-east-9z"
/// 不存在 → DNS 解析失败快速收口；URL 拼接先于请求，官方域名分支的
/// 行覆盖与请求结果无关（与 kodo/cos/upyun 的批次 V 用例同款）
TEST_F(S3BrowserMockTest, ConnectWithoutEndpointUsesOfficialDomainAndFails) {
    S3Browser browser;
    const std::map<std::string, std::string> options = {
        {"access_key_id", "AKIA_TEST"},
        {"secret_access_key", "secret"},
        {"region", "us-east-9z"}};
    EXPECT_FALSE(browser.connect("s3://" + std::string(kBucket), options));
}
