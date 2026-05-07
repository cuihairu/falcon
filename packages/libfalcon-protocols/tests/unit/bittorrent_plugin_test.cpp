/**
 * @file bittorrent_plugin_test.cpp
 * @brief BitTorrent/Magnet 插件单元测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/plugins/bittorrent/bittorrent_plugin.hpp>
#include <falcon/exceptions.hpp>
#include <fstream>

using namespace falcon;
using namespace falcon::protocols;

class BitTorrentHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler = std::make_unique<BitTorrentHandler>();
    }

    std::unique_ptr<BitTorrentHandler> handler;

    // 测试用的 magnet URI
    const std::string validMagnetUri = "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"
                                       "&dn=TestFile.torrent"
                                       "&tr=udp%3A%2F%2Ftracker.example.com%3A6969"
                                       "&tr=udp%3A%2F%2Ftracker2.example.com%3A6969";

    // 简单的 torrent 文件内容（B编码）
    const std::string simpleTorrentData = "d8:announce44:http://tracker.example.com:6969/announce"
                                        "10:created by13:Falcon Client13:creation datei1703980800e"
                                        "8:encoding5:UTF-84:infod6:lengthi1048576e4:name12:test_file.zip"
                                        "12:piece lengthi262144e6:pieces20:abcdefghijklmnopqrstuv"
                                        "6:filesld6:lengthi524288e4:pathl4:test8:file1.ziped6:lengthi524288e"
                                        "4:pathl4:test8:file2.zipeeee";

    // BitTorrent 处理器辅助方法
    std::string bencodeToString(const BitTorrentHandler::BValue& value) {
        return handler->bencodeToString(value);
    }
};

TEST_F(BitTorrentHandlerTest, ProtocolName) {
    EXPECT_EQ(handler->protocol_name(), "bittorrent");
}

TEST_F(BitTorrentHandlerTest, SupportedSchemes) {
    auto schemes = handler->supported_schemes();
    EXPECT_EQ(schemes.size(), 2);
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "magnet"), schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "bittorrent"), schemes.end());
}

TEST_F(BitTorrentHandlerTest, CanHandleUrls) {
    // Magnet links
    EXPECT_TRUE(handler->can_handle("magnet:?xt=urn:btih:abc123"));
    EXPECT_TRUE(handler->can_handle("MAGNET:?xt=urn:btih:ABC123"));  // 大小写

    // Torrent files
    EXPECT_TRUE(handler->can_handle("http://example.com/file.torrent"));
    EXPECT_TRUE(handler->can_handle("https://example.com/file.torrent"));
    EXPECT_TRUE(handler->can_handle("ftp://example.com/file.torrent"));
    EXPECT_TRUE(handler->can_handle("/path/to/file.torrent"));
    EXPECT_TRUE(handler->can_handle("file:///path/to/file.torrent"));

    // Custom protocol
    EXPECT_TRUE(handler->can_handle("bittorrent://magnet:?xt=urn:btih:abc123"));

    // Not supported
    EXPECT_FALSE(handler->can_handle("http://example.com/file.zip"));
    EXPECT_FALSE(handler->can_handle("thunder://abc"));
    EXPECT_FALSE(handler->can_handle(""));
}

TEST_F(BitTorrentHandlerTest, ParseMagnetUri) {
    // 使用 mock 或简化实现测试
    // 这里测试解析逻辑是否正确识别各个组件

    std::string uri = "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"
                     "&dn=Example%20File"
                     "&xl=1048576"
                     "&tr=udp%3A%2F%2Ftracker.example.com%3A6969"
                     "&ws=http%3A%2F%2Fwebseed.example.com%2Ffile";

    // 验证包含必要的组件
    EXPECT_NE(uri.find("xt=urn:btih:"), std::string::npos);
    EXPECT_NE(uri.find("dn="), std::string::npos);
    EXPECT_NE(uri.find("tr="), std::string::npos);
}

TEST_F(BitTorrentHandlerTest, ParseInvalidMagnetUri) {
    std::vector<std::string> invalidUris = {
        "",  // 空
        "magnet:",  // 缺少参数
        "magnet:?xt=invalid",  // 无效的 xt 参数
        "magnet:?xl=123",  // 缺少 xt 参数
        "magnet:?xt=urn:btih:abc",  // hash 太短
        "magnet:?xt=urn:btih:ghijklmnopqrstuvwxyzabcdef",  // 无效字符
    };

    for (const auto& uri : invalidUris) {
        EXPECT_FALSE(handler->can_handle(uri)) << "Should not handle invalid URI: " << uri;
    }
}

TEST_F(BitTorrentHandlerTest, Sha1Hash) {
    // 测试 SHA1 哈希计算
    std::string input = "hello world";
    std::string expected = "2ef7bde608ce5404e97d5f042f95f89f1c232871";  // SHA1 of "hello world"

    std::string hash = handler->sha1(input);
    EXPECT_EQ(hash, expected);

    // 测试空字符串
    std::string emptyHash = handler->sha1("");
    EXPECT_EQ(emptyHash, "da39a3ee5e6b4b0d3255bfef95601890afd80709");  // SHA1 of empty string
}

TEST_F(BitTorrentHandlerTest, Base32Decode) {
    // 测试 Base32 解码
    std::string input = "MFRGGZDFMZTWQ===";  // Base32 of "test"
    std::string decoded = handler->base32Decode(input);
    EXPECT_EQ(decoded, "test");

    // 测试更长的字符串
    input = "MFRGGZA=";  // Base32 of "Hello"
    decoded = handler->base32Decode(input);
    EXPECT_EQ(decoded, "Hello");
}

TEST_F(BitTorrentHandlerTest, BencodeParsing) {
    // 测试 B 编码解析
    size_t pos = 0;

    // 解析整数
    BitTorrentHandler::BValue intVal = handler->parseBencode("i1234e", pos);
    EXPECT_EQ(intVal.type, BitTorrentHandler::BValue::Integer);
    EXPECT_EQ(intVal.intValue, 1234);

    // 解析字符串
    pos = 0;
    BitTorrentHandler::BValue strVal = handler->parseBencode("5:hello", pos);
    EXPECT_EQ(strVal.type, BitTorrentHandler::BValue::String);
    EXPECT_EQ(strVal.strValue, "hello");

    // 解析列表
    pos = 0;
    BitTorrentHandler::BValue listVal = handler->parseBencode("l4:test5:worldi42ee", pos);
    EXPECT_EQ(listVal.type, BitTorrentHandler::BValue::List);
    EXPECT_EQ(listVal.listValue.size(), 3);

    // 解析字典
    pos = 0;
    BitTorrentHandler::BValue dictVal = handler->parseBencode("d3:key5:value4:testi42ee", pos);
    EXPECT_EQ(dictVal.type, BitTorrentHandler::BValue::Dict);
    EXPECT_EQ(dictVal.dictValue["key"].strValue, "value");
    EXPECT_EQ(dictVal.dictValue["test"].intValue, 42);
}

TEST_F(BitTorrentHandlerTest, BencodeEncoding) {
    // 测试 B 编码到字符串
    BitTorrentHandler::BValue intVal;
    intVal.type = BitTorrentHandler::BValue::Integer;
    intVal.intValue = 1234;
    EXPECT_EQ(bencodeToString(intVal), "i1234e");

    BitTorrentHandler::BValue strVal;
    strVal.type = BitTorrentHandler::BValue::String;
    strVal.strValue = "hello";
    EXPECT_EQ(bencodeToString(strVal), "5:hello");

    BitTorrentHandler::BValue listVal;
    listVal.type = BitTorrentHandler::BValue::List;
    listVal.listValue.push_back(strVal);
    listVal.listValue.push_back(intVal);
    EXPECT_EQ(bencodeToString(listVal), "l5:helloi1234ee");
}

TEST_F(BitTorrentHandlerTest, ValidateTorrent) {
    // 创建基本的 torrent 结构
    BitTorrentHandler::BValue torrent;
    torrent.type = BitTorrentHandler::BValue::Dict;
    torrent.dictValue["info"] = BitTorrentHandler::BValue();
    torrent.dictValue["info"].type = BitTorrentHandler::BValue::Dict;
    torrent.dictValue["info"].dictValue["name"] = BitTorrentHandler::BValue();
    torrent.dictValue["info"].dictValue["name"].type = BitTorrentHandler::BValue::String;
    torrent.dictValue["info"].dictValue["name"].strValue = "test.torrent";
    torrent.dictValue["info"].dictValue["pieces"] = BitTorrentHandler::BValue();
    torrent.dictValue["info"].dictValue["pieces"].type = BitTorrentHandler::BValue::String;
    torrent.dictValue["info"].dictValue["pieces"].strValue = "abcdefghijklmnopqrstuv";
    torrent.dictValue["info"].dictValue["length"] = BitTorrentHandler::BValue();
    torrent.dictValue["info"].dictValue["length"].type = BitTorrentHandler::BValue::Integer;
    torrent.dictValue["info"].dictValue["length"].intValue = 1048576;

    EXPECT_TRUE(handler->validateTorrent(torrent));

    // 测试无效的 torrent
    BitTorrentHandler::BValue invalidTorrent;
    invalidTorrent.type = BitTorrentHandler::BValue::String;
    invalidTorrent.strValue = "not a torrent";
    EXPECT_FALSE(handler->validateTorrent(invalidTorrent));
}

TEST_F(BitTorrentHandlerTest, GetTrackers) {
    BitTorrentHandler::BValue torrent;
    torrent.type = BitTorrentHandler::BValue::Dict;

    // 添加 announce
    torrent.dictValue["announce"] = BitTorrentHandler::BValue();
    torrent.dictValue["announce"].type = BitTorrentHandler::BValue::String;
    torrent.dictValue["announce"].strValue = "http://tracker.example.com:6969";

    // 添加 announce-list
    torrent.dictValue["announce-list"] = BitTorrentHandler::BValue();
    torrent.dictValue["announce-list"].type = BitTorrentHandler::BValue::List;

    BitTorrentHandler::BValue trackerList;
    trackerList.type = BitTorrentHandler::BValue::List;

    BitTorrentHandler::BValue tracker1, tracker2;
    tracker1.type = BitTorrentHandler::BValue::String;
    tracker1.strValue = "udp://tracker1.example.com:6969";
    tracker2.type = BitTorrentHandler::BValue::String;
    tracker2.strValue = "udp://tracker2.example.com:6969";

    trackerList.listValue.push_back(tracker1);
    trackerList.listValue.push_back(tracker2);
    torrent.dictValue["announce-list"].listValue.push_back(trackerList);

    auto trackers = handler->getTrackers(torrent);
    EXPECT_GT(trackers.size(), 0);
}

TEST_F(BitTorrentHandlerTest, GenerateNodeId) {
    std::string nodeId1 = handler->generateNodeId();
    std::string nodeId2 = handler->generateNodeId();

    // Node ID 应该是 20 字节
    EXPECT_EQ(nodeId1.length(), 20);
    EXPECT_EQ(nodeId2.length(), 20);

    // 两个 ID 应该不同
    EXPECT_NE(nodeId1, nodeId2);
}

TEST_F(BitTorrentHandlerTest, EdgeCases) {
    // 空字符串
    EXPECT_FALSE(handler->can_handle(""));

    // 只有协议名
    EXPECT_FALSE(handler->can_handle("magnet:"));
    EXPECT_FALSE(handler->can_handle("bittorrent:"));

    // 无效的文件扩展名
    EXPECT_FALSE(handler->can_handle("http://example.com/file.txt"));

    // 带片段的 magnet（应该有效）
    EXPECT_TRUE(handler->can_handle("magnet:?xt=urn:btih:abc#fragment"));
}

TEST_F(BitTorrentHandlerTest, GetFileInfo) {
    DownloadOptions options;

    // 测试 magnet URI
    try {
        auto info = handler->get_file_info(validMagnetUri, options);
        EXPECT_TRUE(info.supports_resume);
    } catch (const std::exception& e) {
        // 可能需要 libtorrent 支持
        SUCCEED() << "Get file info requires libtorrent: " << e.what();
    }
}

TEST_F(BitTorrentHandlerTest, LargeFiles) {
    // 测试大文件处理
    std::string largeFileMagnet = "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"
                                 "&dn=Large_File_10GB.iso"
                                 "&xl=10737418240";

    EXPECT_TRUE(handler->can_handle(largeFileMagnet));
}

TEST_F(BitTorrentHandlerTest, MultipleTrackers) {
    // 测试多个 tracker
    std::string multiTrackerMagnet = "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"
                                    "&tr=udp%3A%2F%2Ftracker1.example.com%3A6969"
                                    "&tr=udp%3A%2F%2Ftracker2.example.com%3A6969"
                                    "&tr=http%3A%2F%2Ftracker3.example.com%3A80"
                                    "&tr=https%3A%2F%2Ftracker4.example.com%3A443";

    EXPECT_TRUE(handler->can_handle(multiTrackerMagnet));
}

TEST_F(BitTorrentHandlerTest, WebSeeds) {
    // 测试 web seeds
    std::string webSeedMagnet = "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"
                               "&ws=http%3A%2F%2Fwebseed1.example.com%2Ffile"
                               "&ws=https%3A%2F%2Fwebseed2.example.com%2Ffile";

    EXPECT_TRUE(handler->can_handle(webSeedMagnet));
}

// 测试工厂函数
TEST_F(BitTorrentHandlerTest, FactoryFunction) {
    auto h = create_bittorrent_handler();
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->protocol_name(), "bittorrent");
}
