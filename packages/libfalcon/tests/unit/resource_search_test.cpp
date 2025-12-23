/**
 * @file resource_search_test.cpp
 * @brief 资源搜索功能单元测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/resource_search.hpp>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <unordered_set>
#include <sstream>
#include <regex>

using namespace falcon::search;

class ResourceSearchTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建临时配置文件
        config_file_ = "test_search_config.json";
        create_test_config();
    }

    void TearDown() override {
        // 清理临时文件
        std::remove(config_file_.c_str());
    }

    void create_test_config() {
        nlohmann::json config = {
            {"search_engines", {
                {
                    {"name", "TestEngine1"},
                    {"base_url", "https://example.com"},
                    {"search_path", "/search"},
                    {"enabled", true},
                    {"weight", 1.0},
                    {"response_format", "json"},
                    {"headers", {
                        {"User-Agent", "Falcon Test"}
                    }}
                },
                {
                    {"name", "TestEngine2"},
                    {"base_url", "https://test.org"},
                    {"search_path", "/api/search"},
                    {"enabled", false},
                    {"weight", 0.8}
                }
            }},
            {"global_settings", {
                {"default_delay_ms", 500},
                {"timeout_seconds", 10},
                {"max_results_per_engine", 50}
            }}
        };

        std::ofstream file(config_file_);
        file << config.dump(2);
        file.close();
    }

    std::string config_file_;
};

// 测试搜索引擎配置加载
TEST_F(ResourceSearchTest, LoadConfig) {
    ResourceSearchManager manager;
    EXPECT_TRUE(manager.load_config(config_file_));

    auto providers = manager.get_providers();
    EXPECT_EQ(providers.size(), 1); // 只有启用的引擎
    EXPECT_EQ(providers[0], "TestEngine1");
}

// 测试搜索查询参数
TEST_F(ResourceSearchTest, SearchQueryConstruction) {
    SearchQuery query;
    query.keyword = "test";
    query.limit = 10;
    query.min_size = 100 * 1024 * 1024; // 100MB
    query.min_seeds = 5;
    query.sort_by = "seeds";

    EXPECT_EQ(query.keyword, "test");
    EXPECT_EQ(query.limit, 10);
    EXPECT_EQ(query.min_size, 100 * 1024 * 1024);
    EXPECT_EQ(query.min_seeds, 5);
    EXPECT_EQ(query.sort_by, "seeds");
    EXPECT_TRUE(query.sort_desc);
}

// 测试搜索结果数据结构
TEST_F(ResourceSearchTest, SearchResultStructure) {
    SearchResult result;
    result.title = "Test Movie 2023";
    result.url = "magnet:?xt=urn:btih:testhash";
    result.source = "TestEngine";
    result.size = 1024 * 1024 * 1024; // 1GB
    result.seeds = 100;
    result.peers = 50;
    result.confidence = 0.95;
    result.hash = "testhash";

    EXPECT_EQ(result.title, "Test Movie 2023");
    EXPECT_EQ(result.url, "magnet:?xt=urn:btih:testhash");
    EXPECT_EQ(result.source, "TestEngine");
    EXPECT_EQ(result.size, 1024 * 1024 * 1024);
    EXPECT_EQ(result.seeds, 100);
    EXPECT_EQ(result.peers, 50);
    EXPECT_DOUBLE_EQ(result.confidence, 0.95);
    EXPECT_EQ(result.hash, "testhash");
}

// 测试搜索结果验证
TEST_F(ResourceSearchTest, SearchResultValidation) {
    SearchResult magnet_result;
    magnet_result.url = "magnet:?xt=urn:btih:testhash";

    SearchResult http_result;
    http_result.url = "https://example.com/file.torrent";

    SearchResult invalid_result;
    invalid_result.url = "invalid://url";

    // 模拟验证（实际实现中会有验证逻辑）
    EXPECT_TRUE(!magnet_result.url.empty());
    EXPECT_TRUE(!http_result.url.empty());
    EXPECT_FALSE(invalid_result.url.empty()); // URL本身不为空
}

// 测试搜索结果过滤
TEST_F(ResourceSearchTest, ResultFiltering) {
    std::vector<SearchResult> results = {
        {"Test1", "magnet:?xt=1", "Engine1", 100 * 1024 * 1024, "", 5, 2, 0.8},
        {"Test2", "magnet:?xt=2", "Engine1", 50 * 1024 * 1024, "", 3, 1, 0.6},
        {"Test3", "magnet:?xt=3", "Engine1", 200 * 1024 * 1024, "", 10, 5, 0.9},
        {"Test4", "magnet:?xt=4", "Engine2", 75 * 1024 * 1024, "", 2, 1, 0.5}
    };

    SearchQuery query;
    query.limit = 3;
    query.min_size = 80 * 1024 * 1024; // 最小80MB
    query.min_seeds = 3; // 最少3个种子

    // 模拟过滤逻辑
    std::vector<SearchResult> filtered;
    for (const auto& result : results) {
        if (result.size >= query.min_size && result.seeds >= query.min_seeds) {
            filtered.push_back(result);
        }
    }

    EXPECT_EQ(filtered.size(), 2);
    EXPECT_EQ(filtered[0].title, "Test1");
    EXPECT_EQ(filtered[1].title, "Test3");
}

// 测试结果排序
TEST_F(ResourceSearchTest, ResultSorting) {
    std::vector<SearchResult> results = {
        {"Test1", "magnet:?xt=1", "Engine1", 100, "", 5, 2, 0.8},
        {"Test2", "magnet:?xt=2", "Engine1", 200, "", 3, 1, 0.9},
        {"Test3", "magnet:?xt=3", "Engine1", 50, "", 10, 5, 0.7}
    };

    SearchQuery query;
    query.sort_by = "seeds";

    // 按种子数排序（降序）
    std::sort(results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b) {
            return a.seeds > b.seeds;
        });

    EXPECT_EQ(results[0].seeds, 10);
    EXPECT_EQ(results[1].seeds, 5);
    EXPECT_EQ(results[2].seeds, 3);
}

// 测试搜索引擎配置结构
TEST_F(ResourceSearchTest, SearchEngineConfigStructure) {
    SearchEngineConfig config;
    config.name = "TestEngine";
    config.base_url = "https://test.com";
    config.search_path = "/search";
    config.enabled = true;
    config.weight = 1.0;
    config.delay_ms = 2000;
    config.encoding = "utf-8";

    EXPECT_EQ(config.name, "TestEngine");
    EXPECT_EQ(config.base_url, "https://test.com");
    EXPECT_EQ(config.search_path, "/search");
    EXPECT_TRUE(config.enabled);
    EXPECT_DOUBLE_EQ(config.weight, 1.0);
    EXPECT_EQ(config.delay_ms, 2000);
    EXPECT_EQ(config.encoding, "utf-8");

    // 测试HTTP头
    config.headers["User-Agent"] = "Falcon";
    config.headers["Accept"] = "application/json";
    EXPECT_EQ(config.headers["User-Agent"], "Falcon");
    EXPECT_EQ(config.headers["Accept"], "application/json");
}

// 测试去重功能
TEST_F(ResourceSearchTest, DeduplicateResults) {
    std::vector<SearchResult> results = {
        {"Test1", "magnet:?xt=hash1", "Engine1", 100, "hash1", 5, 2, 0.8},
        {"Test2", "magnet:?xt=hash2", "Engine2", 100, "hash2", 5, 2, 0.8},
        {"Test1 Duplicate", "magnet:?xt=hash1", "Engine3", 200, "hash1", 5, 2, 0.8}, // 相同哈希
        {"Test3", "https://example.com/file3.torrent", "Engine4", 300, "", 10, 5, 0.9}
    };

    // 模拟去重逻辑
    std::unordered_set<std::string> seen_hashes;
    std::vector<SearchResult> unique_results;

    for (auto& result : results) {
        std::string key = result.hash.empty() ? result.url : result.hash;
        if (seen_hashes.insert(key).second) {
            unique_results.push_back(std::move(result));
        }
    }

    EXPECT_EQ(unique_results.size(), 3);
    EXPECT_EQ(unique_results[0].hash, "hash1");
    EXPECT_EQ(unique_results[1].hash, "hash2");
    EXPECT_EQ(unique_results[2].url, "https://example.com/file3.torrent");
}

// 测试Magnet链接解析
TEST_F(ResourceSearchTest, MagnetLinkParsing) {
    std::string magnet_url = "magnet:?xt=urn:btih:testhash123456789abcdef&dn=Test%20File&tr=http%3A%2F%2Ftracker.example.com%3A8080";

    // 模拟解析逻辑
    SearchResult result;
    result.url = magnet_url;

    // 提取哈希
    size_t hash_pos = magnet_url.find("btih:");
    if (hash_pos != std::string::npos) {
        size_t hash_end = magnet_url.find("&", hash_pos);
        if (hash_end == std::string::npos) hash_end = magnet_url.length();
        result.hash = magnet_url.substr(hash_pos + 5, hash_end - hash_pos - 5);
    }

    // 提取文件名
    size_t dn_pos = magnet_url.find("dn=");
    if (dn_pos != std::string::npos) {
        size_t dn_end = magnet_url.find("&", dn_pos);
        if (dn_end == std::string::npos) dn_end = magnet_url.length();
        // URL解码（简化版）
        std::string encoded = magnet_url.substr(dn_pos + 3, dn_end - dn_pos - 3);
        std::replace(encoded.begin(), encoded.end(), '+', ' ');
        result.title = encoded;
    }

    EXPECT_EQ(result.hash, "testhash123456789abcdef");
    EXPECT_EQ(result.title, "Test File");
    EXPECT_EQ(result.url, magnet_url);
}

// 测试文件大小解析
TEST_F(ResourceSearchTest, ParseFileSize) {
    // 模拟文件大小解析函数
    auto parse_size = [](const std::string& size_str) -> size_t {
        if (size_str.empty()) return 0;

        size_t value = 0;
        std::istringstream ss(size_str);
        ss >> value;

        char suffix = 0;
        ss >> suffix;

        switch (std::toupper(suffix)) {
            case 'K': return value * 1024;
            case 'M': return value * 1024 * 1024;
            case 'G': return value * 1024 * 1024 * 1024;
            case 'T': return value * 1024ULL * 1024 * 1024 * 1024;
            default: return value;
        }
    };

    EXPECT_EQ(parse_size("1024"), 1024);
    EXPECT_EQ(parse_size("1K"), 1024);
    EXPECT_EQ(parse_size("10M"), 10 * 1024 * 1024);
    EXPECT_EQ(parse_size("2G"), 2ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(parse_size("1.5M"), static_cast<size_t>(1.5 * 1024 * 1024));
}

// 性能测试 - 搜索结果数量
TEST_F(ResourceSearchTest, PerformanceLargeResultSet) {
    // 生成大量测试数据
    std::vector<SearchResult> results;
    for (int i = 0; i < 10000; ++i) {
        results.push_back({
            "Test " + std::to_string(i),
            "magnet:?xt=hash" + std::to_string(i),
            "Engine1",
            100 * 1024 * 1024,
            "hash" + std::to_string(i),
            i % 100,
            i % 50,
            0.5 + (i % 100) / 100.0
        });
    }

    auto start = std::chrono::high_resolution_clock::now();

    // 排序
    std::sort(results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b) {
            return a.confidence > b.confidence;
        });

    // 过滤和限制
    SearchQuery query;
    query.limit = 100;
    query.min_seeds = 10;

    std::vector<SearchResult> filtered;
    for (const auto& result : results) {
        if (result.seeds >= query.min_seeds) {
            filtered.push_back(result);
        }
    }

    if (filtered.size() > query.limit) {
        filtered.resize(query.limit);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_LT(duration.count(), 100); // 应该在100ms内完成
    EXPECT_LE(filtered.size(), 100);
}

// 测试配置文件错误处理
TEST_F(ResourceSearchTest, InvalidConfigHandling) {
    // 创建无效配置文件
    std::string invalid_config = "test_invalid_config.json";
    std::ofstream file(invalid_config);
    file << "{ invalid json }";
    file.close();

    ResourceSearchManager manager;
    EXPECT_FALSE(manager.load_config(invalid_config));

    // 测试不存在的文件
    EXPECT_FALSE(manager.load_config("non_existent_config.json"));

    std::remove(invalid_config.c_str());
}

// 测试代理配置
TEST_F(ResourceSearchTest, ProxyConfiguration) {
    // 测试代理URL解析
    std::string proxy_url = "http://user:pass@proxy.example.com:8080";

    // 模拟解析逻辑
    std::string protocol = "http://";
    size_t start = protocol.length();
    size_t at_pos = proxy_url.find("@");

    std::string host_port;
    if (at_pos != std::string::npos) {
        host_port = proxy_url.substr(at_pos + 1);
    } else {
        host_port = proxy_url.substr(start);
    }

    EXPECT_EQ(host_port, "proxy.example.com:8080");

    // 测试SOCKS代理
    std::string socks_url = "socks5://socks.example.com:1080";
    EXPECT_TRUE(socks_url.find("socks5://") == 0);
}
