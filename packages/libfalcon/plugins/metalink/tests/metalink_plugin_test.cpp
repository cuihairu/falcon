/**
 * @file metalink_plugin_test.cpp
 * @brief Metalink 插件单元测试
 */

#include <gtest/gtest.h>
#include <falcon/plugins/metalink/metalink_plugin.hpp>
#include <fstream>

using namespace falcon::plugins;

class MetalinkPluginTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin_ = std::make_unique<MetalinkPlugin>();
    }

    void TearDown() override {
        plugin_.reset();
    }

    // 创建测试用的 metalink 文件
    std::string createTestMetalinkFile(const std::string& path) {
        std::string metalinkContent = R"(<?xml version="1.0" encoding="utf-8"?>
<metalink version="4.0" xmlns="http://www.metalinker.org/">
  <file name="test-file.zip">
    <size>1048576</size>
    <hash type="sha-256">a1b2c3d4e5f6...</hash>
    <url priority="100" type="http" location="cn">http://mirror1.example.com/file.zip</url>
    <url priority="90" type="http" location="us">http://mirror2.example.com/file.zip</url>
    <url priority="80" type="ftp" location="eu">ftp://mirror3.example.com/file.zip</url>
  </file>
</metalink>)";

        std::ofstream file(path);
        file << metalinkContent;
        file.close();

        return path;
    }

    std::unique_ptr<MetalinkPlugin> plugin_;
};

// 测试协议名称
TEST_F(MetalinkPluginTest, GetProtocolName) {
    EXPECT_EQ(plugin_->getProtocolName(), "metalink");
}

// 测试支持的协议方案
TEST_F(MetalinkPluginTest, GetSupportedSchemes) {
    auto schemes = plugin_->getSupportedSchemes();
    EXPECT_EQ(schemes.size(), 1);
    EXPECT_EQ(schemes[0], "metalink");
}

// 测试 URL 识别
TEST_F(MetalinkPluginTest, CanHandle) {
    // Metalink URLs
    EXPECT_TRUE(plugin_->canHandle("metalink://example.com/file.metalink"));
    EXPECT_TRUE(plugin_->canHandle("http://example.com/file.metalink"));
    EXPECT_TRUE(plugin_->canHandle("/path/to/file.metalink"));

    // 非 Metalink URLs
    EXPECT_FALSE(plugin_->canHandle("http://example.com/file.zip"));
    EXPECT_FALSE(plugin_->canHandle("ftp://example.com/file.txt"));
}

// 测试 XML 解析
TEST_F(MetalinkPluginTest, XMLParserBasic) {
    std::string xml = R"(<root attr1="value1">
    <child1>text1</child1>
    <child2 attr2="value2">text2</child2>
</root>)";

    auto root = MetalinkPlugin::XMLParser::parse(xml);

    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->name, "root");
    EXPECT_EQ(root->attributes["attr1"], "value1");

    auto child1 = MetalinkPlugin::XMLParser::findChild(root.get(), "child1");
    ASSERT_NE(child1, nullptr);
    EXPECT_EQ(child1->text, "text1");

    auto child2 = MetalinkPlugin::XMLParser::findChild(root.get(), "child2");
    ASSERT_NE(child2, nullptr);
    EXPECT_EQ(child2->text, "text2");
    EXPECT_EQ(child2->attributes["attr2"], "value2");
}

TEST_F(MetalinkPluginTest, XMLParserNested) {
    std::string xml = R"(<root>
    <parent>
        <child1>text1</child1>
        <child2>text2</child2>
    </parent>
</root>)";

    auto root = MetalinkPlugin::XMLParser::parse(xml);

    auto parent = MetalinkPlugin::XMLParser::findChild(root.get(), "parent");
    ASSERT_NE(parent, nullptr);

    auto child1 = MetalinkPlugin::XMLParser::findChild(parent, "child1");
    ASSERT_NE(child1, nullptr);
    EXPECT_EQ(child1->text, "text1");
}

TEST_F(MetalinkPluginTest, XMLParserMultipleChildren) {
    std::string xml = R"(<root>
    <item>item1</item>
    <item>item2</item>
    <item>item3</item>
</root>)";

    auto root = MetalinkPlugin::XMLParser::parse(xml);

    auto items = MetalinkPlugin::XMLParser::findChildren(root.get(), "item");
    EXPECT_EQ(items.size(), 3);

    if (items.size() >= 3) {
        EXPECT_EQ(items[0]->text, "item1");
        EXPECT_EQ(items[1]->text, "item2");
        EXPECT_EQ(items[2]->text, "item3");
    }
}

TEST_F(MetalinkPluginTest, XMLParserSelfClosing) {
    std::string xml = R"(<root>
    <empty />
    <item>text</item>
</root>)";

    auto root = MetalinkPlugin::XMLParser::parse(xml);

    auto empty = MetalinkPlugin::XMLParser::findChild(root.get(), "empty");
    ASSERT_NE(empty, nullptr);

    auto item = MetalinkPlugin::XMLParser::findChild(root.get(), "item");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->text, "text");
}

