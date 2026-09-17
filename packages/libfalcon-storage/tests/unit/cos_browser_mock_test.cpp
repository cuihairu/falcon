/**
 * @file cos_browser_mock_test.cpp
 * @brief 腾讯云COS资源浏览器 mock HTTP 测试
 * @author Falcon Team
 * @date 2026-09-14
 *
 * 覆盖 COSBrowser 完整请求路径：connect（自定义 endpoint path-style +
 * app_id 桶名后缀生效）/ list_directory（查询串上 URL、JSON 解析、
 * CommonPrefixes）/HEAD 信息头解析/exists 状态码语义/建目录 marker/
 * 递归删除/rename（copy+delete）/配额，以及错误路径。离线可运行。
 */

#include <falcon/storage/cos_browser.hpp>

#include <gtest/gtest.h>

#include "mock_http_server.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace falcon;

namespace {

using MockServer = MockHttpServer;

const char* kBucket = "testbucket";
const char* kAppId = "12345";
// path-style 下的桶路径（endpoint + bucket-appid）
const char* kBucketPath = "/testbucket-12345";

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
    R"("LastModified":"2026-01-01T00:00:00Z","ETag":"\"e1\""}],)"
    R"("CommonPrefixes":[{"Prefix":"docs/sub/"}]})";

/// 列举请求（list-type=2）返回样例，其余 200 {}
MockServer::Response listAwareReply(const std::string& method, const std::string& path) {
    if (method == "GET" && path.find("list-type=2") != std::string::npos) {
        return {200, kListResponse};
    }
    return {200, "{}"};
}

/// 连接选项：endpoint 指向 mock（path-style），region/app_id 齐全
std::map<std::string, std::string> connect_options(const std::string& base) {
    return {{"secret_id", "id"}, {"secret_key", "key"},
            {"region", "ap-test"}, {"app_id", kAppId}, {"endpoint", base}};
}

bool connect_cos(COSBrowser& browser, const std::string& base) {
    return browser.connect("cos://" + std::string(kBucket), connect_options(base));
}

} // namespace

//==============================================================================
// 连接
//==============================================================================

TEST(COSBrowserMockTest, ConnectWithCustomEndpointSucceeds) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    EXPECT_TRUE(connect_cos(browser, server->base_url()));

    // 自定义 endpoint 走 path-style 且带 app_id 后缀：GET /testbucket-12345?max-keys=1
    EXPECT_TRUE(has_request(server->requests(), "GET",
                            std::string(kBucketPath) + "?max-keys=1"));
}

TEST(COSBrowserMockTest, ConnectFailsWhenServerUnreachable) {
    COSBrowser browser;
    EXPECT_FALSE(connect_cos(browser, "http://127.0.0.1:1"));
}

/// 批次 V：无自定义 endpoint——build_url 落到 COS virtual-host 官方
/// 域名分支（{bucket}-{app_id}.cos.{region}.myqcloud.com）。region
/// "ap-test" 不存在 → DNS 解析失败快速收口；URL 拼接先于请求，官方
/// 域名分支的行覆盖与请求结果无关
TEST(COSBrowserMockTest, ConnectWithoutEndpointUsesVirtualHostDomainAndFails) {
    COSBrowser browser;
    const std::map<std::string, std::string> options = {
        {"region", "ap-test"}, {"app_id", kAppId}};
    EXPECT_FALSE(
        browser.connect("cos://" + std::string(kBucket), options));
}

//==============================================================================
// 列举
//==============================================================================

TEST(COSBrowserMockTest, ListDirectoryAppendsQueryStringToUrl) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

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

