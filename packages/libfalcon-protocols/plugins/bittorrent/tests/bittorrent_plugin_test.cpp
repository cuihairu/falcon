/**
 * @file bittorrent_plugin_test.cpp
 * @brief BitTorrent 插件单元测试
 */

#include <gtest/gtest.h>
#include <falcon/plugins/bittorrent/bittorrent_plugin.hpp>
#include <fstream>
#include <sstream>

using namespace falcon;
using namespace falcon::protocols;

class BitTorrentHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler_ = std::make_unique<BitTorrentHandler>();
    }

    void TearDown() override {
        handler_.reset();
    }

    std::unique_ptr<BitTorrentHandler> handler_;
};

// 测试协议名称
TEST_F(BitTorrentHandlerTest, ProtocolName) {
    EXPECT_EQ(handler_->protocol_name(), "bittorrent");
}

// 测试支持的协议方案
TEST_F(BitTorrentHandlerTest, SupportedSchemes) {
    auto schemes = handler_->supported_schemes();
    EXPECT_EQ(schemes.size(), 2);
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "magnet") != schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "bittorrent") != schemes.end());
}

// 测试 URL 识别
TEST_F(BitTorrentHandlerTest, CanHandle) {
    // Magnet 链接
    EXPECT_TRUE(handler_->can_handle("magnet:?xt=urn:btih:1234567890123456789012345678901234567890"));

    // Torrent 文件
    EXPECT_TRUE(handler_->can_handle("http://example.com/file.torrent"));
    EXPECT_TRUE(handler_->can_handle("/path/to/file.torrent"));
    EXPECT_TRUE(handler_->can_handle("file:///path/to/file.torrent"));

    // BitTorrent 自定义协议
    EXPECT_TRUE(handler_->can_handle("bittorrent://magnet:?xt=urn:btih:123456"));

    // 不支持的 URL
    EXPECT_FALSE(handler_->can_handle("http://example.com/file.zip"));
    EXPECT_FALSE(handler_->can_handle("ftp://example.com/file.txt"));
}

// 测试 B 编码解析
TEST_F(BitTorrentHandlerTest, ParseBencodeInteger) {
    std::string data = "i42e";
    size_t pos = 0;
    auto value = handler_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentHandler::BValue::Integer);
    EXPECT_EQ(value.intValue, 42);
    EXPECT_EQ(pos, 4);
}

TEST_F(BitTorrentHandlerTest, ParseBencodeString) {
    std::string data = "4:spam";
    size_t pos = 0;
    auto value = handler_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentHandler::BValue::String);
    EXPECT_EQ(value.strValue, "spam");
    EXPECT_EQ(pos, 6);
}

TEST_F(BitTorrentHandlerTest, ParseBencodeList) {
    std::string data = "l4:spam4:eggse";
    size_t pos = 0;
    auto value = handler_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentHandler::BValue::List);
    EXPECT_EQ(value.listValue.size(), 2);
    EXPECT_EQ(value.listValue[0].strValue, "spam");
    EXPECT_EQ(value.listValue[1].strValue, "eggs");
}

TEST_F(BitTorrentHandlerTest, ParseBencodeDict) {
    std::string data = "d3:cow3:moo4:spam4:eggse";
    size_t pos = 0;
    auto value = handler_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentHandler::BValue::Dict);
    EXPECT_EQ(value.dictValue["cow"].strValue, "moo");
    EXPECT_EQ(value.dictValue["spam"].strValue, "eggs");
}

TEST_F(BitTorrentHandlerTest, ParseBencodeNested) {
    std::string data = "d4:spamld4:spam4:eggseee";
    size_t pos = 0;
    auto value = handler_->parseBencode(data, pos);

    EXPECT_EQ(value.type, BitTorrentHandler::BValue::Dict);
    EXPECT_TRUE(value.dictValue.find("spam") != value.dictValue.end());
    EXPECT_EQ(value.dictValue["spam"].type, BitTorrentHandler::BValue::List);
}

// 测试 Torrent 文件验证
TEST_F(BitTorrentHandlerTest, ValidateTorrent) {
    // 最小的有效 torrent 结构
    std::string data = "d6:lengthi123e4:name8:test.txt12:piece lengthi65536e6:pieces20:AAAAAAAAAAAAAAAAAAAAe";

    size_t pos = 0;
    auto torrent = handler_->parseBencode(data, pos);

    // 基础验证（info 字段）
    EXPECT_TRUE(handler_->validateTorrent(torrent));
}

