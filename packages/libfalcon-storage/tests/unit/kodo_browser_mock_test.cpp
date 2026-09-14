/**
 * @file kodo_browser_mock_test.cpp
 * @brief 七牛云Kodo资源浏览器 mock HTTP 测试
 * @author Falcon Team
 * @date 2026-09-14
 *
 * 覆盖 KodoBrowser 完整请求路径：connect（自定义 endpoint 生效，stat 探测）/
 * list_directory（POST /list、items JSON 解析、递归目录合成）/stat 信息/
 * exists 状态码语义/建目录（POST /put）/删除（POST /delete，含递归）/
 * rename（copy+delete）/配额，以及错误路径。七牛删除/复制/建目录成功返回
 * 200 + 空 body，是"成功判定看 body 非空"缺陷的回归点。离线可运行。
 *
 * 注：mock 服务器只解析请求行（body 不参与断言），故仅断言 method+path。
 */

#include <falcon/storage/kodo_browser.hpp>

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

int count_requests(const std::vector<std::pair<std::string, std::string>>& reqs,
                   const std::string& method, const std::string& path) {
    int n = 0;
    for (const auto& r : reqs) {
        if (r.first == method && r.second == path) ++n;
    }
    return n;
}

bool has_request(const std::vector<std::pair<std::string, std::string>>& reqs,
                 const std::string& method, const std::string& path) {
    return count_requests(reqs, method, path) > 0;
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
    R"({"items":[)"
    R"({"key":"docs/img.png","fsize":200,"hash":"hq1","mimeType":"image/png",)"
    R"("putTime":17356896000000},)"
    R"({"key":"docs/readme.md","fsize":100,"hash":"hr2"}]})";

/// 列举请求（POST /list）返回样例，其余 200 {}
MockServer::Response listAwareReply(const std::string& method, const std::string& path) {
    if (method == "POST" && path == "/list") {
        return {200, kListResponse};
    }
    return {200, "{}"};
}

/// 连接选项：endpoint 指向 mock
std::map<std::string, std::string> connect_options(const std::string& base) {
    return {{"access_key", "ak"}, {"secret_key", "sk"}, {"endpoint", base}};
}

bool connect_kodo(KodoBrowser& browser, const std::string& base) {
    return browser.connect("kodo://" + std::string(kBucket), connect_options(base));
}

} // namespace

//==============================================================================
// 连接
//==============================================================================

TEST(KodoBrowserMockTest, ConnectWithCustomEndpointSucceeds) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    EXPECT_TRUE(connect_kodo(browser, server->base_url()));

    // 自定义 endpoint 优先生效：探测请求 GET /stat/<base64url>，此前
    // endpoint 零消费，探测永远发往官方域名（真实网络，mock 无法触达）
    const auto reqs = server->requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].first, "GET");
    EXPECT_TRUE(reqs[0].second.rfind("/stat/", 0) == 0);
}

TEST(KodoBrowserMockTest, ConnectFailsWhenServerUnreachable) {
    KodoBrowser browser;
    EXPECT_FALSE(connect_kodo(browser, "http://127.0.0.1:1"));
}

//==============================================================================
// 列举
//==============================================================================

TEST(KodoBrowserMockTest, ListDirectoryPostsToListEndpoint) {
    auto server = make_server(listAwareReply);
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    auto resources = browser.list_directory("docs/", ListOptions{});
    ASSERT_EQ(resources.size(), 2u);

    EXPECT_TRUE(has_request(server->requests(), "POST", "/list"));

    EXPECT_EQ(resources[0].name, "img.png");
    EXPECT_EQ(resources[0].path, "docs/img.png");
    EXPECT_EQ(resources[0].size, 200u);
    EXPECT_EQ(resources[0].etag, "hq1");
    EXPECT_EQ(resources[0].mime_type, "image/png");
    // putTime/10000 → 秒级时间戳字符串
    EXPECT_EQ(resources[0].modified_time, "1735689600");
    EXPECT_TRUE(resources[0].is_file());
}

TEST(KodoBrowserMockTest, ListRecursiveSynthesizesDirectoryEntries) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "POST" && path == "/list") {
            return MockServer::Response{200,
                R"({"items":[)"
                R"({"key":"docs/a.txt","fsize":1},)"
                R"({"key":"docs/sub/b.txt","fsize":2},)"
                R"({"key":"docs/c.txt","fsize":3}]})"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    ListOptions options;
    options.recursive = true;
    auto resources = browser.list_directory("docs/", options);

    // 3 个文件 + 按 key 前缀合成的目录 "docs"（去重后只出现一次）
    ASSERT_EQ(resources.size(), 4u);
    bool dir_found = false;
    for (const auto& res : resources) {
        if (res.is_directory()) {
            dir_found = true;
            EXPECT_EQ(res.name, "docs");
            EXPECT_EQ(res.path, "docs");
        }
    }
    EXPECT_TRUE(dir_found);
}

TEST(KodoBrowserMockTest, ListDirectoryFiltersHidden) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"items":[)"
            R"({"key":"zeta.txt","fsize":1},)"
            R"({"key":".hidden","fsize":1}]})"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    auto resources = browser.list_directory("/", ListOptions{});
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].name, "zeta.txt");
}