TEST(COSBrowserMockTest, ListDirectoryParsesContentsAndCommonPrefixes) {
    auto server = make_server(listAwareReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

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

TEST(COSBrowserMockTest, ListDirectoryFiltersHiddenAndSortsByName) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"Contents":[)"
            R"({"Key":"zeta.txt","Size":1},)"
            R"({"Key":"alpha.txt","Size":1},)"
            R"({"Key":".hidden","Size":1}]})"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    ListOptions options;
    options.sort_by = "name";
    auto resources = browser.list_directory("/", options);
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_EQ(resources[0].name, "alpha.txt");
    EXPECT_EQ(resources[1].name, "zeta.txt");
}

TEST(COSBrowserMockTest, ListDirectoryEmptyOnMalformedResponse) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, "not-json{"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    EXPECT_TRUE(browser.list_directory("/", ListOptions{}).empty());
}

TEST(COSBrowserMockTest, ListDirectoryEmptyOnHttpError) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{403, "denied"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    // 连接探测本身也会失败
    EXPECT_FALSE(connect_cos(browser, server->base_url()));
    EXPECT_TRUE(browser.list_directory("/", ListOptions{}).empty());
}

//==============================================================================
// 对象信息 / exists
//==============================================================================

TEST(COSBrowserMockTest, GetResourceInfoHeadParsesResponseHeaders) {
    auto server = make_server([](const std::string& method, const std::string&) {
        if (method == "HEAD") {
            MockServer::Response resp{200, ""};
            resp.headers["Content-Length"] = "264";
            resp.headers["ETag"] = "\"abc123\"";
            resp.headers["Last-Modified"] = "Wed, 01 Jan 2026 00:00:00 GMT";
            resp.headers["Content-Type"] = "text/markdown";
            return resp;
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    auto info = browser.get_resource_info("docs/readme.md");
    EXPECT_EQ(info.name, "readme.md");
    EXPECT_EQ(info.path, "docs/readme.md");
    EXPECT_EQ(info.size, 264u);
    EXPECT_EQ(info.etag, "\"abc123\"");
    EXPECT_EQ(info.modified_time, "Wed, 01 Jan 2026 00:00:00 GMT");
    EXPECT_EQ(info.mime_type, "text/markdown");

    // HEAD 请求 path 保留 key 中的 '/'
    EXPECT_TRUE(has_request(server->requests(), "HEAD",
                            std::string(kBucketPath) + "/docs/readme.md"));
}

TEST(COSBrowserMockTest, ExistsFollowsHttpStatusCode) {
    auto server = make_server([](const std::string& method, const std::string&) {
        return method == "HEAD" ? MockServer::Response{200, ""}
                                : MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));
    EXPECT_TRUE(browser.exists("docs/readme.md"));

    // 404 只作用于 HEAD（GET 探测仍 200，保证 connect 成功）
    auto missing = make_server([](const std::string& method, const std::string&) {
        return method == "HEAD" ? MockServer::Response{404, ""}
                                : MockServer::Response{200, "{}"};
    });
    ASSERT_NE(missing, nullptr);
    COSBrowser browser2;
    ASSERT_TRUE(connect_cos(browser2, missing->base_url()));
    // HEAD 404 → info 为空 → exists false（原恒真条件把不存在也报"存在"）
    EXPECT_FALSE(browser2.exists("nope.txt"));
}

//==============================================================================
// 写操作
//==============================================================================

TEST(COSBrowserMockTest, CreateDirectoryPutsDirectoryMarker) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    // 204 No Content（空 body）也应视为成功
    EXPECT_TRUE(browser.create_directory("newdir"));
    EXPECT_TRUE(has_request(server->requests(), "PUT",
                            std::string(kBucketPath) + "/newdir/"));
}

TEST(COSBrowserMockTest, RemoveObjectSendsDelete) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    EXPECT_TRUE(browser.remove("docs/readme.md"));
    EXPECT_TRUE(has_request(server->requests(), "DELETE",
                            std::string(kBucketPath) + "/docs/readme.md"));
}

TEST(COSBrowserMockTest, RemoveRecursiveDeletesChildrenThenTarget) {
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

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    EXPECT_TRUE(browser.remove("docs", true));

    const auto deletes = delete_requests(server->requests());
    ASSERT_EQ(deletes.size(), 3u);
    EXPECT_EQ(deletes[0].second, "/testbucket-12345/docs/a.txt");
    EXPECT_EQ(deletes[1].second, "/testbucket-12345/docs/sub/b.txt");
    EXPECT_EQ(deletes[2].second, "/testbucket-12345/docs");
}

TEST(COSBrowserMockTest, RenameCopiesThenDeletes) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    EXPECT_TRUE(browser.rename("a.txt", "b.txt"));
    EXPECT_TRUE(has_request(server->requests(), "PUT",
                            std::string(kBucketPath) + "/b.txt"));
    EXPECT_TRUE(has_request(server->requests(), "DELETE",
                            std::string(kBucketPath) + "/a.txt"));
}

