/**
 * @file upyun_browser_mock_test.cpp
 * @brief 又拍云USS资源浏览器 mock HTTP 测试
 * @author Falcon Team
 * @date 2026-09-14
 *
 * 覆盖 UpyunBrowser 完整请求路径：connect（自定义 api_domain path-style 生效，
 * GET /usage/ 探测）/ list_directory（文本行格式 name\tsize\ttype\ttime 解析、
 * 递归子目录列举）/HEAD 信息头解析/exists 状态码语义/建目录（POST + folder
 * 头）/递归删除（子路径前导 '/' 不拼出 "//"）/rename（copy+delete）/配额，
 * 以及错误路径。离线可运行。
 */

#include <falcon/storage/upyun_browser.hpp>

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

bool any_request_contains(
    const std::vector<std::pair<std::string, std::string>>& reqs,
    const std::string& needle) {
    for (const auto& r : reqs) {
        if (r.second.find(needle) != std::string::npos) return true;
    }
    return false;
}

int count_requests(const std::vector<std::pair<std::string, std::string>>& reqs,
                   const std::string& method) {
    int n = 0;
    for (const auto& r : reqs) {
        if (r.first == method) ++n;
    }
    return n;
}

std::unique_ptr<MockServer> make_server(MockServer::Handler handler) {
    auto server = std::make_unique<MockServer>(std::move(handler));
    if (!server->start()) {
        return nullptr;
    }
    return server;
}

/// 默认 handler：一切请求返回 200 + 空 body
MockServer::Response defaultReply(const std::string&, const std::string&) {
    return {200, ""};
}

/// 又拍云列表为文本行格式：name\tsize\ttype\ttime（type "N"=目录，按项目契约）
MockServer::Response listAwareReply(const std::string& method, const std::string& path) {
    if (method == "GET" && path == "/docs/") {
        return {200, "img.png\t200\tF\t1700000000\nsub\t0\tN\t1700000001\n"};
    }
    return {200, ""};
}

/// 连接选项：api_domain 指向 mock（path-style）
std::map<std::string, std::string> connect_options(const std::string& base) {
    return {{"username", "operator"}, {"password", "pass"},
            {"api_domain", base}};
}

bool connect_upyun(UpyunBrowser& browser, const std::string& base) {
    return browser.connect("upyun://" + std::string(kBucket), connect_options(base));
}

} // namespace

//==============================================================================
// 连接
//==============================================================================

TEST(UpyunBrowserMockTest, ConnectWithCustomApiDomainSucceeds) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    EXPECT_TRUE(connect_upyun(browser, server->base_url()));

    // 自定义 api_domain 走 path-style：探测请求 GET /usage/。此前 api_domain
    // 无 scheme 判定，mock 域名被拼成 bucket.http://... 不可达
    EXPECT_TRUE(has_request(server->requests(), "GET", "/usage/"));
}

TEST(UpyunBrowserMockTest, ConnectFailsWhenServerUnreachable) {
    UpyunBrowser browser;
    EXPECT_FALSE(connect_upyun(browser, "http://127.0.0.1:1"));
}

//==============================================================================
// 列举
//==============================================================================

TEST(UpyunBrowserMockTest, ListDirectoryParsesTextLines) {
    auto server = make_server(listAwareReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    auto resources = browser.list_directory("docs/", ListOptions{});
    ASSERT_EQ(resources.size(), 2u);

    EXPECT_EQ(resources[0].name, "img.png");
    EXPECT_EQ(resources[0].path, "/docs/img.png");
    EXPECT_EQ(resources[0].size, 200u);
    EXPECT_EQ(resources[0].modified_time, "1700000000");
    EXPECT_TRUE(resources[0].is_file());

    EXPECT_EQ(resources[1].name, "sub");
    EXPECT_EQ(resources[1].path, "/docs/sub");
    EXPECT_TRUE(resources[1].is_directory());

    EXPECT_TRUE(has_request(server->requests(), "GET", "/docs/"));
}

TEST(UpyunBrowserMockTest, ListRecursiveDescendsIntoSubdirectories) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET" && path == "/docs/") {
            return MockServer::Response{200, "img.png\t200\tF\t1700000000\nsub\t0\tN\t1700000001\n"};
        }
        if (method == "GET" && path == "/docs/sub/") {
            return MockServer::Response{200, "b.txt\t50\tF\t1700000002\n"};
        }
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    ListOptions options;
    options.recursive = true;
    auto resources = browser.list_directory("docs/", options);

    // 顶层 2 项 + 子目录 1 项；递归边遍历边插入曾致悬垂迭代（UB/崩溃）
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_TRUE(has_request(server->requests(), "GET", "/docs/"));
    EXPECT_TRUE(has_request(server->requests(), "GET", "/docs/sub/"));
    bool sub_file_found = false;
    for (const auto& res : resources) {
        if (res.path == "/docs/sub/b.txt") {
            sub_file_found = true;
            EXPECT_EQ(res.size, 50u);
        }
    }
    EXPECT_TRUE(sub_file_found);
}

TEST(UpyunBrowserMockTest, ListDirectoryFiltersHidden) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            "zeta.txt\t1\tF\t1700000000\n.hidden\t1\tF\t1700000000\n"};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    ListOptions options;
    options.sort_by = "name";
    auto resources = browser.list_directory("/", options);
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].name, "zeta.txt");
}

