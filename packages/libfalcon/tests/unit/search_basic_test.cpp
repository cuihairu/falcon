/**
 * @file search_basic_test.cpp
 * @brief 基础搜索功能测试（不依赖JSON库）
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/resource_search.hpp>
#include <algorithm>
#include <vector>

using namespace falcon::search;

// 模拟搜索引擎提供者（不使用JSON）
class MockSearchProvider : public ISearchProvider {
public:
    MockSearchProvider(const std::string& name) : name_(name) {}

    std::string name() const override {
        return name_;
    }

    std::vector<SearchResult> search(const SearchQuery& query) override {
        std::vector<SearchResult> results;

        // 生成模拟结果
        for (size_t i = 1; i <= query.limit; ++i) {
            SearchResult result;
            result.title = query.keyword + " - Result " + std::to_string(i);
            result.url = "https://mocksite.com/file" + std::to_string(i) + ".zip";
            result.source = name_;
            result.size = i * 100 * 1024 * 1024; // 100MB increments
            result.seeds = static_cast<int>((query.limit - i) * 2);
            result.peers = static_cast<int>((query.limit - i) * 3);
            result.confidence = 0.5 + (i * 0.05);

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
        return !url.empty() && url.find("http") == 0;
    }

    SearchResult get_details(const std::string& url) override {
        SearchResult result;
        result.url = url;
        result.source = name_;
        return result;
    }

    bool is_available() override {
        return true;
    }

    int get_delay() const override {
        return 100;
    }

private:
    std::string name_;
};

class SearchBasicTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<ResourceSearchManager>();
    }

    void TearDown() override {
        manager_.reset();
    }

    std::unique_ptr<ResourceSearchManager> manager_;
};

// 测试搜索查询参数
TEST_F(SearchBasicTest, SearchQueryParameters) {
    SearchQuery query;
    query.keyword = "test";
    query.limit = 10;
    query.min_size = 100 * 1024 * 1024;
    query.min_seeds = 5;
    query.sort_by = "seeds";

    EXPECT_EQ(query.keyword, "test");
    EXPECT_EQ(query.limit, 10);
    EXPECT_EQ(query.min_size, 100 * 1024 * 1024);
    EXPECT_EQ(query.min_seeds, 5);
    EXPECT_EQ(query.sort_by, "seeds");
    EXPECT_TRUE(query.sort_desc);
}

// 测试搜索结果结构
TEST_F(SearchBasicTest, SearchResultStructure) {
    SearchResult result;
    result.title = "Test File";
    result.url = "https://example.com/file.zip";
    result.source = "TestEngine";
    result.size = 1024 * 1024;
    result.seeds = 10;
    result.peers = 5;
    result.confidence = 0.95;

    EXPECT_EQ(result.title, "Test File");
    EXPECT_EQ(result.url, "https://example.com/file.zip");
    EXPECT_EQ(result.source, "TestEngine");
    EXPECT_EQ(result.size, 1024 * 1024);
    EXPECT_EQ(result.seeds, 10);
    EXPECT_EQ(result.peers, 5);
    EXPECT_DOUBLE_EQ(result.confidence, 0.95);
}

// 测试搜索管理器注册提供者
TEST_F(SearchBasicTest, RegisterProvider) {
    manager_->register_provider(std::make_unique<MockSearchProvider>("TestEngine"));

    auto providers = manager_->get_providers();
    EXPECT_GT(providers.size(), 0);

    // 查找注册的提供者
    bool found = std::find(providers.begin(), providers.end(), "TestEngine") != providers.end();
    EXPECT_TRUE(found);
}

// 测试搜索功能
TEST_F(SearchBasicTest, PerformSearch) {
    manager_->register_provider(std::make_unique<MockSearchProvider>("TestEngine"));

    SearchQuery query;
    query.keyword = "Ubuntu";
    query.limit = 5;

    auto results = manager_->search_all(query);

    EXPECT_GT(results.size(), 0);
    EXPECT_LE(results.size(), 5);

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

// 测试搜索过滤
TEST_F(SearchBasicTest, SearchFilters) {
    manager_->register_provider(std::make_unique<MockSearchProvider>("TestEngine"));

    SearchQuery query;
    query.keyword = "Large File";
    query.limit = 10;
    query.min_size = 500 * 1024 * 1024; // 500MB
    query.min_seeds = 5;

    auto results = manager_->search_all(query);

    for (const auto& result : results) {
        EXPECT_GE(result.size, query.min_size);
        EXPECT_GE(result.seeds, query.min_seeds);
    }
}

// 测试搜索排序
TEST_F(SearchBasicTest, SearchSorting) {
    manager_->register_provider(std::make_unique<MockSearchProvider>("TestEngine"));

    SearchQuery query;
    query.keyword = "Sort Test";
    query.limit = 10;
    query.sort_by = "seeds";

    auto results = manager_->search_all(query);

    // 验证降序排序
    if (results.size() > 1) {
        for (size_t i = 0; i < results.size() - 1; ++i) {
            EXPECT_GE(results[i].seeds, results[i + 1].seeds);
        }
    }
}

// 测试多个提供者
TEST_F(SearchBasicTest, MultipleProviders) {
    manager_->register_provider(std::make_unique<MockSearchProvider>("Engine1"));
    manager_->register_provider(std::make_unique<MockSearchProvider>("Engine2"));
    manager_->register_provider(std::make_unique<MockSearchProvider>("Engine3"));

    auto providers = manager_->get_providers();
    EXPECT_GE(providers.size(), 3);

    SearchQuery query;
    query.keyword = "Multi Engine";
    query.limit = 5;

    auto results = manager_->search_all(query);
    EXPECT_GT(results.size(), 0);

    // 结果应该来自不同的提供者
    std::set<std::string> sources;
    for (const auto& result : results) {
        sources.insert(result.source);
    }
    EXPECT_GT(sources.size(), 1);
}

// 测试URL验证
TEST_F(SearchBasicTest, URLValidation) {
    MockSearchProvider provider("Test");

    EXPECT_TRUE(provider.validate_url("https://example.com/file.zip"));
    EXPECT_TRUE(provider.validate_url("http://test.org/data.bin"));
    EXPECT_FALSE(provider.validate_url(""));
    EXPECT_FALSE(provider.validate_url("invalid-url"));
}

// 测试获取详细信息
TEST_F(SearchBasicTest, GetDetails) {
    MockSearchProvider provider("Test");
    std::string url = "https://example.com/test.zip";

    auto details = provider.get_details(url);
    EXPECT_EQ(details.url, url);
    EXPECT_EQ(details.source, "Test");
}

// 测试可用性检查
TEST_F(SearchBasicTest, AvailabilityCheck) {
    MockSearchProvider provider("Test");
    EXPECT_TRUE(provider.is_available());
}

// 测试获取延迟
TEST_F(SearchBasicTest, GetDelay) {
    MockSearchProvider provider("Test");
    EXPECT_EQ(provider.get_delay(), 100);
}