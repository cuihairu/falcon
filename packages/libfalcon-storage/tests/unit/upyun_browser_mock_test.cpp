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

/// 批次 V：无自定义 api_domain——connect 的 options 处理落到默认值
/// 兜底（"v0.api.upyun.com"），build_url 随之走官方域名分支（https://
/// {bucket}.v0.api.upyun.com）。无签名的 /usage/ 探测被拒（401）或网
/// 络不可达，connect 以失败收口；一个用例同时覆盖兜底赋值与官方域名
/// 拼接两处路径（均先于请求发生）
TEST(UpyunBrowserMockTest, ConnectWithoutApiDomainFallsBackToDefaultAndFails) {
    UpyunBrowser browser;
    // 空 options（无凭据无 api_domain）：默认域名兜底 + 真实网络只读
    // 探测，无副作用
    EXPECT_FALSE(browser.connect("upyun://" + std::string(kBucket), {}));
}

//==============================================================================
// 列举
//==============================================================================

/// path-style URL 拼接：path 不带前导斜杠时自动补 '/'（build_upyun_url
/// 的分隔符分支，官方域名分支的对应行因 connect 探测必然失败不可达）
TEST(UpyunBrowserMockTest, ListDirectoryPathWithoutLeadingSlashGetsSeparator) {
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    // mock 一律 200 空 body：拼接后请求发出即达目标，应答解析为空列表
    auto resources = browser.list_directory("docs", ListOptions{});
    EXPECT_TRUE(resources.empty());
}

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

TEST(UpyunBrowserMockTest, ListDirectoryTreatsNonNumericSizeAsDirectory) {
    // 又拍真实语义：目录行的 size 列可为 "N"——stoull 抛异常须落入
    // 「视为目录」分支而非向上传播
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET" && path == "/docs/") {
            return MockServer::Response{200, "sub\tN\tN\t1700000001\n"};
        }
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    auto resources = browser.list_directory("docs/", ListOptions{});
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].name, "sub");
    EXPECT_TRUE(resources[0].is_directory());
    EXPECT_EQ(resources[0].size, 0u);
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

//==============================================================================
// 连接（补充：URL 解析与 api_domain 分支）
//==============================================================================

TEST(UpyunBrowserMockTest, ConnectRejectsNonUpyunUrl) {
    UpyunBrowser browser;
    // 非 upyun:// 前缀在解析阶段直接抛出，不发任何网络请求
    EXPECT_THROW(browser.connect("http://bucket", connect_options("http://127.0.0.1:1")),
                 std::invalid_argument);
    EXPECT_THROW(browser.connect("upyunxx://bucket", {}), std::invalid_argument);
}

TEST(UpyunBrowserMockTest, ConnectFailsWhenApiDomainUsesHttpsAgainstHttpMock) {
    // api_domain 带 https:// 前缀同样走 path-style。注意不能指向活着的
    // 明文 mock：TLS ClientHello 之后客户端等 ServerHello、mock 等请求
    // 行，双方互等直到 curl 级超时（数分钟）。改指无人监听的端口——
    // 连接立即被拒，https:// 前缀分支同样被覆盖
    UpyunBrowser browser;
    std::map<std::string, std::string> options = {
        {"username", "operator"}, {"password", "pass"},
        {"api_domain", "https://127.0.0.1:1"}};
    EXPECT_FALSE(browser.connect("upyun://" + std::string(kBucket), options));
}

TEST(UpyunBrowserMockTest, ConnectStripsApiDomainTrailingSlash) {
    // path-style api_domain 尾部 '/' 必须剥离，否则拼出 "//usage/" 双斜杠路径
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    EXPECT_TRUE(connect_upyun(browser, server->base_url() + "/"));
    EXPECT_TRUE(has_request(server->requests(), "GET", "/usage/"));
    EXPECT_FALSE(any_request_contains(server->requests(), "//"));
}