TEST(UpyunBrowserMockTest, ListDirectoryEmptyOnHttpError) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{403, "denied"};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    // 连接探测本身也会失败
    EXPECT_FALSE(connect_upyun(browser, server->base_url()));
    EXPECT_TRUE(browser.list_directory("/", ListOptions{}).empty());
}

//==============================================================================
// 对象信息 / exists
//==============================================================================

TEST(UpyunBrowserMockTest, GetResourceInfoHeadParsesResponseHeaders) {
    auto server = make_server([](const std::string& method, const std::string&) {
        if (method == "HEAD") {
            MockServer::Response resp{200, ""};
            resp.headers["Content-Length"] = "264";
            resp.headers["Last-Modified"] = "Wed, 01 Jan 2026 00:00:00 GMT";
            return resp;
        }
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    auto info = browser.get_resource_info("docs/readme.md");
    EXPECT_EQ(info.name, "readme.md");
    EXPECT_EQ(info.path, "docs/readme.md");
    EXPECT_EQ(info.size, 264u);
    EXPECT_EQ(info.modified_time, "Wed, 01 Jan 2026 00:00:00 GMT");

    EXPECT_TRUE(has_request(server->requests(), "HEAD", "/docs/readme.md"));
}

TEST(UpyunBrowserMockTest, ExistsFollowsHttpStatusCode) {
    auto server = make_server([](const std::string& method, const std::string&) {
        return method == "HEAD" ? MockServer::Response{200, ""}
                                : MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));
    EXPECT_TRUE(browser.exists("docs/readme.md"));

    // 404 只作用于 HEAD（GET 探测仍 200，保证 connect 成功）
    auto missing = make_server([](const std::string& method, const std::string&) {
        return method == "HEAD" ? MockServer::Response{404, ""}
                                : MockServer::Response{200, ""};
    });
    ASSERT_NE(missing, nullptr);
    UpyunBrowser browser2;
    ASSERT_TRUE(connect_upyun(browser2, missing->base_url()));
    // HEAD 404 → info 为空 → exists false（原恒真条件把不存在也报"存在"）
    EXPECT_FALSE(browser2.exists("nope.txt"));
}

//==============================================================================
// 写操作（又拍云写操作成功是 200 + 空 body）
//==============================================================================

TEST(UpyunBrowserMockTest, CreateDirectoryPostsWithFolderHeader) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    EXPECT_TRUE(browser.create_directory("newdir"));
    EXPECT_TRUE(has_request(server->requests(), "POST", "/newdir/"));
}

TEST(UpyunBrowserMockTest, RemoveObjectSendsDelete) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    EXPECT_TRUE(browser.remove("docs/readme.md"));
    EXPECT_TRUE(has_request(server->requests(), "DELETE", "/docs/readme.md"));
}

TEST(UpyunBrowserMockTest, RemoveRecursiveDeletesWithoutDoubleSlash) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET" && path == "/docs/") {
            return MockServer::Response{200, "img.png\t200\tF\t1700000000\nsub\t0\tN\t1700000001\n"};
        }
        if (method == "GET" && path == "/docs/sub/") {
            return MockServer::Response{200, "b.txt\t50\tF\t1700000002\n"};
        }
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    EXPECT_TRUE(browser.remove("docs", true));

    // 递归删除：子对象先删、目标最后删；子目录列举返回的 path 已带前导
    // '/'，再拼 "/" 曾产生 "//docs/..."（真实服务上必然 404）
    EXPECT_TRUE(has_request(server->requests(), "DELETE", "/docs/sub/b.txt"));
    EXPECT_TRUE(has_request(server->requests(), "DELETE", "/docs/img.png"));
    EXPECT_TRUE(has_request(server->requests(), "DELETE", "/docs"));
    EXPECT_EQ(count_requests(server->requests(), "DELETE"), 3);
    EXPECT_FALSE(any_request_contains(server->requests(), "//"));
}

TEST(UpyunBrowserMockTest, RenameCopiesThenDeletes) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    EXPECT_TRUE(browser.rename("a.txt", "b.txt"));
    EXPECT_TRUE(has_request(server->requests(), "PUT", "/b.txt"));
    EXPECT_TRUE(has_request(server->requests(), "DELETE", "/a.txt"));
}

TEST(UpyunBrowserMockTest, WriteOperationsFailOnHttpError) {
    // 写操作（POST/PUT/DELETE）被 403 拒绝，读操作正常——写路径如实返回 false
    auto server = make_server([](const std::string& method, const std::string&) {
        return (method == "POST" || method == "PUT" || method == "DELETE")
            ? MockServer::Response{403, "forbidden"}
            : MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    EXPECT_FALSE(browser.create_directory("newdir"));
    EXPECT_FALSE(browser.remove("a.txt"));
    EXPECT_FALSE(browser.rename("a.txt", "b.txt"));
}

//==============================================================================
// 配额
//==============================================================================

TEST(UpyunBrowserMockTest, GetQuotaInfoParsesUsageResponse) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, R"({"space":123456,"amount":42})"};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    auto quota = browser.get_quota_info();
    EXPECT_EQ(quota["used"], 123456u);
    EXPECT_EQ(quota["file_count"], 42u);
}

TEST(UpyunBrowserMockTest, GetQuotaInfoEmptyOnBadResponse) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, "{{bad"};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    EXPECT_TRUE(browser.get_quota_info().empty());
}
