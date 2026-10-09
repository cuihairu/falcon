/**
 * @file swarm_source_provider_test.cpp
 * @brief P2SP 查询源 provider 单测（§10.4 查询注入镜像池 + §10.5 NAT 过滤
 *        + §11 查询失败退避）。
 *
 * swarm_source_provider.cpp 直接编进测试目标（仅 nlohmann_json 头依赖）：
 * parse_sources / node_source_usable 为纯静态函数直接驱动；sources_for
 * 退避语义经 QueryFn fake 驱动（不触网）。
 */

#include "daemon/swarm_source_provider.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

using falcon::daemon::SwarmSourceProvider;
using json = nlohmann::json;

const std::string kShaHex(64, 'a');  // 小写 64 位 hex（合法形态）

// ---------------------------------------------------------------------------
// 测试基建
// ---------------------------------------------------------------------------

/// node 源条目（§16 线上形状：type/node_id + advertise{addr, direct}）
json node_source(const std::string& node_id, const std::string& addr,
                 bool direct) {
    return json{{"type", "node"},
                {"node_id", node_id},
                {"advertise", json{{"addr", addr}, {"direct", direct}}}};
}

json url_source(const std::string& url) {
    return json{{"type", "url"}, {"url", url}};
}

json sources_result(std::vector<json> items) {
    json arr = json::array();
    for (auto& item : items) arr.push_back(std::move(item));
    return json{{"sha256", kShaHex}, {"sources", std::move(arr)}};
}

/// 记录型查询 fake：调用留痕、返回码序列可预设（空队列 = 恒成功）、
/// 成功载荷可注入。
class FakeQuery {
public:
    SwarmSourceProvider::QueryFn fn() {
        return [this](const std::string& sha, json* out) -> int {
            std::lock_guard<std::mutex> lock(mu_);
            asked_.push_back(sha);
            int code = 0;
            if (!codes_.empty()) {
                code = codes_.front();
                codes_.pop_front();
            }
            if (code == 0 && out != nullptr) *out = payload_;
            return code;
        };
    }

    void set_payload(json payload) {
        std::lock_guard<std::mutex> lock(mu_);
        payload_ = std::move(payload);
    }

    void push_failure() {
        std::lock_guard<std::mutex> lock(mu_);
        codes_.push_back(1);
    }

    std::vector<std::string> asked() const {
        std::lock_guard<std::mutex> lock(mu_);
        return asked_;
    }

    std::size_t ask_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return asked_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<std::string> asked_;
    std::deque<int> codes_;
    json payload_ = json::object();
};

// ---------------------------------------------------------------------------
// parse_sources：node 源 URL 装配与过滤
// ---------------------------------------------------------------------------

TEST(ParseSourcesTest, NodeSourceBuildsDataUrl) {
    const auto result =
        sources_result({node_source("node-b", "10.0.0.9:7801", true)});
    const auto urls = SwarmSourceProvider::parse_sources(result, kShaHex, "");
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "http://10.0.0.9:7801/by-sha256/" + kShaHex);
}

TEST(ParseSourcesTest, NatNodeSourceSkipped) {
    // direct==false（NAT 后不可被动直连）不作数据源
    const auto result =
        sources_result({node_source("node-b", "10.0.0.9:7801", false)});
    EXPECT_TRUE(
        SwarmSourceProvider::parse_sources(result, kShaHex, "").empty());
}