TEST(UpyunBrowserMockTest, ConnectFailsOnUnresolvableOfficialDomain) {
    // api_domain 无 scheme → 官方虚拟主机分支 https://bucket.api_domain；
    // 用保留 TLD .invalid 触发 DNS 必败，离线覆盖该分支且不碰真实网络
    UpyunBrowser browser;
    std::map<std::string, std::string> options = {
        {"username", "operator"}, {"password", "pass"},
        {"api_domain", "v0.api.upyun.invalid"}};
    EXPECT_FALSE(browser.connect("upyun://" + std::string(kBucket), options));
}

TEST(UpyunBrowserMockTest, ConnectAcceptsBucketAndDomainOptions) {
    // bucket 选项覆盖 URL 里的 bucket，domain 选项记录 CDN 加速域名；
    // 两者对 path-style 请求路径不可见（bucket 只进官方域名主机名与
    // copy-source 头），此处覆盖选项分支本身不破坏连接流程
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    std::map<std::string, std::string> options = {
        {"username", "operator"}, {"password", "pass"},
        {"api_domain", server->base_url()},
        {"bucket", "otherbucket"}, {"domain", "cdn.example.invalid"}};
    EXPECT_TRUE(browser.connect("upyun://" + std::string(kBucket), options));
    EXPECT_TRUE(has_request(server->requests(), "GET", "/usage/"));
}

//==============================================================================
// 列举（补充：通配符过滤 / 排序选项 / 递归边角）
//==============================================================================

TEST(UpyunBrowserMockTest, FilterWildcardVariants) {
    auto server = make_server([](const std::string&, const std::string&) {
        return MockServer::Response{
            200, "a.txt\t1\tF\t1700000000\nb.png\t2\tF\t1700000001\n"
                 "pre_x.txt\t3\tF\t1700000002\n"};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    auto names_of = [&browser](const std::string& filter) {
        ListOptions options;
        options.filter = filter;
        auto resources = browser.list_directory("docs/", options);
        std::vector<std::string> names;
        for (const auto& res : resources) names.push_back(res.name);
        return names;
    };

    // "*" 恒真；无 '*' 精确匹配；单侧/双侧 '*' 前后缀匹配
    auto all = names_of("*");
    EXPECT_EQ(all.size(), 3u);
    auto exact = names_of("a.txt");
    ASSERT_EQ(exact.size(), 1u);
    EXPECT_EQ(exact[0], "a.txt");
    auto txt = names_of("*.txt");
    ASSERT_EQ(txt.size(), 2u);
    auto pre = names_of("pre*.txt");
    ASSERT_EQ(pre.size(), 1u);
    EXPECT_EQ(pre[0], "pre_x.txt");
    EXPECT_TRUE(names_of("missing.txt").empty());
}

TEST(UpyunBrowserMockTest, ListSortOptionsCoverListOrderVariants) {
    // sort_by 映射为 x-list-order 请求头（size:/time: 前缀），由服务器侧
    // 排序；mock 不记录请求头，此处覆盖请求构造分支且结果可解析
    auto server = make_server(listAwareReply);
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    for (bool desc : {false, true}) {
        ListOptions by_size;
        by_size.sort_by = "size";
        by_size.sort_desc = desc;
        EXPECT_EQ(browser.list_directory("docs/", by_size).size(), 2u);

        ListOptions by_time;
        by_time.sort_by = "modified_time";
        by_time.sort_desc = desc;
        EXPECT_EQ(browser.list_directory("docs/", by_time).size(), 2u);
    }
    EXPECT_EQ(count_requests(server->requests(), "GET"), 5);  // 1 探测 + 4 列举
}

TEST(UpyunBrowserMockTest, ListRecursiveHandlesTrailingSlashEntryNames) {
    // 目录项 name 带尾 '/' 时，递归子路径先剥离尾斜杠再拼列举 URI，
    // 不产生 "//" 双斜杠路径
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET" && path == "/docs/") {
            return MockServer::Response{200, "dir/\t0\tN\t1700000001\n"};
        }
        if (method == "GET" && path == "/docs/dir/") {
            return MockServer::Response{200, "inner.txt\t5\tF\t1700000002\n"};
        }
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    ListOptions options;
    options.recursive = true;
    auto resources = browser.list_directory("docs/", options);
    ASSERT_EQ(resources.size(), 2u);

    bool inner_found = false;
    for (const auto& res : resources) {
        if (res.name == "inner.txt") {
            inner_found = true;
            EXPECT_EQ(res.path, "/docs/dir/inner.txt");
        }
    }
    EXPECT_TRUE(inner_found);
    EXPECT_TRUE(has_request(server->requests(), "GET", "/docs/dir/"));
    EXPECT_FALSE(any_request_contains(server->requests(), "//"));
}

//==============================================================================
// 元信息与目录辅助方法
//==============================================================================

TEST(UpyunBrowserMockTest, BasicAttributesAndProtocolSupport) {
    UpyunBrowser browser;
    EXPECT_EQ(browser.get_name(), "又拍云USS");

    auto protocols = browser.get_supported_protocols();
    ASSERT_EQ(protocols.size(), 2u);
    bool has_upyun = false, has_upaiyun = false;
    for (const auto& p : protocols) {
        if (p == "upyun") has_upyun = true;
        if (p == "upaiyun") has_upaiyun = true;
    }
    EXPECT_TRUE(has_upyun);
    EXPECT_TRUE(has_upaiyun);

    EXPECT_TRUE(browser.can_handle("upyun://bucket"));
    EXPECT_TRUE(browser.can_handle("upaiyun://bucket"));
    EXPECT_FALSE(browser.can_handle("http://example.com"));
    EXPECT_FALSE(browser.can_handle("s3://bucket"));
}

TEST(UpyunBrowserMockTest, DisconnectIsSafeBeforeAndAfterConnect) {
    UpyunBrowser browser;
    EXPECT_NO_FATAL_FAILURE(browser.disconnect());

    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));
    EXPECT_NO_FATAL_FAILURE(browser.disconnect());
}

