/**
 * @file webdav_browser_mock_test.cpp
 * @brief WebDAV 资源浏览器 mock HTTP 测试
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 覆盖 WebDavBrowser 的完整请求路径：connect（Depth:0 探活、401 Basic
 * 挑战协商）/list_directory（207 multistatus 解析、自条目剔除、href 百分
 * 号解码、命名空间大小写、过滤排序递归）/get_resource_info/exists/
 * create_directory（含逐级 MKCOL）/remove（Depth: infinity 与逐项兜底）/
 * rename/copy（MOVE/COPY Destination）/get_quota_info（RFC 4331 缺失容忍），
 * 以及 URL 解析与工厂注册。
 * 服务器监听 INADDR_ANY、绑定随机端口，离线可运行。
 */

#include <falcon/storage/webdav_browser.hpp>

#include <gtest/gtest.h>

#include "mock_http_server.hpp"

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace falcon;

namespace {

/// WebDAV mock：复用共享 MockHttpServer（一连接一请求 + 编程式应答）
using MockDavServer = MockHttpServer;

/// 207 multistatus 包装
std::string multistatusXml(const std::string& responses) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<d:multistatus xmlns:d=\"DAV:\">" + responses + "</d:multistatus>";
}

/// 单个 <response> 条目：href + prop 字段按需携带（模拟实现侧字段裁剪）
std::string davEntry(const std::string& href,
                     bool is_dir,
                     uint64_t size = 0,
                     const std::string& modified = "",
                     const std::string& etag = "",
                     const std::string& content_type = "",
                     const std::string& displayname = "") {
    std::string s = "<d:response><d:href>" + href + "</d:href>"
                    "<d:propstat><d:prop>";
    if (is_dir) {
        s += "<d:resourcetype><d:collection/></d:resourcetype>";
    } else {
        s += "<d:resourcetype/>";
        s += "<d:getcontentlength>" + std::to_string(size) +
             "</d:getcontentlength>";
        if (!content_type.empty()) {
            s += "<d:getcontenttype>" + content_type + "</d:getcontenttype>";
        }
    }
    if (!modified.empty()) {
        s += "<d:getlastmodified>" + modified + "</d:getlastmodified>";
    }
    if (!etag.empty()) {
        s += "<d:getetag>&quot;" + etag + "&quot;</d:getetag>";
    }
    if (!displayname.empty()) {
        s += "<d:displayname>" + displayname + "</d:displayname>";
    }
    s += "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
         "</d:response>";
    return s;
}

/// 根列表（自条目 /dav/ + docs/ 目录 + 两个文件，href 按真服务器形态编码）
const std::string kRootListing =
    davEntry("/dav/", true) +
    davEntry("/dav/docs/", true, 0, "Mon, 02 Feb 2026 03:04:05 GMT") +
    davEntry("/dav/docs/read%20me.txt", false, 100,
             "Mon, 02 Feb 2026 03:04:05 GMT", "abc123", "text/plain") +
    davEntry("/dav/docs/img.png", false, 200, "", "", "image/png");

} // namespace

//==============================================================================
// URL 解析（纯逻辑）
//==============================================================================

TEST(WebDavUrlParserTest, MapsDavSchemesToHttp) {
    auto url = WebDavUrlParser::parse("webdav://host:5244/dav");
    EXPECT_EQ(url.scheme, "http");
    EXPECT_EQ(url.host, "host");
    EXPECT_EQ(url.port, "5244");
    EXPECT_EQ(url.base_path, "/dav");

    auto dav = WebDavUrlParser::parse("dav://host");
    EXPECT_EQ(dav.scheme, "http");
    EXPECT_EQ(dav.port, "");
    EXPECT_EQ(dav.base_path, "/");
}

TEST(WebDavUrlParserTest, MapsSecureSchemesToHttps) {
    auto url = WebDavUrlParser::parse("davs://host/path");
    EXPECT_EQ(url.scheme, "https");
    auto webdavs = WebDavUrlParser::parse("webdavs://host:8443");
    EXPECT_EQ(webdavs.scheme, "https");
    EXPECT_EQ(webdavs.port, "8443");
}

TEST(WebDavUrlParserTest, ExtractsInlineCredentialsWithPercentDecode) {
    auto url = WebDavUrlParser::parse("webdav://user%40x:p%23ss@host:123/d");
    EXPECT_EQ(url.username, "user@x");
    EXPECT_EQ(url.password, "p#ss");
    EXPECT_EQ(url.host, "host");
}