TEST(ParseSourcesTest, MalformedNodeSourcesSkipped) {
    // 逐形态： advertise 缺席 / 非 object / addr 缺失 / addr 空 / addr 无
    // 端口 / direct 缺失 / direct 非布尔 / node_id 缺失 / node_id 非字符串。
    // 好条目同列——证明坏条目是跳过而非中止解析。
    std::vector<json> bad;
    bad.push_back(json{{"type", "node"},
                       {"node_id", "n1"}});  // advertise 缺席
    bad.push_back(json{{"type", "node"},
                       {"node_id", "n1"},
                       {"advertise", json::array()}});  // advertise 非 object
    bad.push_back(json{{"type", "node"},
                       {"node_id", "n1"},
                       {"advertise", json{{"direct", true}}}});  // addr 缺失
    bad.push_back(node_source("n1", "", true));                  // addr 空
    bad.push_back(node_source("n1", "10.0.0.9", true));          // 无 ':'
    bad.push_back(json{{"type", "node"},
                       {"node_id", "n1"},
                       {"advertise", json{{"addr", "10.0.0.9:7801"}}}});  // direct 缺失
    bad.push_back(json{{"type", "node"},
                       {"node_id", "n1"},
                       {"advertise",
                        json{{"addr", "10.0.0.9:7801"},
                             {"direct", "true"}}}});  // direct 非布尔
    bad.push_back(
        json{{"type", "node"},
             {"advertise", json{{"addr", "10.0.0.9:7801"}, {"direct", true}}}});  // node_id 缺失
    bad.push_back(json{{"type", "node"},
                       {"node_id", 42},
                       {"advertise",
                        json{{"addr", "10.0.0.9:7801"}, {"direct", true}}}});  // node_id 非字符串

    for (const auto& entry : bad) {
        const auto result = sources_result({entry, url_source("http://m/x")});
        const auto urls =
            SwarmSourceProvider::parse_sources(result, kShaHex, "");
        ASSERT_EQ(urls.size(), 1u) << entry.dump();
        EXPECT_EQ(urls[0], "http://m/x") << entry.dump();
    }
}

TEST(ParseSourcesTest, OwnNodeIdExcluded) {
    // 自身 node_id 跳过（不从自己拉数据）；他人同形态保留
    const auto result =
        sources_result({node_source("self-node", "127.0.0.1:9001", true),
                        node_source("other-node", "10.0.0.9:7801", true)});
    const auto urls = SwarmSourceProvider::parse_sources(result, kShaHex,
                                                         "self-node");
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "http://10.0.0.9:7801/by-sha256/" + kShaHex);
}

TEST(ParseSourcesTest, UrlSourceHttpHttpsOnly) {
    const auto result = sources_result({url_source("http://m1/f.bin"),
                                        url_source("https://m2/f.bin"),
                                        url_source("ftp://m3/f.bin"),
                                        url_source("//m4/f.bin"),  // 无 scheme
                                        url_source(""),            // 空串
                                        json{{"type", "url"}},     // url 缺失
                                        json{{"type", "url"}, {"url", 7}}});
    const auto urls = SwarmSourceProvider::parse_sources(result, kShaHex, "");
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_EQ(urls[0], "http://m1/f.bin");
    EXPECT_EQ(urls[1], "https://m2/f.bin");
}

TEST(ParseSourcesTest, UnknownTypeAndNonObjectEntriesSkipped) {
    const auto result = sources_result(
        {json{{"type", "torrent"}, {"url", "http://m/x"}},   // 未知类型
         json{{"url", "http://m/y"}},                        // type 缺失
         json{{"type", 1}, {"url", "http://m/z"}},           // type 非字符串
         json("bare-string"),                                // 非 object
         json(7)});                                          // 非对象数值
    EXPECT_TRUE(
        SwarmSourceProvider::parse_sources(result, kShaHex, "").empty());
}

TEST(ParseSourcesTest, DuplicateUrlsDeduplicated) {
    // 两条 node 源（同 addr 不同 node_id）+ 一条显式同 URL 的 url 源——
    // 产出同一 URL，去重后恰 1 条
    const auto result = sources_result(
        {node_source("n1", "10.0.0.9:7801", true),
         node_source("n2", "10.0.0.9:7801", true),
         url_source("http://10.0.0.9:7801/by-sha256/" + kShaHex)});
    const auto urls = SwarmSourceProvider::parse_sources(result, kShaHex, "");
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "http://10.0.0.9:7801/by-sha256/" + kShaHex);
}

