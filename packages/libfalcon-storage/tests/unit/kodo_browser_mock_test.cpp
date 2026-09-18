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

/// 批次 V：无自定义 endpoint——build_api_url 落到官方域名分支
/// （rs.qbox.me）拼 URL 后发真实探测请求；无签名的 stat 必被拒绝
/// （401/612）或网络不可达，connect 以失败收口。URL 拼接先于请求，
/// 官方域名分支的行覆盖与请求结果无关
TEST(KodoBrowserMockTest, ConnectWithoutEndpointUsesOfficialDomainAndFails) {
    KodoBrowser browser;
    // 只给凭据不给 endpoint：请求发往 http://rs.qbox.me（真实网络，
    // 只读探测，无副作用）
    const std::map<std::string, std::string> options = {
        {"access_key", "ak"}, {"secret_key", "sk"}};
    EXPECT_FALSE(
        browser.connect("kodo://" + std::string(kBucket), options));
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

TEST(KodoBrowserMockTest, GetResourceInfoToleratesMalformedStatJson) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "GET" && path.rfind("/stat/", 0) == 0) {
            // 200 + 非法 JSON：解析异常必须被吞掉，信息字段保持默认
            return MockServer::Response{200, "<<not-json>>"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    auto info = browser.get_resource_info("docs/readme.md");
    EXPECT_EQ(info.name, "readme.md");
    EXPECT_EQ(info.size, 0u);
    EXPECT_TRUE(info.etag.empty());
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

//==============================================================================
// URL 解析 / 基本属性
//==============================================================================

TEST(KodoBrowserMockTest, ListDirectorySortsByNameAndSize) {
    // 故意乱序：a/c 名序与 b/a 尺寸序都与返回序相反，升/降四个比较器
    // 分支都靠断言顺序收口
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "POST" && path == "/list") {
            return MockServer::Response{200,
                R"({"items":[)"
                R"({"key":"docs/c.txt","fsize":300},)"
                R"({"key":"docs/a.txt","fsize":200},)"
                R"({"key":"docs/b.txt","fsize":100}]})"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    // 默认 sort_by="name" 升序
    auto resources = browser.list_directory("docs/", ListOptions{});
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "a.txt");
    EXPECT_EQ(resources[1].name, "b.txt");
    EXPECT_EQ(resources[2].name, "c.txt");

    // name 降序
    ListOptions name_desc;
    name_desc.sort_desc = true;
    resources = browser.list_directory("docs/", name_desc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "c.txt");
    EXPECT_EQ(resources[2].name, "a.txt");

    // size 升序
    ListOptions size_asc;
    size_asc.sort_by = "size";
    resources = browser.list_directory("docs/", size_asc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].size, 100u);
    EXPECT_EQ(resources[2].size, 300u);

    // size 降序
    ListOptions size_desc;
    size_desc.sort_by = "size";
    size_desc.sort_desc = true;
    resources = browser.list_directory("docs/", size_desc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].size, 300u);
    EXPECT_EQ(resources[2].size, 100u);
}

TEST(KodoBrowserMockTest, UrlParserRejectsNonKodoProtocol) {
    // 非 kodo/qiniu 协议必须抛异常（此前缺失协议是未定义行为路径）
    EXPECT_THROW(KodoUrlParser::parse("https://bucket/key"), std::invalid_argument);
    EXPECT_THROW(KodoUrlParser::parse("ftp://bucket"), std::invalid_argument);
    EXPECT_THROW(KodoUrlParser::parse(""), std::invalid_argument);
}

TEST(KodoBrowserMockTest, UrlParserSplitsBucketAndKeyForBothProtocols) {
    auto kodo = KodoUrlParser::parse("kodo://mybucket/path/to/file.txt");
    EXPECT_EQ(kodo.bucket, "mybucket");
    EXPECT_EQ(kodo.key, "path/to/file.txt");

    auto qiniu = KodoUrlParser::parse("qiniu://otherbucket");
    EXPECT_EQ(qiniu.bucket, "otherbucket");
    EXPECT_TRUE(qiniu.key.empty());

    // 只有协议前缀没有 bucket 同样抛异常
    EXPECT_THROW(KodoUrlParser::parse("kodo://"), std::invalid_argument);
}

TEST(KodoBrowserMockTest, UrlParserAcceptsQnProtocol) {
    // can_handle/get_supported_protocols 承诺 qn://，parse 拒绝会把
    // std::invalid_argument 直接抛出 connect() 公共 API
    auto qn = KodoUrlParser::parse("qn://qnbucket/dir/file.txt");
    EXPECT_EQ(qn.bucket, "qnbucket");
    EXPECT_EQ(qn.key, "dir/file.txt");

    // 通过 connect() 公共 API 走通（此前 qn:// 在此抛出）
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    EXPECT_TRUE(browser.connect("qn://" + std::string(kBucket),
                                connect_options(server->base_url())));
}

TEST(KodoBrowserMockTest, BasicPropertiesAndProtocolHandling) {
    KodoBrowser browser;
    EXPECT_EQ(browser.get_name(), "七牛云Kodo");
    EXPECT_EQ(browser.get_supported_protocols(),
              (std::vector<std::string>{"kodo", "qiniu", "qn"}));

    EXPECT_TRUE(browser.can_handle("kodo://bucket/key"));
    EXPECT_TRUE(browser.can_handle("qiniu://bucket/key"));
    EXPECT_TRUE(browser.can_handle("qn://bucket/key"));
    EXPECT_FALSE(browser.can_handle("http://bucket/key"));
    EXPECT_FALSE(browser.can_handle("s3://bucket/key"));
}