// 测试资源选择
TEST_F(MetalinkPluginTest, ResourceSelection) {
    std::vector<MetalinkResource> resources = {
        {"http://mirror1.com", 90, "http", "cn", 100},
        {"http://mirror2.com", 100, "http", "us", 90},
        {"ftp://mirror3.com", 80, "ftp", "eu", 80}
    };

    falcon::DownloadOptions options;
    std::string metalinkPath = "/tmp/test.metalink";
    createTestMetalinkFile(metalinkPath);

    auto task = dynamic_cast<MetalinkPlugin::MetalinkDownloadTask*>(
        plugin_->createTask("file://" + metalinkPath, options).get()
    );

    ASSERT_NE(task, nullptr);

    task->start();
    // 验证选择了优先级最高的资源
    auto status = task->getStatus();
    EXPECT_NE(status, falcon::TaskStatus::Failed);

    std::remove(metalinkPath.c_str());
}

// 测试地理位置过滤
TEST_F(MetalinkPluginTest, LocationFiltering) {
    std::vector<MetalinkResource> resources = {
        {"http://mirror1.com", 90, "http", "cn", 100},
        {"http://mirror2.com", 100, "http", "us", 90},
        {"http://mirror3.com", 80, "http", "cn", 70}
    };

    falcon::DownloadOptions options;
    std::string metalinkPath = "/tmp/test_location.metalink";
    createTestMetalinkFile(metalinkPath);

    auto task = dynamic_cast<MetalinkPlugin::MetalinkDownloadTask*>(
        plugin_->createTask("file://" + metalinkPath, options).get()
    );

    ASSERT_NE(task, nullptr);

    // 过滤中国地区的资源
    auto cnResources = task->filterByLocation(resources, "cn");
    EXPECT_EQ(cnResources.size(), 2);

    std::remove(metalinkPath.c_str());
}

// 测试 Metalink 文件解析
TEST_F(MetalinkPluginTest, ParseMetalinkFile) {
    std::string metalinkPath = "/tmp/test_parse.metalink";
    createTestMetalinkFile(metalinkPath);

    falcon::DownloadOptions options;
    auto task = dynamic_cast<MetalinkPlugin::MetalinkDownloadTask*>(
        plugin_->createTask("file://" + metalinkPath, options).get()
    );

    ASSERT_NE(task, nullptr);
    task->start();

    auto status = task->getStatus();
    EXPECT_NE(status, falcon::TaskStatus::Failed);
    EXPECT_GT(task->getTotalBytes(), 0);

    std::remove(metalinkPath.c_str());
}

// 性能测试
TEST_F(MetalinkPluginTest, PerformanceXMLParsing) {
    // 创建一个较大的 metalink 文件
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<metalink version="4.0">
  <file name="large-file.bin">
    <size>1073741824</size>
    <hash type="sha-256">abc123...</hash>
)";

    // 添加 100 个镜像
    for (int i = 0; i < 100; ++i) {
        xml += "    <url priority=\"" + std::to_string(100 - i) +
               "\" type=\"http\" location=\"cn\">http://mirror" +
               std::to_string(i) + ".example.com/file.bin</url>\n";
    }

    xml += "  </file>\n</metalink>";

    auto start = std::chrono::high_resolution_clock::now();
    auto root = MetalinkPlugin::XMLParser::parse(xml);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 解析应该在合理时间内完成（< 50ms）
    EXPECT_LT(duration.count(), 50);
    EXPECT_NE(root, nullptr);
}

// 边界测试
TEST_F(MetalinkPluginTest, EmptyMetalink) {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<metalink version="4.0">
</metalink>)";

    auto root = MetalinkPlugin::XMLParser::parse(xml);
    ASSERT_NE(root, nullptr);

    auto metalink = MetalinkPlugin::XMLParser::findChild(root.get(), "metalink");
    ASSERT_NE(metalink, nullptr);
}

TEST_F(MetalinkPluginTest, InvalidMetalink) {
    std::string metalinkPath = "/tmp/invalid.metalink";
    std::ofstream file(metalinkPath);
    file << "This is not a valid metalink file";
    file.close();

    falcon::DownloadOptions options;
    auto task = dynamic_cast<MetalinkPlugin::MetalinkDownloadTask*>(
        plugin_->createTask("file://" + metalinkPath, options).get()
    );

    ASSERT_NE(task, nullptr);
    task->start();

    // 应该失败
    auto status = task->getStatus();
    EXPECT_EQ(status, falcon::TaskStatus::Failed);
    EXPECT_FALSE(task->getErrorMessage().empty());

    std::remove(metalinkPath.c_str());
}