TEST(WebDavUrlParserTest, PathAtSignInPathIsNotCredentials) {
    auto url = WebDavUrlParser::parse("webdav://host/dir@2x/file");
    EXPECT_TRUE(url.username.empty());
    EXPECT_EQ(url.base_path, "/dir@2x/file");
}

TEST(WebDavUrlParserTest, Ipv6LiteralAuthority) {
    auto url = WebDavUrlParser::parse("dav://[::1]:5244/dav");
    EXPECT_EQ(url.host, "[::1]");
    EXPECT_EQ(url.port, "5244");
}

TEST(WebDavUrlParserTest, UnknownSchemeYieldsEmpty) {
    auto url = WebDavUrlParser::parse("https://example.com/dav");
    EXPECT_TRUE(url.scheme.empty());
    EXPECT_TRUE(WebDavUrlParser::parse("no-scheme").scheme.empty());
}

//==============================================================================
// 基础属性与工厂
//==============================================================================

TEST(WebDavBrowserBasicTest, NameProtocolsAndCanHandle) {
    WebDavBrowser browser;
    EXPECT_EQ(browser.get_name(), "WebDAV");
    auto protocols = browser.get_supported_protocols();
    ASSERT_GE(protocols.size(), size_t{4});
    EXPECT_EQ(protocols[0], "webdav");

    EXPECT_TRUE(browser.can_handle("webdav://host/dav"));
    EXPECT_TRUE(browser.can_handle("dav://host"));
    EXPECT_TRUE(browser.can_handle("davs://host:443"));
    EXPECT_TRUE(browser.can_handle("webdavs://host"));
    EXPECT_FALSE(browser.can_handle("https://example.com/dav"));
    EXPECT_FALSE(browser.can_handle(""));
    EXPECT_TRUE(browser.get_root_path() == "/");
    browser.disconnect();  // 无状态协议，无副作用
}