//==============================================================================
// endpoint 边缘与连接选项
//==============================================================================

TEST(KodoBrowserMockTest, ConnectStripsEndpointTrailingSlash) {
    // endpoint 带尾斜杠：剥离后不得产生 "//stat" 双斜杠路径
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    EXPECT_TRUE(connect_kodo(browser, server->base_url() + "/"));

    const auto reqs = server->requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].second.rfind("/stat/", 0), 0);
}

TEST(KodoBrowserMockTest, ListWorksWithTrailingSlashEndpoint) {
    // rsf 列举端点同样剥离尾斜杠（此前 endpoint 带 '/' 时列举必然 404）
    auto server = make_server(listAwareReply);
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url() + "/"));

    auto resources = browser.list_directory("docs/", ListOptions{});
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_TRUE(has_request(server->requests(), "POST", "/list"));
}

TEST(KodoBrowserMockTest, ConnectHonorsDomainAndHttpsOptions) {
    // domain / https 选项被消费（endpoint 优先生效，connect 仍走 mock）
    auto server = make_server(defaultReply);
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    auto options = connect_options(server->base_url());
    options.emplace("domain", "cdn.example.com");
    options.emplace("https", "1");
    EXPECT_TRUE(browser.connect("kodo://" + std::string(kBucket), options));
}

//==============================================================================
// 排序 / 过滤
//==============================================================================

TEST(KodoBrowserMockTest, ListSortsByNameDescAndBySize) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "POST" && path == "/list") {
            return MockServer::Response{200,
                R"({"items":[)"
                R"({"key":"c.bin","fsize":300},)"
                R"({"key":"a.bin","fsize":100},)"
                R"({"key":"b.bin","fsize":200}]})"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    ListOptions name_desc;
    name_desc.sort_by = "name";
    name_desc.sort_desc = true;
    auto resources = browser.list_directory("/", name_desc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "c.bin");
    EXPECT_EQ(resources[2].name, "a.bin");

    ListOptions size_asc;
    size_asc.sort_by = "size";
    resources = browser.list_directory("/", size_asc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "a.bin");
    EXPECT_EQ(resources[2].name, "c.bin");

    ListOptions size_desc;
    size_desc.sort_by = "size";
    size_desc.sort_desc = true;
    resources = browser.list_directory("/", size_desc);
    ASSERT_EQ(resources.size(), 3u);
    EXPECT_EQ(resources[0].name, "c.bin");
    EXPECT_EQ(resources[2].name, "a.bin");
}

TEST(KodoBrowserMockTest, ListFilterWildcardPrefixSuffixExactAndStar) {
    auto server = make_server([](const std::string& method, const std::string& path) {
        if (method == "POST" && path == "/list") {
            return MockServer::Response{200,
                R"({"items":[)"
                R"({"key":"a.txt","fsize":1},)"
                R"({"key":"pre_x.txt","fsize":1},)"
                R"({"key":"exact.log","fsize":1},)"
                R"({"key":"other.bin","fsize":1}]})"};
        }
        return MockServer::Response{200, "{}"};
    });
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

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

TEST(KodoBrowserMockTest, ListWithMetadataOptionSucceeds) {
    // include_metadata 走更高的 limit 分支，列举结果不受影响
    auto server = make_server(listAwareReply);
    ASSERT_NE(server, nullptr);

    KodoBrowser browser;
    ASSERT_TRUE(connect_kodo(browser, server->base_url()));

    ListOptions options;
    options.include_metadata = true;
    auto resources = browser.list_directory("docs/", options);
    ASSERT_EQ(resources.size(), 2u);
    EXPECT_TRUE(has_request(server->requests(), "POST", "/list"));
}

//==============================================================================
// 目录辅助 / 断开
//==============================================================================

TEST(KodoBrowserMockTest, DirectoryHelpersAreInMemory) {
    KodoBrowser browser;
    EXPECT_TRUE(browser.get_current_directory().empty());
    EXPECT_TRUE(browser.change_directory("docs"));
    EXPECT_EQ(browser.get_current_directory(), "docs");
    EXPECT_EQ(browser.get_root_path(), "/");
    browser.disconnect();  // Kodo 无状态，无副作用
}

//==============================================================================
// endpoint 带 https:// scheme 的 path-style 前缀分支（92 行）。指向
// 无人监听的端口连接立即被拒，URL 构造分支照样执行
//==============================================================================

TEST(KodoBrowserMockTest, ConnectHttpsEndpointPrefixStillPathStyle) {
    KodoBrowser browser;
    std::map<std::string, std::string> options = {
        {"access_key", "ak"}, {"secret_key", "sk"},
        {"endpoint", "https://127.0.0.1:1"}};
    EXPECT_FALSE(browser.connect("kodo://" + std::string(kBucket), options));
}

/// 批次 W：无 endpoint 时 list_directory 的 rsf 官方域名分支——
/// build_rsf_url("list") 落到 rsf.qbox.me（connect 已失败不阻断列举，
/// URL 拼接先于请求）；真实 rsf 域名未授权/不可达 → 空结果收口
TEST(KodoBrowserMockTest, ListDirectoryWithoutEndpointBuildsOfficialRsfUrlAndFails) {
    KodoBrowser browser;
    const std::map<std::string, std::string> options = {
        {"access_key", "ak"}, {"secret_key", "sk"}};
    EXPECT_FALSE(browser.connect("kodo://" + std::string(kBucket), options));
    EXPECT_TRUE(browser.list_directory("/", ListOptions{}).empty());
}