TEST_F(BitTorrentHandlerTest, ValidateInvalidTorrent) {
    // 无效的 torrent（缺少必需字段）
    std::string data = "d4:name8:test.txte";

    size_t pos = 0;
    auto torrent = handler_->parseBencode(data, pos);

    EXPECT_FALSE(handler_->validateTorrent(torrent));
}

// 测试获取文件信息
TEST_F(BitTorrentHandlerTest, GetFileInfoMagnet) {
    std::string magnetUri = "magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=example";

    DownloadOptions options;

#ifdef FALCON_USE_LIBTORRENT
    auto info = handler_->get_file_info(magnetUri, options);
    EXPECT_EQ(info.filename, "example");
    EXPECT_TRUE(info.supports_resume);
#else
    // 纯 C++ 模式可能不支持完整解析
    EXPECT_TRUE(handler_->can_handle(magnetUri));
#endif
}

// 测试 SHA1 哈希
TEST_F(BitTorrentHandlerTest, SHA1Hash) {
    std::string data = "test";
    std::string hash = handler_->sha1(data);

    // SHA1("test") = "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3"
    EXPECT_EQ(hash, "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3");
}

// 测试节点 ID 生成
TEST_F(BitTorrentHandlerTest, GenerateNodeId) {
    std::string nodeId1 = handler_->generateNodeId();
    std::string nodeId2 = handler_->generateNodeId();

    EXPECT_EQ(nodeId1.size(), 20);
    EXPECT_EQ(nodeId2.size(), 20);
    EXPECT_NE(nodeId1, nodeId2);  // 应该生成不同的 ID
}

// 性能测试
TEST_F(BitTorrentHandlerTest, PerformanceLargeTorrent) {
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
    auto torrent = handler_->parseBencode(data, pos);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 解析应该在合理时间内完成（< 100ms）
    EXPECT_LT(duration.count(), 100);
    EXPECT_TRUE(handler_->validateTorrent(torrent));
}

// 测试 Base32 解码
TEST_F(BitTorrentHandlerTest, Base32Decode) {
    std::string input = "MFRGGZDFMZTWQ===";  // Base32 of "test"
    std::string decoded = handler_->base32Decode(input);
    EXPECT_EQ(decoded, "test");
}

// 测试 URL 解码
TEST_F(BitTorrentHandlerTest, UrlDecode) {
    std::string encoded = "hello%20world%2Btest";
    std::string decoded = handler_->urlDecode(encoded);
    EXPECT_EQ(decoded, "hello world+test");
}

// 测试 B 编码到字符串转换
TEST_F(BitTorrentHandlerTest, BencodeToString) {
    BitTorrentHandler::BValue intVal;
    intVal.type = BitTorrentHandler::BValue::Integer;
    intVal.intValue = 42;
    EXPECT_EQ(handler_->bencodeToString(intVal), "i42e");

    BitTorrentHandler::BValue strVal;
    strVal.type = BitTorrentHandler::BValue::String;
    strVal.strValue = "hello";
    EXPECT_EQ(handler_->bencodeToString(strVal), "5:hello");

    BitTorrentHandler::BValue listVal;
    listVal.type = BitTorrentHandler::BValue::List;
    listVal.listValue.push_back(strVal);
    listVal.listValue.push_back(intVal);
    EXPECT_EQ(handler_->bencodeToString(listVal), "l5:helloi42ee");
}

// 测试获取 tracker 列表
TEST_F(BitTorrentHandlerTest, GetTrackers) {
    std::string data = "d8:announce44:http://tracker.example.com:6969/announce"
                       "13:announce-listld44:http://tracker1.example.com:6969/announce"
                       "44:http://tracker2.example.com:6969/announceee";

    size_t pos = 0;
    auto torrent = handler_->parseBencode(data, pos);

    auto trackers = handler_->getTrackers(torrent);
    EXPECT_GE(trackers.size(), 1);
    EXPECT_FALSE(std::find(trackers.begin(), trackers.end(),
                           "http://tracker.example.com:6969/announce") == trackers.end());
}

// 测试工厂函数
TEST_F(BitTorrentHandlerTest, FactoryFunction) {
    auto handler = create_bittorrent_handler();
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->protocol_name(), "bittorrent");
}