TEST(WebDavBrowserFactoryTest, RegistersWebDavAndAliases) {
    EXPECT_TRUE(BrowserFactory::create_browser("webdav") != nullptr);
    EXPECT_TRUE(BrowserFactory::create_browser("dav") != nullptr);
    EXPECT_TRUE(BrowserFactory::create_browser("davs") != nullptr);
    EXPECT_TRUE(BrowserFactory::create_browser("webdavs") != nullptr);

    auto browser = BrowserFactory::create_from_url("dav://host/dav");
    ASSERT_TRUE(browser != nullptr);
    EXPECT_EQ(browser->get_name(), "WebDAV");

    bool found = false;
    for (const auto& info : BrowserFactory::available_browsers()) {
        if (info.protocol == "webdav") {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

//==============================================================================
// mock 请求路径
//==============================================================================

class WebDavBrowserMockTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 默认：PROPFIND 一律回根列表（自条目剔除后为 3 条目）
        server_ = std::make_unique<MockDavServer>(
            [](const std::string& method, const std::string& path) {
                if (method == "PROPFIND") {
                    return MockDavServer::Response{207, multistatusXml(kRootListing),
                                                   {{"Content-Type",
                                                     "application/xml"}}};
                }
                return MockDavServer::Response{204, ""};
            });
        ASSERT_TRUE(server_->start());
    }

    /// webdav://127.0.0.1:<port>/dav + Basic 凭据
    bool connectBrowser(WebDavBrowser& browser,
                        std::map<std::string, std::string> options = {}) {
        if (options.find("username") == options.end()) {
            options["username"] = "user";
            options["password"] = "pass";
        }
        // base_url 为 http://127.0.0.1:port —— 换 webdav scheme + /dav 前缀
        return browser.connect("webdav" + server_->base_url().substr(4) + "/dav",
                               options);
    }

    std::unique_ptr<MockDavServer> server_;
};

TEST_F(WebDavBrowserMockTest, ConnectProbesWithPropfindDepth0) {
    std::atomic<bool> saw_depth0{false};
    server_ = std::make_unique<MockDavServer>(
        [&](const std::string& method, const std::string&,
            const std::map<std::string, std::string>& headers) {
            if (method == "PROPFIND") {
                if (headers.at("depth") == "0") {
                    saw_depth0 = true;
                }
                return MockDavServer::Response{207, multistatusXml(""), {}};
            }
            return MockDavServer::Response{204, ""};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    EXPECT_TRUE(connectBrowser(browser));
    EXPECT_TRUE(saw_depth0.load());

    auto reqs = server_->requests();
    ASSERT_GE(reqs.size(), size_t{1});
    EXPECT_EQ(reqs[0].first, "PROPFIND");
    EXPECT_EQ(reqs[0].second, "/dav");
}

TEST_F(WebDavBrowserMockTest, ConnectFailsOnUnreachableServer) {
    WebDavBrowser browser;
    std::map<std::string, std::string> options;
    options["username"] = "u";
    options["password"] = "p";
    EXPECT_FALSE(browser.connect("webdav://127.0.0.1:1/dav", options));
}

TEST_F(WebDavBrowserMockTest, ConnectRejectsInvalidUrl) {
    WebDavBrowser browser;
    EXPECT_FALSE(browser.connect("https://example.com/dav", {}));
    EXPECT_FALSE(browser.connect("webdav://", {}));
    EXPECT_FALSE(browser.connect("not a url", {}));
}

TEST_F(WebDavBrowserMockTest, ConnectNegotiatesBasicAuthAfter401Challenge) {
    std::atomic<bool> saw_auth{false};
    server_ = std::make_unique<MockDavServer>(
        [&](const std::string& method, const std::string&,
            const std::map<std::string, std::string>& headers) {
            const auto auth = headers.find("authorization");
            if (auth == headers.end()) {
                return MockDavServer::Response{
                    401, "unauthorized",
                    {{"WWW-Authenticate", "Basic realm=\"dav\""}}};
            }
            if (auth->second == "Basic dXNlcjpwYXNz") {  // user:pass
                saw_auth = true;
            }
            return MockDavServer::Response{207, multistatusXml(""), {}};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    EXPECT_TRUE(connectBrowser(browser));
    EXPECT_TRUE(saw_auth.load());
}

TEST_F(WebDavBrowserMockTest, ListDirectoryParsesMultistatus) {
    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    auto resources = browser.list_directory("", options);

    // 自条目 /dav/ 剔除；docs/ 为目录；两个文件；默认按名排序
    ASSERT_EQ(resources.size(), size_t{3});
    EXPECT_EQ(resources[0].name, "docs");
    EXPECT_EQ(resources[0].path, "docs");
    EXPECT_EQ(resources[0].type, ResourceType::Directory);
    EXPECT_EQ(resources[0].modified_time, "Mon, 02 Feb 2026 03:04:05 GMT");
    EXPECT_EQ(resources[1].name, "img.png");
    EXPECT_EQ(resources[1].size, uint64_t{200});
    EXPECT_EQ(resources[1].mime_type, "image/png");
    EXPECT_EQ(resources[2].name, "read me.txt");  // href 百分号解码
    EXPECT_EQ(resources[2].path, "docs/read me.txt");
    EXPECT_EQ(resources[2].size, uint64_t{100});
    EXPECT_EQ(resources[2].type, ResourceType::File);
    // 实体解码（getetag 包裹 &quot;）；引号原样保留
    EXPECT_EQ(resources[2].etag, "\"abc123\"");

    // PROPFIND 打到 base_path + 路径，携带 Depth: 1
    auto reqs = server_->requests();
    ASSERT_GE(reqs.size(), size_t{2});
    EXPECT_EQ(reqs.back().first, "PROPFIND");
    EXPECT_EQ(reqs.back().second, "/dav");
}

TEST_F(WebDavBrowserMockTest, ListDirectoryToleratesUppercaseNamespace) {
    server_ = std::make_unique<MockDavServer>(
        [](const std::string& method, const std::string&) {
            if (method != "PROPFIND") {
                return MockDavServer::Response{204, ""};
            }
            const std::string xml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<D:multistatus xmlns:D=\"DAV:\">"
                "<D:response><D:href>/dav/a.txt</D:href><D:propstat>"
                "<D:prop><D:resourcetype/><D:getcontentlength>7"
                "</D:getcontentlength></D:prop>"
                "<D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
                "</D:response></D:multistatus>";
            return MockDavServer::Response{207, xml, {}};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));
    auto resources = browser.list_directory("", {});
    ASSERT_EQ(resources.size(), size_t{1});
    EXPECT_EQ(resources[0].name, "a.txt");
    EXPECT_EQ(resources[0].size, uint64_t{7});
}

TEST_F(WebDavBrowserMockTest, ListDirectoryHandlesAbsoluteHrefs) {
    server_ = std::make_unique<MockDavServer>(
        [&](const std::string& method, const std::string&) {
            if (method != "PROPFIND") {
                return MockDavServer::Response{204, ""};
            }
            const std::string xml = multistatusXml(
                "<d:response><d:href>" + server_->base_url() +
                "/dav/a.txt</d:href><d:propstat><d:prop>"
                "<d:resourcetype/><d:getcontentlength>3</d:getcontentlength>"
                "</d:prop></d:propstat></d:response>");
            return MockDavServer::Response{207, xml, {}};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));
    auto resources = browser.list_directory("", {});
    // 绝对 URL href：剥 scheme://host 后按同一 base_path 规则归位
    ASSERT_EQ(resources.size(), size_t{1});
    EXPECT_EQ(resources[0].path, "a.txt");
    EXPECT_EQ(resources[0].size, uint64_t{3});
}

TEST_F(WebDavBrowserMockTest, ListDirectoryFiltersHiddenAndSorts) {
    server_ = std::make_unique<MockDavServer>(
        [](const std::string& method, const std::string&) {
            if (method != "PROPFIND") {
                return MockDavServer::Response{204, ""};
            }
            return MockDavServer::Response{
                207,
                multistatusXml(davEntry("/dav/b.txt", false, 300) +
                               davEntry("/dav/.hidden", false, 1) +
                               davEntry("/dav/a.txt", false, 100)),
                {}};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));

    // 默认隐藏文件剔除、按名排序
    auto resources = browser.list_directory("", {});
    ASSERT_EQ(resources.size(), size_t{2});
    EXPECT_EQ(resources[0].name, "a.txt");
    EXPECT_EQ(resources[1].name, "b.txt");

    // show_hidden 放开
    ListOptions show_hidden;
    show_hidden.show_hidden = true;
    resources = browser.list_directory("", show_hidden);
    ASSERT_EQ(resources.size(), size_t{3});
    EXPECT_EQ(resources[0].name, ".hidden");

    // 通配符过滤
    ListOptions filtered;
    filtered.filter = "*.txt";
    resources = browser.list_directory("", filtered);
    ASSERT_EQ(resources.size(), size_t{2});

    // 按大小降序
    ListOptions by_size;
    by_size.sort_by = "size";
    by_size.sort_desc = true;
    resources = browser.list_directory("", by_size);
    ASSERT_EQ(resources.size(), size_t{2});
    EXPECT_EQ(resources[0].size, uint64_t{300});
}

TEST_F(WebDavBrowserMockTest, ListDirectoryRecursiveFlattensAndSkipsDirs) {
    server_ = std::make_unique<MockDavServer>(
        [](const std::string& method, const std::string& path) {
            if (method != "PROPFIND") {
                return MockDavServer::Response{204, ""};
            }
            if (path == "/dav") {
                return MockDavServer::Response{
                    207,
                    multistatusXml(davEntry("/dav/", true) +
                                   davEntry("/dav/docs/", true) +
                                   davEntry("/dav/root.txt", false, 1)),
                    {}};
            }
            if (path == "/dav/docs") {
                return MockDavServer::Response{
                    207,
                    multistatusXml(davEntry("/dav/docs", true) +
                                   davEntry("/dav/docs/sub/", true) +
                                   davEntry("/dav/docs/a.txt", false, 2)),
                    {}};
            }
            if (path == "/dav/docs/sub") {
                return MockDavServer::Response{
                    207,
                    multistatusXml(davEntry("/dav/docs/sub/", true) +
                                   davEntry("/dav/docs/sub/b.txt", false, 3)),
                    {}};
            }
            return MockDavServer::Response{404, ""};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));

    ListOptions options;
    options.recursive = true;
    auto resources = browser.list_directory("", options);

    // 递归模式：目录条目不出现，只收集文件
    ASSERT_EQ(resources.size(), size_t{3});
    EXPECT_EQ(resources[0].name, "a.txt");
    EXPECT_EQ(resources[0].path, "docs/a.txt");
    EXPECT_EQ(resources[1].name, "b.txt");
    EXPECT_EQ(resources[1].path, "docs/sub/b.txt");
    EXPECT_EQ(resources[2].name, "root.txt");

    // connect 探活 1 次 + 三层各一次 PROPFIND（Depth: 1 下钻）
    int propfinds = 0;
    for (const auto& [method, path] : server_->requests()) {
        if (method == "PROPFIND") {
            ++propfinds;
        }
    }
    EXPECT_EQ(propfinds, 4);
}

TEST_F(WebDavBrowserMockTest, GetResourceInfoAndExists) {
    // Depth:0 探测：命中的文件回单条目，其余路径回空 multistatus
    server_ = std::make_unique<MockDavServer>(
        [](const std::string& method, const std::string& path) {
            if (method == "PROPFIND") {
                if (path == "/dav/docs/read%20me.txt") {
                    return MockDavServer::Response{
                        207,
                        multistatusXml(davEntry("/dav/docs/read%20me.txt",
                                                false, 100,
                                                "Mon, 02 Feb 2026 03:04:05 GMT",
                                                "abc123", "text/plain")),
                        {}};
                }
                return MockDavServer::Response{207, multistatusXml(""), {}};
            }
            return MockDavServer::Response{204, ""};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));

    auto info = browser.get_resource_info("docs/read me.txt");
    EXPECT_EQ(info.path, "docs/read me.txt");
    EXPECT_EQ(info.name, "read me.txt");
    EXPECT_EQ(info.type, ResourceType::File);
    EXPECT_EQ(info.size, uint64_t{100});
    // getetag 引号原样保留（真服务器形态，不做剥离）
    EXPECT_EQ(info.etag, "\"abc123\"");

    EXPECT_TRUE(browser.exists("docs/read me.txt"));
    // 空 multistatus → 空资源 → exists false
    EXPECT_FALSE(browser.exists("missing.txt"));
}

TEST_F(WebDavBrowserMockTest, CreateDirectoryMkcolAndRecursiveAncestors) {
    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.create_directory("newdir"));
    auto reqs = server_->requests();
    ASSERT_GE(reqs.size(), size_t{2});
    EXPECT_EQ(reqs.back().first, "MKCOL");
    EXPECT_EQ(reqs.back().second, "/dav/newdir");

    // recursive：逐级 MKCOL（已存在的中间级 405 放行）
    server_ = std::make_unique<MockDavServer>(
        [](const std::string& method, const std::string& path) {
            if (method == "PROPFIND") {
                return MockDavServer::Response{207, multistatusXml(""), {}};
            }
            if (method == "MKCOL" && path == "/dav/a/b/c") {
                return MockDavServer::Response{201, ""};
            }
            if (method == "MKCOL") {
                return MockDavServer::Response{405, ""};  // 已存在
            }
            return MockDavServer::Response{204, ""};
        });
    ASSERT_TRUE(server_->start());
    WebDavBrowser deep;
    ASSERT_TRUE(connectBrowser(deep));
    EXPECT_TRUE(deep.create_directory("a/b/c", true));

    int mkcols = 0;
    for (const auto& [method, path] : server_->requests()) {
        if (method == "MKCOL") {
            ++mkcols;
        }
    }
    EXPECT_EQ(mkcols, 3);  // /dav/a /dav/a/b /dav/a/b/c

    // 根目录不可创建
    EXPECT_FALSE(browser.create_directory("/"));
}

TEST_F(WebDavBrowserMockTest, RemoveSendsDeleteWithDepthInfinity) {
    std::atomic<bool> saw_infinity{false};
    server_ = std::make_unique<MockDavServer>(
        [&](const std::string& method, const std::string&,
            const std::map<std::string, std::string>& headers) {
            if (method == "PROPFIND") {
                return MockDavServer::Response{207, multistatusXml(""), {}};
            }
            if (method == "DELETE") {
                const auto depth = headers.find("depth");
                if (depth != headers.end() && depth->second == "infinity") {
                    saw_infinity = true;
                }
                return MockDavServer::Response{204, ""};
            }
            return MockDavServer::Response{204, ""};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.remove("docs", true));
    EXPECT_TRUE(saw_infinity.load());

    EXPECT_TRUE(browser.remove("a.txt"));
    auto reqs = server_->requests();
    EXPECT_EQ(reqs.back().first, "DELETE");
    EXPECT_EQ(reqs.back().second, "/dav/a.txt");
}

TEST_F(WebDavBrowserMockTest, RemoveRecursiveFallsBackToPerItemDeletes) {
    // Depth: infinity 被服务器拒绝（403）：兜底先删文件再删目录
    server_ = std::make_unique<MockDavServer>(
        [](const std::string& method, const std::string& path,
           const std::map<std::string, std::string>& headers) {
            if (method == "PROPFIND") {
                if (path == "/dav/docs") {
                    return MockDavServer::Response{
                        207,
                        multistatusXml(davEntry("/dav/docs", true) +
                                       davEntry("/dav/docs/a.txt", false, 1)),
                        {}};
                }
                return MockDavServer::Response{207, multistatusXml(""), {}};
            }
            if (method == "DELETE") {
                const auto depth = headers.find("depth");
                if (depth != headers.end() && depth->second == "infinity") {
                    return MockDavServer::Response{403, ""};
                }
                return MockDavServer::Response{204, ""};
            }
            return MockDavServer::Response{204, ""};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));
    EXPECT_TRUE(browser.remove("docs", true));

    // 首删（Depth: infinity）403 → 兜底：子项先删（无 Depth 头），目录后删
    std::vector<std::string> deleted;
    for (const auto& [method, path] : server_->requests()) {
        if (method == "DELETE") {
            deleted.push_back(path);
        }
    }
    ASSERT_EQ(deleted.size(), size_t{3});
    EXPECT_EQ(deleted[0], "/dav/docs");
    EXPECT_EQ(deleted[1], "/dav/docs/a.txt");
    EXPECT_EQ(deleted[2], "/dav/docs");
}

TEST_F(WebDavBrowserMockTest, RenameAndCopyUseMoveCopyWithDestination) {
    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));

    EXPECT_TRUE(browser.rename("a.txt", "b.txt"));
    auto reqs = server_->requests();
    EXPECT_EQ(reqs.back().first, "MOVE");
    EXPECT_EQ(reqs.back().second, "/dav/a.txt");

    EXPECT_TRUE(browser.copy("a.txt", "sub/b.txt"));
    reqs = server_->requests();
    EXPECT_EQ(reqs.back().first, "COPY");
    EXPECT_EQ(reqs.back().second, "/dav/a.txt");
}

TEST_F(WebDavBrowserMockTest, MoveDestinationAndOverwriteHeadersVerified) {
    std::atomic<bool> ok_move{false};
    server_ = std::make_unique<MockDavServer>(
        [&](const std::string& method, const std::string& path,
            const std::map<std::string, std::string>& headers) {
            if (method == "PROPFIND") {
                return MockDavServer::Response{207, multistatusXml(""), {}};
            }
            if (method == "MOVE") {
                const auto dest = headers.find("destination");
                const auto overwrite = headers.find("overwrite");
                ok_move = dest != headers.end() &&
                          overwrite != headers.end() &&
                          dest->second == server_->base_url() + "/dav/b.txt" &&
                          overwrite->second == "T";
            }
            return MockDavServer::Response{201, ""};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));
    EXPECT_TRUE(browser.rename("a.txt", "b.txt"));
    EXPECT_TRUE(ok_move.load());
}

TEST_F(WebDavBrowserMockTest, QuotaInfoParsesRfc4331OrStaysEmpty) {
    server_ = std::make_unique<MockDavServer>(
        [](const std::string& method, const std::string&) {
            if (method == "PROPFIND") {
                return MockDavServer::Response{
                    207,
                    multistatusXml(
                        "<d:response><d:href>/dav/</d:href><d:propstat>"
                        "<d:prop><d:quota-used-bytes>1234</d:quota-used-bytes>"
                        "<d:quota-available-bytes>5678"
                        "</d:quota-available-bytes></d:prop>"
                        "</d:propstat></d:response>"),
                    {}};
            }
            return MockDavServer::Response{204, ""};
        });
    ASSERT_TRUE(server_->start());

    WebDavBrowser browser;
    ASSERT_TRUE(connectBrowser(browser));
    auto quota = browser.get_quota_info();
    EXPECT_EQ(quota.at("used"), uint64_t{1234});
    EXPECT_EQ(quota.at("available"), uint64_t{5678});
    EXPECT_EQ(quota.at("total"), uint64_t{6912});

    // 不支持 RFC 4331 的服务器：字段缺失 → 空 map
    server_ = std::make_unique<MockDavServer>(
        [](const std::string& method, const std::string&) {
            if (method == "PROPFIND") {
                return MockDavServer::Response{207, multistatusXml(""), {}};
            }
            return MockDavServer::Response{204, ""};
        });
    ASSERT_TRUE(server_->start());
    WebDavBrowser no_quota;
    ASSERT_TRUE(connectBrowser(no_quota));
    EXPECT_TRUE(no_quota.get_quota_info().empty());
}

TEST_F(WebDavBrowserMockTest, DirectoryHelpersAreInMemory) {
    WebDavBrowser browser;
    EXPECT_TRUE(browser.get_current_directory().empty());
    EXPECT_TRUE(browser.change_directory("docs"));
    EXPECT_EQ(browser.get_current_directory(), "/docs");
    EXPECT_TRUE(browser.change_directory("/"));
    EXPECT_EQ(browser.get_current_directory(), "/");
}