TEST(KodoBrowserMockTest, ListDirectoryEmptyOnMalformedResponse) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, "not-json{"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    EXPECT_TRUE(browser.list_directory("/", ListOptions{}).empty());
}

TEST(KodoBrowserMockTest, ListDirectoryEmptyOnHttpError) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{403, "denied"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    // 连接探测本身也会失败
    EXPECT_FALSE(connect_kodo(browser, server->base_url()));
    EXPECT_TRUE(browser.list_directory("/", ListOptions{}).empty());
}

//==============================================================================
// 对象信息 / exists
//==============================================================================

TEST(KodoBrowserMockTest, GetResourceInfoParsesStatResponse) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET" && path.rfind("/stat/", 0) == 0) {
            return MockServer::Response{200,
                R"({"fsize":264,"hash":"FrUeabc","putTime":17356896000000,)"
                R"("mimeType":"text/markdown"})"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    auto info = browser.get_resource_info("docs/readme.md");
    EXPECT_EQ(info.name, "readme.md");
    EXPECT_EQ(info.path, "docs/readme.md");
    EXPECT_EQ(info.size, 264u);
    EXPECT_EQ(info.etag, "FrUeabc");
    EXPECT_EQ(info.modified_time, "1735689600");
    EXPECT_EQ(info.mime_type, "text/markdown");
}

TEST(KodoBrowserMockTest, ExistsFollowsStatResult) {
    auto server = make_server([](const std::string& method, const std::string&) {
        return method == "GET" ? MockServer::Response{200, "{}"}
                               : MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));
    // stat 成功 → info 非空 → 存在
    EXPECT_TRUE(browser.exists("docs/readme.md"));

    // 第 2 次及以后的 stat 返回 404（第 1 次是 connect 探测，须放行）
    int stat_count = 0;
    auto missing = make_server([&stat_count](const std::string& method,
                                             const std::string& path) {
        if (method == "GET" && path.rfind("/stat/", 0) == 0) {
            return ++stat_count >= 2 ? MockServer::Response{404, "no"}
                                     : MockServer::Response{200, "{}"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(missing, nullptr);
    KodoBrowser browser2;
    ASSERT_TRUE(connect_kodo(browser2, missing->base_url()));
    // stat 404 → info 为空 → exists false（原恒真条件把不存在也报"存在"）
    EXPECT_FALSE(browser2.exists("nope.txt"));
}

//==============================================================================
// 写操作（七牛成功响应是 200 + 空 body——成功判定缺陷的回归点）
//==============================================================================

TEST(KodoBrowserMockTest, CreateDirectoryPutsMarkerWithEmptyBody) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    // 200 + 空 body 也应视为成功
    EXPECT_TRUE(browser.create_directory("newdir"));
    EXPECT_TRUE(has_request(server->requests(), "POST", "/put"));
}

TEST(KodoBrowserMockTest, RemoveObjectSendsDelete) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    EXPECT_TRUE(browser.remove("docs/readme.md"));
    EXPECT_EQ(count_requests(server->requests(), "POST", "/delete"), 1);
}

TEST(KodoBrowserMockTest, RemoveRecursiveDeletesChildrenThenTarget) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "POST" && path == "/list") {
            return MockServer::Response{200,
                R"({"items":[)"
                R"({"key":"docs/a.txt","fsize":1},)"
                R"({"key":"docs/sub/b.txt","fsize":2}]})"};
        }
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    EXPECT_TRUE(browser.remove("docs", true));

    // 2 个子对象 + 目标自身
    EXPECT_EQ(count_requests(server->requests(), "POST", "/delete"), 3);
}

TEST(KodoBrowserMockTest, RenameCopiesThenDeletes) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    EXPECT_TRUE(browser.rename("a.txt", "b.txt"));
    EXPECT_EQ(count_requests(server->requests(), "POST", "/copy"), 1);
    EXPECT_EQ(count_requests(server->requests(), "POST", "/delete"), 1);
}

TEST(KodoBrowserMockTest, WriteOperationsFailOnHttpError) {
    // 写操作（POST）被 403 拒绝，读操作正常——写路径如实返回 false
    auto server = make_server([](const std::string& method, const std::string&) {
        return method == "POST" ? MockServer::Response{403, "forbidden"}
                                : MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    EXPECT_FALSE(browser.create_directory("newdir"));
    EXPECT_FALSE(browser.remove("a.txt"));
    EXPECT_FALSE(browser.rename("a.txt", "b.txt"));
}

//==============================================================================
// 配额
//==============================================================================

TEST(KodoBrowserMockTest, GetQuotaInfoParsesStorageBytes) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, R"({"bytes":123456,"count":42})"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    auto quota = browser.get_quota_info();
    EXPECT_EQ(quota["used"], 123456u);
    EXPECT_EQ(quota["object_count"], 42u);
}

TEST(KodoBrowserMockTest, GetQuotaInfoEmptyOnBadResponse) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, "{{bad"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    EXPECT_TRUE(browser.get_quota_info().empty());
}