TEST(UpyunBrowserMockTest, DirectoryHelpersAreInMemory) {
    UpyunBrowser browser;
    // 又拍云浏览器无工作目录状态：current_directory 纯内存读写，根恒 "/"
    EXPECT_TRUE(browser.get_current_directory().empty());
    EXPECT_TRUE(browser.change_directory("/docs"));
    EXPECT_EQ(browser.get_current_directory(), "/docs");
    EXPECT_EQ(browser.get_root_path(), "/");
}

//==============================================================================
// 客户端排序：sort_resources 此前是死代码，sort_by/sort_desc 从未生效
//（服务器可无视 x-list-order 请求头，客户端排序必须兜底）
//==============================================================================

TEST(UpyunBrowserMockTest, ListSortsClientSideBySizeAndName) {
    // 服务器按乱序返回（无视 x-list-order 的现实行为）
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET") {
            return MockServer::Response{200,
                "mid.txt\t100\tF\t1700000000\n"
                "zenith.txt\t300\tF\t1700000001\n"
                "alpha.txt\t200\tF\t1700000002\n"};
        }
        return MockServer::Response{200, ""};
    });
    ASSERT_NE(server, nullptr);

    UpyunBrowser browser;
    ASSERT_TRUE(connect_upyun(browser, server->base_url()));

    ListOptions by_size;
    by_size.sort_by = "size";
    auto resources = browser.list_directory("/", by_size);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "mid.txt");
    EXPECT_EQ(resources[2].name, "zenith.txt");

    ListOptions by_size_desc;
    by_size_desc.sort_by = "size";
    by_size_desc.sort_desc = true;
    resources = browser.list_directory("/", by_size_desc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "zenith.txt");

    ListOptions by_name_desc;
    by_name_desc.sort_by = "name";
    by_name_desc.sort_desc = true;
    resources = browser.list_directory("/", by_name_desc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "zenith.txt");
    EXPECT_EQ(resources[2].name, "alpha.txt");
}
