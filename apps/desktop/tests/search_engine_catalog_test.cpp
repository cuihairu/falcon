/**
 * @file search_engine_catalog_test.cpp
 * @brief SearchEngineCatalog 单元测试（engines.json 目录读取 / 模板生成 / 启停切换，纯 C++）
 * @author Falcon Team
 * @date 2026-09-28
 */

#include "services/search_engine_catalog.hpp"

#include <falcon/drives/resource_search.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

/// 临时目录（构造创建、析构递归清理；random_device 熵源防跨进程同名）
class TempDir {
public:
    TempDir()
    {
        path_ = fs::temp_directory_path()
                / ("falcon_catalog_test_"
                   + std::to_string(std::random_device{}()));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }
    std::string engines_path() const
    {
        return (path_ / "engines.json").string();
    }

private:
    fs::path path_;
};

/// 在指定路径写一个已知内容的文件
void write_file(const fs::path& p, const std::string& content)
{
    std::ofstream out(p, std::ios::binary);
    out << content;
}

/// 读文件全部内容（不存在返回空串）
std::string read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

} // namespace

namespace falcon::desktop {

// ---------------------------------------------------------------------------
// ensure_default：全 disabled 模板
// ---------------------------------------------------------------------------

TEST(SearchEngineCatalogTest, EnsureDefaultCreatesAllDisabledTemplate)
{
    TempDir dir;
    const std::string path = dir.engines_path();

    EXPECT_FALSE(fs::exists(path));
    EXPECT_TRUE(SearchEngineCatalog::ensure_default(path));
    ASSERT_TRUE(fs::exists(path));

    SearchEngineCatalog catalog(path);
    ASSERT_TRUE(catalog.load());
    ASSERT_FALSE(catalog.engines().empty());

    // 模板全部 disabled（manager 侧 enabled 缺省 true——模板必须显式 false）
    for (const auto& engine : catalog.engines()) {
        EXPECT_FALSE(engine.enabled) << engine.name;
    }
    EXPECT_FALSE(catalog.has_enabled());
}

TEST(SearchEngineCatalogTest, EnsureDefaultKeepsExistingFile)
{
    TempDir dir;
    const std::string path = dir.engines_path();
    write_file(path, "{}");

    EXPECT_TRUE(SearchEngineCatalog::ensure_default(path));
    // 已有配置（含损坏内容）绝不覆盖
    EXPECT_EQ(read_file(path), "{}");
}

// ---------------------------------------------------------------------------
// load：缺失 / 损坏按空目录且不写盘
// ---------------------------------------------------------------------------

TEST(SearchEngineCatalogTest, LoadMissingFileFailsWithoutWriting)
{
    TempDir dir;
    const std::string path = dir.engines_path();

    SearchEngineCatalog catalog(path);
    EXPECT_FALSE(catalog.load());
    EXPECT_TRUE(catalog.engines().empty());

    // load 缺文件不写盘（首次落盘只经 ensure_default）
    EXPECT_FALSE(fs::exists(path));
}

TEST(SearchEngineCatalogTest, LoadCorruptFileFailsAndPreservesContent)
{
    TempDir dir;
    const std::string path = dir.engines_path();
    write_file(path, "not-json{{{");

    SearchEngineCatalog catalog(path);
    EXPECT_FALSE(catalog.load());
    EXPECT_TRUE(catalog.engines().empty());
    // 原样保留，绝不覆盖
    EXPECT_EQ(read_file(path), "not-json{{{");
}

TEST(SearchEngineCatalogTest, LoadRejectsEngineMissingNameOrBaseUrl)
{
    TempDir dir;
    const std::string path = dir.engines_path();
    // manager load_config 对缺 name/base_url 直接 .get 抛异常整份拒绝——
    // 目录同语义按损坏处理
    write_file(path, R"({"search_engines":[{"name":"only"}]})");

    SearchEngineCatalog catalog(path);
    EXPECT_FALSE(catalog.load());
    EXPECT_TRUE(catalog.engines().empty());
}

// ---------------------------------------------------------------------------
// set_enabled：单键翻转 + 持久化
// ---------------------------------------------------------------------------

TEST(SearchEngineCatalogTest, SetEnabledPersistsAcrossReload)
{
    TempDir dir;
    const std::string path = dir.engines_path();
    ASSERT_TRUE(SearchEngineCatalog::ensure_default(path));

    SearchEngineCatalog catalog(path);
    ASSERT_TRUE(catalog.load());
    ASSERT_FALSE(catalog.has_enabled());

    EXPECT_TRUE(catalog.set_enabled("example", true));

    // 重开实例读取：翻转已持久
    SearchEngineCatalog reloaded(path);
    ASSERT_TRUE(reloaded.load());
    ASSERT_FALSE(reloaded.engines().empty());
    EXPECT_TRUE(reloaded.engines().front().enabled);
    EXPECT_TRUE(reloaded.has_enabled());

    // 未知引擎名失败
    EXPECT_FALSE(reloaded.set_enabled("no-such-engine", true));
}

TEST(SearchEngineCatalogTest, SetEnabledPreservesAdvancedFields)
{
    TempDir dir;
    const std::string path = dir.engines_path();
    // 全字段的用户配置：headers/params/selectors/_usage/global_settings
    write_file(path, R"({
  "version": 1,
  "_usage": {"how_to": "doc"},
  "global_settings": {"default_delay_ms": 3000},
  "search_engines": [
    {
      "name": "mine",
      "base_url": "https://example.org",
      "search_path": "/find",
      "params": {"q": "", "page": "1"},
      "headers": {"User-Agent": "UA/1.0"},
      "selectors": {"item": "pattern", "title": "title-pattern"},
      "response_format": "html",
      "enabled": false,
      "delay_ms": 5000
    }
  ]
})");