TEST(ParseSourcesTest, MalformedResultShapesTolerated) {
    const std::vector<json> bad = {
        json::array(),           // result 非 object
        json("garbage"),         // 标量
        json::object(),          // 无 sources 键
        json{{"sha256", kShaHex}, {"sources", json::object()}},  // 非 array
        json{{"sha256", kShaHex}, {"sources", "x"}},
    };
    for (const auto& result : bad) {
        EXPECT_TRUE(
            SwarmSourceProvider::parse_sources(result, kShaHex, "").empty())
            << result.dump();
    }
    // 空 sources 数组合法 → 空表
    const auto empty = SwarmSourceProvider::parse_sources(
        sources_result({}), kShaHex, "");
    EXPECT_TRUE(empty.empty());
}

// ---------------------------------------------------------------------------
// node_source_usable：单条可用性判定
// ---------------------------------------------------------------------------

TEST(NodeSourceUsableTest, DirectMatrix) {
    EXPECT_TRUE(SwarmSourceProvider::node_source_usable(
        node_source("n1", "10.0.0.9:7801", true)));
    EXPECT_FALSE(SwarmSourceProvider::node_source_usable(
        node_source("n1", "10.0.0.9:7801", false)));
    // direct 缺失 / 非布尔 → 不可用（不猜测）
    EXPECT_FALSE(SwarmSourceProvider::node_source_usable(
        json{{"type", "node"},
             {"advertise", json{{"addr", "10.0.0.9:7801"}}}}));
    EXPECT_FALSE(SwarmSourceProvider::node_source_usable(
        json{{"type", "node"},
             {"advertise",
              json{{"addr", "10.0.0.9:7801"}, {"direct", "yes"}}}}));
    // addr 无端口形态（无 ':'）→ 不可用
    EXPECT_FALSE(SwarmSourceProvider::node_source_usable(
        node_source("n1", "10.0.0.9", true)));
}

// ---------------------------------------------------------------------------
// sources_for：查询驱动 + 失败退避（§11）
// ---------------------------------------------------------------------------

TEST(SourcesForTest, SuccessParsesAndPassesShaToQuery) {
    FakeQuery query;
    query.set_payload(sources_result({node_source("n1", "10.0.0.9:7801", true),
                                      url_source("http://m1/f.bin")}));
    SwarmSourceProvider provider(query.fn(), "");

    const auto urls = provider.sources_for(kShaHex);
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_EQ(urls[0], "http://10.0.0.9:7801/by-sha256/" + kShaHex);
    EXPECT_EQ(urls[1], "http://m1/f.bin");

    // 查询实参 = 本任务整文件哈希
    const auto asked = query.asked();
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0], kShaHex);

    // 成功清除退避状态
    EXPECT_EQ(provider.consecutive_failures(), 0u);
    EXPECT_EQ(provider.suppressed_until(),
              std::chrono::steady_clock::time_point{});
}

TEST(SourcesForTest, FailureBacksOffAndSuppressedCallsSkipQuery) {
    FakeQuery query;
    query.push_failure();
    SwarmSourceProvider provider(query.fn(), "");

    // 首次失败：空表 + 记账 + 抑制（30s 起步）
    EXPECT_TRUE(provider.sources_for(kShaHex).empty());
    EXPECT_EQ(provider.consecutive_failures(), 1u);
    const auto until = provider.suppressed_until();
    EXPECT_GT(until, std::chrono::steady_clock::now());
    const auto window = std::chrono::duration_cast<std::chrono::seconds>(
        until - std::chrono::steady_clock::now());
    EXPECT_GE(window.count(), 25);
    EXPECT_LE(window.count(), 35);

    // 抑制期内：查询不发往网络，直接空表
    query.set_payload(sources_result({url_source("http://m1/f.bin")}));
    EXPECT_TRUE(provider.sources_for(kShaHex).empty());
    EXPECT_EQ(query.ask_count(), 1u);
}

TEST(SourcesForTest, MissingQueryFnReturnsEmptyWithoutState) {
    SwarmSourceProvider provider(nullptr, "");
    EXPECT_TRUE(provider.sources_for(kShaHex).empty());
    EXPECT_EQ(provider.consecutive_failures(), 0u);
    EXPECT_EQ(provider.suppressed_until(),
              std::chrono::steady_clock::time_point{});
}

}  // namespace
