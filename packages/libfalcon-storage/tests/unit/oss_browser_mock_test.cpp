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
#include <stdexcept>
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

//==============================================================================
// 过滤/排序/选项/元数据补充
//==============================================================================

TEST(OSSBrowserMockTest, ConnectThrowsOnUrlWithoutHost) {
    // "oss://" 无 host：URL 解析在网络之前即拒绝
    OSSBrowser browser;
    EXPECT_THROW(browser.connect("oss://", connect_options("http://127.0.0.1:1")),
                 std::invalid_argument);
}

TEST(OSSBrowserMockTest, ConnectStripsEndpointSchemeAndTrailingSlash) {
    // endpoint 带 scheme 与尾斜杠：剥 scheme 后剥 '/'，不得产生
    // "endpoint//bucket" 双斜杠路径
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    std::map<std::string, std::string> options = connect_options(server->base_url() + "/");
    EXPECT_TRUE(browser.connect("oss://" + std::string(kBucket), options));

    for (const auto& r : server->requests()) {
        EXPECT_EQ(r.second.find("//"), std::string::npos) << r.second;
    }
    EXPECT_TRUE(has_request(server->requests(), "GET",
                            "/" + std::string(kBucket) + "?max-keys=1"));
}

TEST(OSSBrowserMockTest, ConnectHonorsRegionOption) {
    // region 与 endpoint 同时给出：endpoint 优先（不得推导官方域名
    // 覆盖自定义 endpoint），请求仍发往 mock 的 path-style 地址
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    auto options = connect_options(server->base_url());
    options.emplace("region", "cn-hangzhou");
    EXPECT_TRUE(browser.connect("oss://" + std::string(kBucket), options));
    EXPECT_TRUE(has_request(server->requests(), "GET",
                            "/" + std::string(kBucket) + "?max-keys=1"));
}

TEST(OSSBrowserMockTest, ConnectHonorsSecurityTokenOption) {
    // STS 临时凭据：token 随选项接受，连接与列举照常
    auto server = make_server(listAwareReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    auto options = connect_options(server->base_url());
    options.emplace("security_token", "sts-token-xyz");
    ASSERT_TRUE(browser.connect("oss://" + std::string(kBucket), options));

    auto resources = browser.list_directory("docs/", ListOptions{});
    EXPECT_FALSE(resources.empty());
}

TEST(OSSBrowserMockTest, DisconnectIsSafeAfterConnect) {
    // 无状态协议：disconnect 无副作用，之后浏览器不可再用即可
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));
    browser.disconnect();
}

TEST(OSSBrowserMockTest, BrowserMetadataAndProtocolDetection) {
    OSSBrowser browser;
    EXPECT_EQ(browser.get_name(), "阿里云OSS");

    const auto protocols = browser.get_supported_protocols();
    EXPECT_EQ(protocols, (std::vector<std::string>{"oss", "aliyun", "oss-cn"}));

    EXPECT_TRUE(browser.can_handle("oss://bucket/key"));
    EXPECT_TRUE(browser.can_handle("aliyun://bucket/key"));
    EXPECT_TRUE(browser.can_handle("https://bucket.oss-cn-hangzhou.aliyuncs.com/key"));
    EXPECT_TRUE(browser.can_handle("https://oss-aliyuncs.com/bucket"));
    EXPECT_FALSE(browser.can_handle("https://example.com/file.zip"));
    EXPECT_FALSE(browser.can_handle("s3://bucket/key"));
}

TEST(OSSBrowserMockTest, DirectoryHelpersAreInMemory) {
    OSSBrowser browser;
    EXPECT_TRUE(browser.get_current_directory().empty());
    EXPECT_TRUE(browser.change_directory("docs/"));
    EXPECT_EQ(browser.get_current_directory(), "docs/");
    EXPECT_EQ(browser.get_root_path(), "/");
}