TEST(COSBrowserMockTest, WriteOperationsFailOnHttpError) {
    // 写操作（PUT/DELETE）被 403 拒绝，读操作正常——写路径如实返回 false
    auto server = make_server([](const std::string& method, const std::string&) {
        return (method == "PUT" || method == "DELETE")
            ? MockServer::Response{403, "forbidden"}
            : MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    EXPECT_FALSE(browser.create_directory("newdir"));
    EXPECT_FALSE(browser.remove("a.txt"));
    EXPECT_FALSE(browser.rename("a.txt", "b.txt"));
}

//==============================================================================
// 配额
//==============================================================================

TEST(COSBrowserMockTest, GetQuotaInfoParsesStorageBytes) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, R"({"Size":123456,"Count":42})"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    auto quota = browser.get_quota_info();
    EXPECT_EQ(quota["used"], 123456u);
    EXPECT_EQ(quota["object_count"], 42u);
}

TEST(COSBrowserMockTest, GetQuotaInfoEmptyOnBadResponse) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200, "{{bad"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    EXPECT_TRUE(browser.get_quota_info().empty());
}

//==============================================================================
// URL 解析（纯逻辑）
//==============================================================================

TEST(COSBrowserMockTest, UrlParserRejectsNonCosUrl) {
    // 缺少 cos:// 协议前缀
    EXPECT_THROW(COSUrlParser::parse("https://example.com/bucket/key"),
                 std::invalid_argument);
    EXPECT_THROW(COSUrlParser::parse("tencent://bucket/key"),
                 std::invalid_argument);
}

TEST(COSBrowserMockTest, UrlParserRejectsMissingHost) {
    // "cos://" 后没有任何 host
    EXPECT_THROW(COSUrlParser::parse("cos://"), std::invalid_argument);
}

//==============================================================================
// 浏览器元数据与协议支持
//==============================================================================

TEST(COSBrowserMockTest, BrowserMetadataAndProtocolSupport) {
    COSBrowser browser;
    EXPECT_EQ(browser.get_name(), "腾讯云COS");
    EXPECT_EQ(browser.get_supported_protocols(),
              (std::vector<std::string>{"cos", "tencent", "qcloud"}));

    EXPECT_TRUE(browser.can_handle("cos://bucket/key"));
    EXPECT_TRUE(browser.can_handle("tencent://bucket/key"));
    EXPECT_TRUE(browser.can_handle("https://bucket.cos.ap-beijing.myqcloud.com/key"));
    EXPECT_TRUE(browser.can_handle("https://bucket-123.cos.ap-beijing.myqcloud.com"));
    EXPECT_FALSE(browser.can_handle("https://example.com/file.zip"));
    EXPECT_FALSE(browser.can_handle(""));
}

//==============================================================================
// endpoint 形态
//==============================================================================

TEST(COSBrowserMockTest, ConnectStripsEndpointTrailingSlash) {
    // endpoint 带尾斜杠：不得产生 "endpoint//bucket-appid" 双斜杠路径
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    auto options = connect_options(server->base_url() + "/");
    ASSERT_TRUE(browser.connect("cos://" + std::string(kBucket), options));

    for (const auto& r : server->requests()) {
        EXPECT_EQ(r.second.find("//"), std::string::npos) << r.second;
    }
    EXPECT_TRUE(has_request(server->requests(), "GET",
                            std::string(kBucketPath) + "?max-keys=1"));
}

