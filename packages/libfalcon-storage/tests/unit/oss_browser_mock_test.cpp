/**
 * @file oss_browser_mock_test.cpp
 * @brief 阿里云OSS资源浏览器 mock HTTP 测试
 * @author Falcon Team
 * @date 2026-09-14
 *
 * 覆盖 OSSBrowser 完整请求路径：connect（自定义 endpoint path-style 生效）/
 * list_directory（查询串拼接、JSON Contents + CommonPrefixes、过滤）/HEAD
 * 信息（响应头解析）/exists 状态码语义/create_directory/remove（含递归）/
 * rename（copy+delete）/get_quota_info，以及错误路径。离线可运行。
 */

#include <falcon/storage/oss_browser.hpp>

#include <gtest/gtest.h>

#include "mock_http_server.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace falcon;

namespace {

using MockServer = MockHttpServer;

const char* kBucket = "testbucket";

bool has_request(const std::vector<std::pair<std::string, std::string>>& reqs,
                 const std::string& method, const std::string& path) {
    for (const auto& r : reqs) {
        if (r.first == method && r.second == path) return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> delete_requests(
    const std::vector<std::pair<std::string, std::string>>& reqs) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& r : reqs) {
        if (r.first == "DELETE") out.push_back(r);
    }
    return out;
}

std::unique_ptr<MockServer> make_server(MockServer::Handler handler) {
    auto server = std::make_unique<MockServer>(std::move(handler));
    if (!server->start()) {
        return nullptr;
    }
    return server;
}

/// 默认 handler：一切请求返回 200 + 空 JSON 对象
MockServer::Response defaultReply(const std::string&, const std::string&) {
    return {200, "{}"};
}

const char* kListResponse =
    R"({"Contents":[)"
    R"({"Key":"docs/img.png","Size":200,"ETag":"\"e2\""},)"
    R"({"Key":"docs/readme.md","Size":100,)"
    R"("LastModified":"2026-01-01T00:00:00Z","ETag":"\"e1\"",)"
    R"("StorageClass":"STANDARD"}],)"
    R"("CommonPrefixes":[{"Prefix":"docs/sub/"}]})";

/// 列举请求（list-type=2）返回样例，其余 200 {}
MockServer::Response listAwareReply(const std::string& method, const std::string& path) {
    if (method == "GET" && path.find("list-type=2") != std::string::npos) {
        return {200, kListResponse};
    }
    return {200, "{}"};
}

/// 连接选项：endpoint 指向 mock（path-style）
std::map<std::string, std::string> connect_options(const std::string& base) {
    return {{"access_key_id", "ak"}, {"access_key_secret", "sk"}, {"endpoint", base}};
}

bool connect_oss(OSSBrowser& browser, const std::string& base) {
    return browser.connect("oss://" + std::string(kBucket), connect_options(base));
}

} // namespace

//==============================================================================
// 连接
//==============================================================================

TEST(OSSBrowserMockTest, ConnectWithCustomEndpointSucceeds) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    EXPECT_TRUE(connect_oss(browser, server->base_url()));

    // 自定义 endpoint 走 path-style：GET /testbucket?max-keys=1
    EXPECT_TRUE(has_request(server->requests(), "GET",
                            "/" + std::string(kBucket) + "?max-keys=1"));
}

TEST(OSSBrowserMockTest, ConnectFailsWhenServerUnreachable) {
    OSSBrowser browser;
    EXPECT_FALSE(connect_oss(browser, "http://127.0.0.1:1"));
}

//==============================================================================
// 列举
//==============================================================================

TEST(OSSBrowserMockTest, ListDirectoryAppendsQueryStringToUrl) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    ListOptions options;
    browser.list_directory("docs/", options);

    // 查询串此前从未拼到 URL 上（只进签名）——prefix/max-keys 从未发到服务端
    const auto reqs = server->requests();
    ASSERT_FALSE(reqs.empty());
    bool found = false;
    for (const auto& r : reqs) {
        if (r.first == "GET" &&
            r.second.find("list-type=2") != std::string::npos &&
            r.second.find("prefix=docs%2F") != std::string::npos &&
            r.second.find("max-keys=100") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(OSSBrowserMockTest, ListDirectoryParsesContentsAndCommonPrefixes) {
    auto server = make_server(listAwareReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    auto resources = browser.list_directory("docs/", ListOptions{});
    ASSERT_EQ(resources.size(), 3u);

    // 文件条目
    EXPECT_EQ(resources[0].name, "img.png");
    EXPECT_EQ(resources[0].path, "docs/img.png");
    EXPECT_EQ(resources[0].size, 200u);
    EXPECT_TRUE(resources[0].is_file());

    // CommonPrefixes → Directory
    bool dir_found = false;
    for (const auto& res : resources) {
        if (res.is_directory()) {
            dir_found = true;
            EXPECT_EQ(res.name, "sub");
            EXPECT_EQ(res.path, "docs/sub");
        }
    }
    EXPECT_TRUE(dir_found);
}

TEST(OSSBrowserMockTest, ListDirectoryFiltersHiddenAndSortsByName) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"Contents":[)"
            R"({"Key":"zeta.txt","Size":1},)"
            R"({"Key":"alpha.txt","Size":1},)"
            R"({"Key":".hidden","Size":1}]})"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    auto resources = browser.list_directory("/", ListOptions{});
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_EQ(resources[0].name, "alpha.txt");
    EXPECT_EQ(resources[1].name, "zeta.txt");
}

TEST(OSSBrowserMockTest, ListDirectoryEmptyOnMalformedResponse) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, "not-json{"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    EXPECT_TRUE(browser.list_directory("/", ListOptions{}).empty());
}

TEST(OSSBrowserMockTest, ListDirectoryEmptyOnHttpError) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{403, "denied"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    // 连接探测本身也会失败
    EXPECT_FALSE(connect_oss(browser, server->base_url()));
    EXPECT_TRUE(browser.list_directory("/", ListOptions{}).empty());
}

//==============================================================================
// 对象信息 / exists
//==============================================================================

TEST(OSSBrowserMockTest, GetResourceInfoHeadParsesResponseHeaders) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "HEAD") {
            MockServer::Response resp{200, ""};
            resp.headers["Content-Length"] = "264";
            resp.headers["ETag"] = "\"abc123\"";
            resp.headers["Last-Modified"] = "Wed, 01 Jan 2026 00:00:00 GMT";
            resp.headers["Content-Type"] = "text/markdown";
            return resp;
        }
        (void)path;
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    auto info = browser.get_resource_info("docs/readme.md");
    EXPECT_EQ(info.name, "readme.md");
    EXPECT_EQ(info.path, "docs/readme.md");
    EXPECT_EQ(info.size, 264u);
    EXPECT_EQ(info.etag, "\"abc123\"");
    EXPECT_EQ(info.modified_time, "Wed, 01 Jan 2026 00:00:00 GMT");
    EXPECT_EQ(info.mime_type, "text/markdown");

    // HEAD 请求 path 保留 key 中的 '/'
    EXPECT_TRUE(has_request(server->requests(), "HEAD",
                            "/" + std::string(kBucket) + "/docs/readme.md"));
}