    SearchEngineCatalog catalog(path);
    ASSERT_TRUE(catalog.load());
    ASSERT_EQ(catalog.engines().size(), 1u);
    EXPECT_EQ(catalog.engines().front().name, "mine");
    EXPECT_EQ(catalog.engines().front().base_url, "https://example.org");
    EXPECT_EQ(catalog.engines().front().search_path, "/find");
    EXPECT_EQ(catalog.engines().front().response_format, "html");
    EXPECT_FALSE(catalog.engines().front().enabled);
    EXPECT_EQ(catalog.engines().front().delay_ms, 5000);

    // 翻转后再读原文：仅 enabled 键变化，其余字段全保留
    ASSERT_TRUE(catalog.set_enabled("mine", true));
    const std::string after = read_file(path);
    EXPECT_NE(after.find("\"User-Agent\": \"UA/1.0\""), std::string::npos);
    EXPECT_NE(after.find("title-pattern"), std::string::npos);
    EXPECT_NE(after.find("\"how_to\": \"doc\""), std::string::npos);
    EXPECT_NE(after.find("\"default_delay_ms\": 3000"), std::string::npos);
    EXPECT_NE(after.find("\"delay_ms\": 5000"), std::string::npos);

    SearchEngineCatalog reloaded(path);
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.engines().size(), 1u);
    EXPECT_TRUE(reloaded.engines().front().enabled);
}

// ---------------------------------------------------------------------------
// 与 drives ResourceSearchManager 对拍：目录展示的引擎集合必须是
// manager 真能加载的（防"目录侧一套语义、manager 侧另一套"自我印证）
// ---------------------------------------------------------------------------

TEST(SearchEngineCatalogTest, TemplateFeedsRealManager)
{
    TempDir dir;
    const std::string path = dir.engines_path();
    ASSERT_TRUE(SearchEngineCatalog::ensure_default(path));

    // 全 disabled 模板：manager 整份加载成功但零 provider
    {
        falcon::search::ResourceSearchManager manager;
        ASSERT_TRUE(manager.load_config(path));
        EXPECT_TRUE(manager.get_providers().empty());
    }

    // 勾选启用（与设置页同一入口）后重载：provider 出现
    {
        SearchEngineCatalog catalog(path);
        ASSERT_TRUE(catalog.load());
        ASSERT_TRUE(catalog.set_enabled("example", true));
    }
    falcon::search::ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(path));
    const auto providers = manager.get_providers();
    ASSERT_EQ(providers.size(), 1u);
    EXPECT_EQ(providers.front(), "example");
}

} // namespace falcon::desktop