TEST(COSBrowserMockTest, ConnectWithHttpsEndpointFailsOnPlaintextMock) {
    // https:// scheme 的 endpoint 会被识别为自定义 endpoint（path-style），
    // 但明文 mock 端口经 TLS 握手必然失败——连接如实报败
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    auto options = connect_options("https://127.0.0.1:" +
                                   server->base_url().substr(server->base_url().rfind(':') + 1));
    EXPECT_FALSE(browser.connect("cos://" + std::string(kBucket), options));
}

TEST(COSBrowserMockTest, ConnectAcceptsTokenOption) {
    // token（临时密钥）选项可传入并正常连接
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    auto options = connect_options(server->base_url());
    options["token"] = "temporary-token";
    EXPECT_TRUE(browser.connect("cos://" + std::string(kBucket), options));
}

//==============================================================================
// 过滤 / 排序
//==============================================================================

TEST(COSBrowserMockTest, ListFilterWildcardPrefixSuffixExactAndStar) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"Contents":[)"
            R"({"Key":"a.txt","Size":1},)"
            R"({"Key":"pre_x.txt","Size":1},)"
            R"({"Key":"exact.log","Size":1},)"
            R"({"Key":"other.bin","Size":1}]})"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    // 后缀通配
    ListOptions suffix;
    suffix.filter = "*.txt";
    auto resources = browser.list_directory("/", suffix);
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_EQ(resources[0].name, "a.txt");
    EXPECT_EQ(resources[1].name, "pre_x.txt");

    // 前缀+后缀
    ListOptions both;
    both.filter = "pre_*.txt";
    resources = browser.list_directory("/", both);
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].name, "pre_x.txt");

    // 无通配符按精确名匹配
    ListOptions exact;
    exact.filter = "exact.log";
    resources = browser.list_directory("/", exact);
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].name, "exact.log");

    // 单独 "*" 匹配一切
    ListOptions all;
    all.filter = "*";
    resources = browser.list_directory("/", all);
    ASSERT_EQ(resources.size(), 4u);
}

TEST(COSBrowserMockTest, ListSortsByNameDescending) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"Contents":[)"
            R"({"Key":"alpha.txt","Size":1},)"
            R"({"Key":"zeta.txt","Size":1}]})"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    ListOptions options;
    options.sort_by = "name";
    options.sort_desc = true;
    auto resources = browser.list_directory("/", options);
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_EQ(resources[0].name, "zeta.txt");
    EXPECT_EQ(resources[1].name, "alpha.txt");
}

TEST(COSBrowserMockTest, ListSortsBySizeAscendingAndDescending) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"Contents":[)"
            R"({"Key":"c.bin","Size":300},)"
            R"({"Key":"a.bin","Size":100},)"
            R"({"Key":"b.bin","Size":200}]})"};
    });
    ASSERT_NE(server, nullptr);

    COSBrowser browser;
    ASSERT_TRUE(connect_cos(browser, server->base_url()));

    ListOptions asc;
    asc.sort_by = "size";
    auto resources = browser.list_directory("/", asc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "a.bin");
    EXPECT_EQ(resources[2].name, "c.bin");

    ListOptions desc;
    desc.sort_by = "size";
    desc.sort_desc = true;
    resources = browser.list_directory("/", desc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "c.bin");
    EXPECT_EQ(resources[2].name, "a.bin");
}

//==============================================================================
// 目录辅助与断开
//==============================================================================

TEST(COSBrowserMockTest, DirectoryHelpersAreInMemory) {
    COSBrowser browser;
    EXPECT_EQ(browser.get_root_path(), "/");
    EXPECT_TRUE(browser.change_directory("docs"));
    EXPECT_EQ(browser.get_current_directory(), "docs");
    browser.disconnect();  // 无状态协议，无副作用
}

TEST(COSBrowserMockTest, DisconnectIsSafeBeforeAndAfterConnect) {
    COSBrowser browser;
    browser.disconnect();  // 未连接也安全
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(connect_cos(browser, server->base_url()));
    browser.disconnect();
}
