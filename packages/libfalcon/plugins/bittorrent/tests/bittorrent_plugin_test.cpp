/**
 * @file bittorrent_plugin_test.cpp
 * @brief BitTorrent 插件单元测试
 */

#include <gtest/gtest.h>
#include <falcon/plugins/bittorrent/bittorrent_plugin.hpp>
#include <fstream>
#include <sstream>

using namespace falcon::plugins;

class BitTorrentPluginTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin_ = std::make_unique<BitTorrentPlugin>();
    }

    void TearDown() override {
        plugin_.reset();
    }

    std::unique_ptr<BitTorrentPlugin> plugin_;
};

// 测试协议名称
TEST_F(BitTorrentPluginTest, GetProtocolName) {
    EXPECT_EQ(plugin_->getProtocolName(), "bittorrent");
}

// 测试支持的协议方案
TEST_F(BitTorrentPluginTest, GetSupportedSchemes) {
    auto schemes = plugin_->getSupportedSchemes();
    EXPECT_EQ(schemes.size(), 2);
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "magnet") != schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "bittorrent") != schemes.end());
}

// 测试 URL 识别
TEST_F(BitTorrentPluginTest, CanHandle) {
    // Magnet 链接
    EXPECT_TRUE(plugin_->canHandle("magnet:?xt=urn:btih:1234567890123456789012345678901234567890"));

    // Torrent 文件
    EXPECT_TRUE(plugin_->canHandle("http://example.com/file.torrent"));
    EXPECT_TRUE(plugin_->canHandle("/path/to/file.torrent"));
    EXPECT_TRUE(plugin_->canHandle("file:///path/to/file.torrent"));

    // BitTorrent 自定义协议
    EXPECT_TRUE(plugin_->canHandle("bittorrent://magnet:?xt=urn:btih:123456"));

    // 不支持的 URL
    EXPECT_FALSE(plugin_->canHandle("http://example.com/file.zip"));
    EXPECT_FALSE(plugin_->canHandle("ftp://example.com/file.txt"));
}

// 测试 B 编码解析
TEST_F(BitTorrentPluginTest, ParseBencodeInteger) {
    std::string data = "i42e";
    size_t pos = 0;
    auto value = plugin_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentPlugin::BValue::Integer);
    EXPECT_EQ(value.intValue, 42);
    EXPECT_EQ(pos, 4);
}

TEST_F(BitTorrentPluginTest, ParseBencodeString) {
    std::string data = "4:spam";
    size_t pos = 0;
    auto value = plugin_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentPlugin::BValue::String);
    EXPECT_EQ(value.strValue, "spam");
    EXPECT_EQ(pos, 6);
}

TEST_F(BitTorrentPluginTest, ParseBencodeList) {
    std::string data = "l4:spam4:eggse";
    size_t pos = 0;
    auto value = plugin_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentPlugin::BValue::List);
    EXPECT_EQ(value.listValue.size(), 2);
    EXPECT_EQ(value.listValue[0].strValue, "spam");
    EXPECT_EQ(value.listValue[1].strValue, "eggs");
}

TEST_F(BitTorrentPluginTest, ParseBencodeDict) {
    std::string data = "d3:cow3:moo4:spam4:eggse";
    size_t pos = 0;
    auto value = plugin_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentPlugin::BValue::Dict);
    EXPECT_EQ(value.dictValue["cow"].strValue, "moo");
    EXPECT_EQ(value.dictValue["spam"].strValue, "eggs");
}

TEST_F(BitTorrentPluginTest, ParseBencodeNested) {
    std::string data = "d4:spamld4:spam4:eggseee";
    size_t pos = 0;
    auto value = plugin_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentPlugin::BValue::Dict);
    EXPECT_TRUE(value.dictValue.find("spam") != value.dictValue.end());
    EXPECT_EQ(value.dictValue["spam"].type, BitTorrentPlugin::BValue::List);
}

