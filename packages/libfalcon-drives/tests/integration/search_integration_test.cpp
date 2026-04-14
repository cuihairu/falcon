/**
 * @file search_integration_test.cpp
 * @brief 资源搜索功能集成测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/resource_search.hpp>
#include <falcon/download_engine.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <thread>
#include <chrono>
#include <set>
#include <algorithm>
#include <chrono>

using namespace falcon::search;

class SearchIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建完整的测试配置
        config_file_ = "test_search_integration.json";
        create_integration_test_config();
    }

    void TearDown() override {
        std::remove(config_file_.c_str());
    }

    void create_integration_test_config() {
        nlohmann::json config = {
            {"search_engines", {
                {
                    {"name", "MockEngine"},
                    {"base_url", "https://mockapi.example.com"},
                    {"search_path", "/api/search"},
                    {"enabled", true},
                    {"weight", 1.0},
                    {"response_format", "json"},
                    {"delay_ms", 100},
                    {"headers", {
                        {"User-Agent", "Falcon/1.0"},
                        {"Accept", "application/json"}
                    }}
                }
            }},
            {"global_settings", {
                {"timeout_seconds", 30},
                {"max_results_per_engine", 50},
                {"parallel_requests", 2},
                {"proxy", {
                    {"enabled", false}
                }}
            }}
        };

        std::ofstream file(config_file_);
        file << config.dump(2);
        file.close();
    }

    std::string config_file_;
};

// 模拟搜索引擎提供者
class MockSearchProvider : public ISearchProvider {
public:
    MockSearchProvider(const std::string& name) : name_(name) {}

    std::string name() const override {
        return name_;
    }

    std::vector<SearchResult> search(const SearchQuery& query) override {
        std::vector<SearchResult> results;

        // 模拟搜索延迟
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 生成模拟结果
        for (size_t i = 1; i <= query.limit; ++i) {
            SearchResult result;
            result.title = query.keyword + " - Part " + std::to_string(i);
            result.url = "magnet:?xt=urn:btih:mockhash" + std::to_string(i);
            result.source = name_;
            result.size = (i * 100) * 1024 * 1024; // 100MB increments
            result.seeds = static_cast<int>((query.limit - i) * 2); // Decreasing seeds
            result.peers = static_cast<int>((query.limit - i) * 3);
            result.confidence = 0.5 + (i * 0.05);
            result.hash = "mockhash" + std::to_string(i);
            result.publish_date = "2023-12-" + std::to_string(i % 28 + 1);

            // 应用过滤条件
            if (query.min_size > 0 && result.size < query.min_size) continue;
            if (query.max_size > 0 && result.size > query.max_size) continue;
            if (query.min_seeds > 0 && result.seeds < query.min_seeds) continue;

            results.push_back(result);
        }

        // 排序
        if (!query.sort_by.empty()) {
            if (query.sort_by == "seeds") {
                std::sort(results.begin(), results.end(),
                    [query](const SearchResult& a, const SearchResult& b) {
                        return query.sort_desc ? a.seeds > b.seeds : a.seeds < b.seeds;
                    });
            } else if (query.sort_by == "size") {
                std::sort(results.begin(), results.end(),
                    [query](const SearchResult& a, const SearchResult& b) {
                        return query.sort_desc ? a.size > b.size : a.size < b.size;
                    });
            }
        }

        return results;
    }

    bool validate_url(const std::string& url) override {
        return !url.empty() &&
               (url.find("magnet:") == 0 || url.find("http:") == 0 || url.find("https:") == 0);
    }

    SearchResult get_details(const std::string& url) override {
        SearchResult result;
        result.url = url;
        result.source = name_;

        if (url.find("magnet:") == 0) {
            // 解析magnet链接
            size_t hash_pos = url.find("btih:");
            if (hash_pos != std::string::npos) {
                size_t hash_end = url.find("&", hash_pos);
                if (hash_end == std::string::npos) hash_end = url.length();
                result.hash = url.substr(hash_pos + 5, hash_end - hash_pos - 5);
            }
        }

        return result;
    }

    bool is_available() override {
        return true; // Mock总是可用
    }

    int get_delay() const override {
        return 100;
    }

private:
    std::string name_;
};

// 测试完整的搜索流程
TEST_F(SearchIntegrationTest, CompleteSearchWorkflow) {
    ResourceSearchManager manager;
    manager.load_config(config_file_);

    // 注册模拟提供者
    manager.register_provider(std::make_unique<MockSearchProvider>("MockEngine"));

    // 执行搜索
    SearchQuery query;
    query.keyword = "Ubuntu 22.04";
    query.limit = 10;
    query.category = "software";

    auto results = manager.search_all(query);

    EXPECT_GT(results.size(), 0);
    EXPECT_LE(results.size(), 10);

    // 验证结果结构
    for (const auto& result : results) {
        EXPECT_FALSE(result.title.empty());
        EXPECT_FALSE(result.url.empty());
        EXPECT_FALSE(result.source.empty());
        EXPECT_GT(result.size, 0);
        EXPECT_GE(result.seeds, 0);
        EXPECT_GE(result.confidence, 0.0);
        EXPECT_LE(result.confidence, 1.0);
    }
}

// 测试多提供者搜索
TEST_F(SearchIntegrationTest, MultipleProviderSearch) {
    ResourceSearchManager manager;
    manager.load_config(config_file_);

    // 注册多个提供者
    manager.register_provider(std::make_unique<MockSearchProvider>("Engine1"));
    manager.register_provider(std::make_unique<MockSearchProvider>("Engine2"));
    manager.register_provider(std::make_unique<MockSearchProvider>("Engine3"));

    SearchQuery query;
    query.keyword = "Test Movie";
    query.limit = 20;

    auto results = manager.search_all(query);

    // 应该有来自不同提供者的结果
    std::set<std::string> sources;
    for (const auto& result : results) {
        sources.insert(result.source);
    }

    EXPECT_GE(sources.size(), 2);
    EXPECT_GT(results.size(), 0);
}

// 测试带过滤条件的搜索
TEST_F(SearchIntegrationTest, SearchWithFilters) {
    ResourceSearchManager manager;
    manager.register_provider(std::make_unique<MockSearchProvider>("FilterEngine"));

    SearchQuery query;
    query.keyword = "Large File";
    query.limit = 20;
    query.min_size = 500 * 1024 * 1024; // 500MB minimum
    query.max_size = 2ULL * 1024 * 1024 * 1024; // 2GB maximum
    query.min_seeds = 5;
    query.sort_by = "seeds";

    auto results = manager.search_all(query);

    for (const auto& result : results) {
        EXPECT_GE(result.size, query.min_size);
        EXPECT_LE(result.size, query.max_size);
        EXPECT_GE(result.seeds, query.min_seeds);
    }

    // 验证排序
    if (results.size() > 1) {
        for (size_t i = 0; i < results.size() - 1; ++i) {
            EXPECT_GE(results[i].seeds, results[i + 1].seeds);
        }
    }
}

// 测试并发搜索
TEST_F(SearchIntegrationTest, ConcurrentSearch) {
    ResourceSearchManager manager;

    // 注册多个提供者
    for (int i = 0; i < 5; ++i) {
        manager.register_provider(
            std::make_unique<MockSearchProvider>("ConcurrentEngine" + std::to_string(i))
        );
    }

    auto start = std::chrono::high_resolution_clock::now();

    SearchQuery query;
    query.keyword = "Concurrent Test";
    query.limit = 10;

    auto results = manager.search_all(query);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 并发搜索应该比串行搜索快
    EXPECT_LT(duration.count(), 500); // 应该在500ms内完成
    EXPECT_GT(results.size(), 0);
}

// 测试搜索结果与下载引擎集成
TEST_F(SearchIntegrationTest, SearchToDownloadIntegration) {
    // 1. 执行搜索
    ResourceSearchManager search_manager;
    search_manager.register_provider(std::make_unique<MockSearchProvider>("DownloadTest"));

    SearchQuery query;
    query.keyword = "Download Test File";
    query.limit = 5;

    auto search_results = search_manager.search_all(query);
    ASSERT_GT(search_results.size(), 0);

    // 2. 选择第一个结果的URL
    std::string download_url = search_results[0].url;

    // 3. 创建下载引擎（模拟）
    falcon::DownloadEngine engine;
    falcon::DownloadOptions options;
    options.output_directory = "./test_downloads";
    options.max_connections = 2;

    // 在实际实现中，这里会启动下载
    // auto task = engine.start_download(download_url, options);

    // 验证URL有效性
    search_manager.register_provider(std::make_unique<MockSearchProvider>("Validator"));
    auto& provider = *(static_cast<MockSearchProvider*>(search_manager.get_providers()[0].get()));
    EXPECT_TRUE(provider.validate_url(download_url));
}

// 测试配置文件热重载
TEST_F(SearchIntegrationTest, ConfigReload) {
    ResourceSearchManager manager;
    manager.load_config(config_file_);

    auto initial_providers = manager.get_providers();
    EXPECT_EQ(initial_providers.size(), 0); // 配置中的MockEngine需要注册

    // 修改配置文件
    nlohmann::json new_config = {
        {"search_engines", {
            {
                {"name", "NewEngine"},
                {"base_url", "https://newapi.example.com"},
                {"enabled", true}
            },
            {
                {"name", "DisabledEngine"},
                {"enabled", false}
            }
        }}
    };

    std::ofstream file(config_file_);
    file << new_config.dump(2);
    file.close();

    // 重新加载配置
    EXPECT_TRUE(manager.load_config(config_file_));
}

// 测试搜索建议
TEST_F(SearchIntegrationTest, SearchSuggestions) {
    ResourceSearchManager manager;

    // 注册提供者
    manager.register_provider(std::make_unique<MockSearchProvider>("SuggestionEngine"));

    std::string keyword = "Ubuntu";
    auto suggestions = manager.get_suggestions(keyword);

    EXPECT_GT(suggestions.size(), 0);
    for (const auto& suggestion : suggestions) {
        EXPECT_TRUE(suggestion.find(keyword) != std::string::npos);
    }
}

// 测试错误处理
TEST_F(SearchIntegrationTest, ErrorHandling) {
    // 创建一个会失败的模拟提供者
    class FailingProvider : public ISearchProvider {
    public:
        std::string name() const override { return "FailingEngine"; }

        std::vector<SearchResult> search(const SearchQuery& query) override {
            throw std::runtime_error("Search failed");
        }

        bool validate_url(const std::string& url) override { return false; }
        SearchResult get_details(const std::string& url) override { return {}; }
        bool is_available() override { return false; }
    };

    ResourceSearchManager manager;
    manager.register_provider(std::make_unique<MockSearchProvider>("GoodEngine"));
    manager.register_provider(std::make_unique<FailingProvider>());

    SearchQuery query;
    query.keyword = "Test Error";
    query.limit = 5;

    // 即使有失败的提供者，其他提供者应该继续工作
    auto results = manager.search_all(query);
    EXPECT_GT(results.size(), 0);
}

// 性能测试 - 大量搜索结果处理
TEST_F(SearchIntegrationTest, PerformanceLargeResultHandling) {
    ResourceSearchManager manager;

    // 注册生成大量结果的提供者
    class LargeResultProvider : public ISearchProvider {
    public:
        std::string name() const override { return "LargeResultEngine"; }

        std::vector<SearchResult> search(const SearchQuery& query) override {
            std::vector<SearchResult> results;
            for (int i = 0; i < 10000; ++i) {
                SearchResult result;
                result.title = "Large Result " + std::to_string(i);
                result.url = "magnet:?xt=hash" + std::to_string(i);
                result.source = name_;
                result.size = 1024 * 1024;
                result.seeds = i % 100;
                result.peers = i % 50;
                result.confidence = 0.5 + (i % 50) / 100.0;
                results.push_back(result);
            }
            return results;
        }

        bool validate_url(const std::string& url) override { return true; }
        SearchResult get_details(const std::string& url) override { return {}; }
        bool is_available() override { return true; }
    };

    manager.register_provider(std::make_unique<LargeResultProvider>());

    SearchQuery query;
    query.keyword = "Large Test";
    query.limit = 1000;

    auto start = std::chrono::high_resolution_clock::now();
    auto results = manager.search_all(query);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_LT(duration.count(), 1000); // 应该在1秒内处理完成
    EXPECT_LE(results.size(), 1000);
}