TEST(OSSBrowserMockTest, ListSortsByNameDescending) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"Contents":[)"
            R"({"Key":"alpha.txt","Size":1},)"
            R"({"Key":"zeta.txt","Size":1}]})"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    ListOptions options;
    options.sort_by = "name";
    options.sort_desc = true;
    auto resources = browser.list_directory("/", options);
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_EQ(resources[0].name, "zeta.txt");
    EXPECT_EQ(resources[1].name, "alpha.txt");
}

TEST(OSSBrowserMockTest, ListSortsBySizeAscendingAndDescending) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"Contents":[)"
            R"({"Key":"c.bin","Size":300},)"
            R"({"Key":"a.bin","Size":100},)"
            R"({"Key":"b.bin","Size":200}]})"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

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

TEST(OSSBrowserMockTest, ListFilterWildcardSuffixPrefixExactAndStar) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{200,
            R"({"Contents":[)"
            R"({"Key":"a.txt","Size":1},)"
            R"({"Key":"pre_x.txt","Size":1},)"
            R"({"Key":"exact.log","Size":1},)"
            R"({"Key":"other.bin","Size":1}]})"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    // 后缀通配
    ListOptions suffix;
    suffix.filter = "*.txt";
    auto resources = browser.list_directory("/", suffix);
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_EQ(resources[0].name, "a.txt");
    EXPECT_EQ(resources[1].name, "pre_x.txt");

    // 前缀 + 后缀
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

//==============================================================================
// 排序：modified_time（此前 sort_resources 完全忽略该排序键，静默不排）
//==============================================================================

TEST(OSSBrowserMockTest, ListSortsByModifiedTimeAscendingAndDescending) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET" && path.find("list-type=2") != std::string::npos) {
            return MockServer::Response{200,
                R"({"Contents":[)"
                R"({"Key":"old.txt","Size":1,"LastModified":"2026-01-01T00:00:00Z"},)"
                R"({"Key":"mid.txt","Size":1,"LastModified":"2026-06-01T00:00:00Z"},)"
                R"({"Key":"new.txt","Size":1,"LastModified":"2026-09-01T00:00:00Z"}]})"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    OSSBrowser browser;
    ASSERT_TRUE(connect_oss(browser, server->base_url()));

    ListOptions asc;
    asc.sort_by = "modified_time";
    auto resources = browser.list_directory("/", asc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "old.txt");
    EXPECT_EQ(resources[2].name, "new.txt");

    ListOptions desc;
    desc.sort_by = "modified_time";
    desc.sort_desc = true;
    resources = browser.list_directory("/", desc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "new.txt");
    EXPECT_EQ(resources[2].name, "old.txt");
}

//==============================================================================
// endpoint 带 https:// scheme 的 path-style 前缀分支（96 行）。指向
// 活着的明文 mock 会互等死锁（TLS ClientHello vs 等请求行），指向
// 无人监听的端口则连接立即被拒——URL 构造分支照样执行
//==============================================================================

TEST(OSSBrowserMockTest, ConnectHttpsEndpointPrefixStillPathStyle) {
    OSSBrowser browser;
    std::map<std::string, std::string> options = {
        {"access_key_id", "ak"}, {"access_key_secret", "sk"},
        {"endpoint", "https://127.0.0.1:1"}};
    EXPECT_FALSE(browser.connect("oss://" + std::string(kBucket), options));
}

/// 批次 W：只传 region 不传 endpoint——connect 的 endpoint 推导
///（"oss-" + region + ".aliyuncs.com"）与 build_url 的官方域名虚拟
/// 主机拼接两处一并走到；region "cn-tesla-9" 不存在 → DNS 失败收口
TEST(OSSBrowserMockTest, ConnectWithRegionOnlyDerivesEndpointAndFails) {
    OSSBrowser browser;
    const std::map<std::string, std::string> options = {
        {"access_key_id", "ak"}, {"access_key_secret", "sk"},
        {"region", "cn-tesla-9"}};
    EXPECT_FALSE(browser.connect("oss://" + std::string(kBucket), options));
}