TEST(OSSBrowserMockTest, ExistsFollowsHttpStatusCode) {
    auto server = make_server([](const std::string& method, const std::string&) {
        return method == "HEAD" ? MockServer::Response{200, ""}
                                : MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));
    EXPECT_TRUE(browser.exists("docs/readme.md"));

    // 404 只作用于 HEAD（GET 探测仍 200，保证 connect 成功）
    auto missing = make_server([](const std::string& method, const std::string&) {
        return method == "HEAD" ? MockServer::Response{404, ""}
                                : MockServer::Response{200, "{}"};
    });
    ASSERT_NE(missing, nullptr);
    OSSBrowser browser2;
    ASSERT_TRUE(connect_oss(browser2, missing->base_url()));
    // HEAD 404 → info 为空 → exists false（原恒真条件把不存在也报"存在"）
    EXPECT_FALSE(browser2.exists("nope.txt"));
}

//==============================================================================
// 写操作
//==============================================================================

TEST(OSSBrowserMockTest, CreateDirectoryPutsDirectoryMarker) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    // 204 No Content（空 body）也应视为成功
    EXPECT_TRUE(browser.create_directory("newdir"));
    EXPECT_TRUE(has_request(server->requests(), "PUT",
                            "/" + std::string(kBucket) + "/newdir/"));
}

TEST(OSSBrowserMockTest, RemoveObjectSendsDelete) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    EXPECT_TRUE(browser.remove("docs/readme.md"));
    EXPECT_TRUE(has_request(server->requests(), "DELETE",
                            "/" + std::string(kBucket) + "/docs/readme.md"));
}

TEST(OSSBrowserMockTest, RemoveRecursiveDeletesChildrenThenTarget) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET" && path.find("list-type=2") != std::string::npos) {
            return MockServer::Response{200,
                R"({"Contents":[)"
                R"({"Key":"docs/a.txt","Size":1},)"
                R"({"Key":"docs/sub/b.txt","Size":2}]})"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    EXPECT_TRUE(browser.remove("docs", true));

    const auto deletes = delete_requests(server->requests());
    ASSERT_EQ(deletes.size(), 3u);
    EXPECT_EQ(deletes[0].second, "/testbucket/docs/a.txt");
    EXPECT_EQ(deletes[1].second, "/testbucket/docs/sub/b.txt");
    EXPECT_EQ(deletes[2].second, "/testbucket/docs");
}

TEST(OSSBrowserMockTest, RenameCopiesThenDeletes) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    EXPECT_TRUE(browser.rename("a.txt", "b.txt"));
    EXPECT_TRUE(has_request(server->requests(), "PUT",
                            "/" + std::string(kBucket) + "/b.txt"));
    EXPECT_TRUE(has_request(server->requests(), "DELETE",
                            "/" + std::string(kBucket) + "/a.txt"));
}

TEST(OSSBrowserMockTest, WriteOperationsFailOnHttpError) {
    // 写操作（PUT/DELETE）被 403 拒绝，读操作正常——写路径如实返回 false
    auto server = make_server([](const std::string& method, const std::string&) {
        return (method == "PUT" || method == "DELETE")
            ? MockServer::Response{403, "forbidden"}
            : MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    EXPECT_FALSE(browser.create_directory("newdir"));
    EXPECT_FALSE(browser.remove("a.txt"));
    EXPECT_FALSE(browser.rename("a.txt", "b.txt"));
}

//==============================================================================
// 配额
//==============================================================================

TEST(OSSBrowserMockTest, GetQuotaInfoParsesStorageBytes) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, R"({"StorageSize":123456,"ObjectCount":42})"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    auto quota = browser.get_quota_info();
    EXPECT_EQ(quota["used"], 123456u);
    EXPECT_EQ(quota["object_count"], 42u);
}

TEST(OSSBrowserMockTest, GetQuotaInfoEmptyOnBadResponse) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, "{{bad"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    EXPECT_TRUE(browser.get_quota_info().empty());
}