// 测试 Torrent 文件验证
TEST_F(BitTorrentPluginTest, ValidateTorrent) {
    // 最小的有效 torrent 结构
    std::string data = "d6:lengthi123e4:name8:test.txt12:piece lengthi65536e6:pieces20:AAAAAAAAAAAAAAAAAAAAe";

    size_t pos = 0;
    auto torrent = plugin_->parseBencode(data, pos);

    // 基础验证（info 字段）
    EXPECT_TRUE(plugin_->validateTorrent(torrent));
}

TEST_F(BitTorrentPluginTest, ValidateInvalidTorrent) {
    // 无效的 torrent（缺少必需字段）
    std::string data = "d4:name8:test.txte";

    size_t pos = 0;
    auto torrent = plugin_->parseBencode(data, pos);

    EXPECT_FALSE(plugin_->validateTorrent(torrent));
}

// 测试 Magnet URI 解析
TEST_F(BitTorrentPluginTest, ParseMagnetUriValid) {
    std::string magnetUri = "magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=example";

    falcon::DownloadOptions options;
    options.output_path = "/tmp/downloads";

    auto task = dynamic_cast<BitTorrentPlugin::BitTorrentDownloadTask*>(
        plugin_->createTask(magnetUri, options).get()
    );

    ASSERT_NE(task, nullptr);

    // 在没有 libtorrent 的情况下，我们只验证基本解析
#ifdef FALCON_USE_LIBTORRENT
    task->start();
    auto status = task->getStatus();
    EXPECT_NE(status, falcon::TaskStatus::Failed);
#endif
}

// 测试文件选择功能
TEST_F(BitTorrentPluginTest, FileSelection) {
    // 这个测试需要实际的 torrent 文件
    // 创建一个模拟的 torrent
    std::string torrentData = "d"
        "6:lengthi1000000e"
        "4:name8:test.txt"
        "12:piece lengthi262144e"
        "6:pieces20:AAAAAAAAAAAAAAAAAAAA"
        "e";

    std::ofstream file("/tmp/test.torrent", std::ios::binary);
    file << torrentData;
    file.close();

    falcon::DownloadOptions options;
    options.output_path = "/tmp";

    auto task = dynamic_cast<BitTorrentPlugin::BitTorrentDownloadTask*>(
        plugin_->createTask("/tmp/test.torrent", options).get()
    );

    ASSERT_NE(task, nullptr);

#ifdef FALCON_USE_LIBTORRENT
    task->start();
    // 验证文件信息
    EXPECT_GT(task->getTotalBytes(), 0);
#endif

    std::remove("/tmp/test.torrent");
}

// 测试 SHA1 哈希
TEST_F(BitTorrentPluginTest, SHA1Hash) {
    std::string data = "test";
    std::string hash = plugin_->sha1(data);

    // SHA1("test") = "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3"
    EXPECT_EQ(hash, "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3");
}

// 测试节点 ID 生成
TEST_F(BitTorrentPluginTest, GenerateNodeId) {
    std::string nodeId1 = plugin_->generateNodeId();
    std::string nodeId2 = plugin_->generateNodeId();

    EXPECT_EQ(nodeId1.size(), 20);
    EXPECT_EQ(nodeId2.size(), 20);
    EXPECT_NE(nodeId1, nodeId2);  // 应该生成不同的 ID
}

// 性能测试
TEST_F(BitTorrentPluginTest, PerformanceLargeTorrent) {
    // 测试大 torrent 文件的解析性能
    std::string data;
    data += "d4:infod";
    data += "6:lengthi10000000000e";  // 10GB
    data += "4:name20:large_file.bin";
    data += "12:piece lengthi16777216e";  // 16MB pieces
    data += "6:pieces";
    data += "600:";  // 600 pieces * 20 bytes
    for (int i = 0; i < 600; ++i) {
        data += "AAAAAAAAAAAAAAAAAAAA";
    }
    data += "ee";

    size_t pos = 0;
    auto start = std::chrono::high_resolution_clock::now();
    auto torrent = plugin_->parseBencode(data, pos);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 解析应该在合理时间内完成（< 100ms）
    EXPECT_LT(duration.count(), 100);
    EXPECT_TRUE(plugin_->validateTorrent(torrent));
}
