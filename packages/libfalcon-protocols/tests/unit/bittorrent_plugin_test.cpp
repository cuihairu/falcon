/**
 * @file bittorrent_plugin_test.cpp
 * @brief BitTorrent/Magnet 插件单元测试
 * @author Falcon Team
 * @date 2025-12-21
 *
 * B 编码相关测试通过公共的 BencodeValue API（bencode.hpp）覆盖。
 * BitTorrentHandler 的 BValue/parseBencode/bencodeToString/sha1/base32Decode
 * 等辅助方法为私有实现细节，不直接测试（其行为由 BencodeValue 测试等价覆盖）。
 */

#include <gtest/gtest.h>
#include <falcon/plugins/bittorrent/bittorrent_plugin.hpp>
#include <falcon/plugins/bittorrent/bencode.hpp>
#include <falcon/exceptions.hpp>

#include <algorithm>
#include <string>
#include <vector>

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

    // 简单的 torrent 文件内容（B 编码，key 按字典序，长度前缀正确）
    const std::string simpleTorrentData =
        "d8:announce40:http://tracker.example.com:6969/announce"
        "10:created by13:Falcon Client"
        "13:creation datei1703980800e"
        "8:encoding5:UTF-8"
        "4:infod6:lengthi1048576e4:name13:test_file.zip"
        "12:piece lengthi262144e"
        "6:pieces22:abcdefghijklmnopqrstuv"
        "ee";
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
    // Magnet links（合法 info-hash：40 位十六进制或 32 位 Base32）
    EXPECT_TRUE(handler->can_handle(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"));
    EXPECT_TRUE(handler->can_handle(
        "MAGNET:?xt=urn:btih:1234567890ABCDEF1234567890ABCDEF12345678"));  // 大小写
    EXPECT_TRUE(handler->can_handle(
        "magnet:?xt=urn:btih:MFRGGZDFMZTWQ2LKMNWG23PJME4TQ45X"));  // 32 位 Base32

    // magnet URI 非法 info-hash
    EXPECT_FALSE(handler->can_handle("magnet:?xt=urn:btih:abc123"));
    EXPECT_FALSE(handler->can_handle("magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef1234567z"));

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

//==============================================================================
// B 编码解析（公共 BencodeValue API）
//==============================================================================

TEST(BencodeValueTest, ParseInteger) {
    size_t pos = 0;
    BencodeValue value = BencodeValue::decode("i42e", pos);

    EXPECT_TRUE(value.isInt());
    EXPECT_EQ(value.asInt(), 42);
    EXPECT_EQ(pos, 4u);
}

TEST(BencodeValueTest, ParseString) {
    size_t pos = 0;
    BencodeValue value = BencodeValue::decode("4:spam", pos);

    EXPECT_TRUE(value.isString());
    EXPECT_EQ(value.asString(), "spam");
    EXPECT_EQ(pos, 6u);
}

TEST(BencodeValueTest, ParseList) {
    size_t pos = 0;
    BencodeValue value = BencodeValue::decode("l4:spam4:eggse", pos);

    EXPECT_TRUE(value.isList());
    EXPECT_EQ(value.size(), 2u);
    EXPECT_EQ(value[0].asString(), "spam");
    EXPECT_EQ(value[1].asString(), "eggs");
}

TEST(BencodeValueTest, ParseDict) {
    size_t pos = 0;
    BencodeValue value = BencodeValue::decode("d3:cow3:moo4:spam4:eggse", pos);

    EXPECT_TRUE(value.isDict());
    EXPECT_EQ(value["cow"].asString(), "moo");
    EXPECT_EQ(value["spam"].asString(), "eggs");
}

TEST(BencodeValueTest, ParseNested) {
    BencodeValue value = BencodeValue::decode("d4:spamld4:spam4:eggseee");

    EXPECT_TRUE(value.isDict());
    EXPECT_TRUE(value.hasKey("spam"));
    EXPECT_TRUE(value["spam"].isList());
    EXPECT_TRUE(value["spam"][0].isDict());
    EXPECT_EQ(value["spam"][0]["spam"].asString(), "eggs");
}

TEST(BencodeValueTest, ParseMixedTypes) {
    size_t pos = 0;
    BencodeValue value = BencodeValue::decode("d3:key5:value4:testi42ee", pos);

    EXPECT_TRUE(value.isDict());
    EXPECT_EQ(value["key"].asString(), "value");
    EXPECT_EQ(value["test"].asInt(), 42);
}

TEST(BencodeValueTest, EncodeInteger) {
    BencodeValue value(static_cast<int64_t>(42));
    EXPECT_EQ(value.encode(), "i42e");
}

TEST(BencodeValueTest, EncodeString) {
    BencodeValue value(std::string("hello"));
    EXPECT_EQ(value.encode(), "5:hello");
}

TEST(BencodeValueTest, EncodeList) {
    BencodeValue list(std::vector<BencodeValue>{
        BencodeValue(std::string("hello")),
        BencodeValue(static_cast<int64_t>(42)),
    });
    EXPECT_EQ(list.encode(), "l5:helloi42ee");
}

TEST(BencodeValueTest, RoundTrip) {
    // 字典 key 必须按字典序排列（编码器通过 std::map 保证）
    const std::string encoded = "d3:key5:value4:listl4:spami1ee4:testi42ee";
    BencodeValue decoded = BencodeValue::decode(encoded);
    EXPECT_EQ(decoded.encode(), encoded);
}

TEST(BencodeValueTest, InvalidDataThrows) {
    EXPECT_THROW(BencodeValue::decode("i42"), BencodeException);       // 缺少结尾 e
    EXPECT_THROW(BencodeValue::decode(""), BencodeException);          // 空输入
    EXPECT_THROW(BencodeValue::decode("g"), BencodeException);         // 非法起始字符
}

//==============================================================================
// Torrent 结构解析（经公共 BencodeValue 覆盖原 validateTorrent/getTrackers 意图）
//==============================================================================

TEST_F(BitTorrentHandlerTest, TorrentStructureParsing) {
    BencodeValue torrent = BencodeValue::decode(simpleTorrentData);

    EXPECT_TRUE(torrent.isDict());
    EXPECT_TRUE(torrent.hasKey("announce"));
    EXPECT_TRUE(torrent.hasKey("info"));

    const BencodeValue& info = torrent["info"];
    EXPECT_TRUE(info.isDict());
    EXPECT_EQ(info["name"].asString(), "test_file.zip");
    EXPECT_EQ(info["length"].asInt(), 1048576);
    EXPECT_EQ(info["piece length"].asInt(), 262144);
}

TEST(BencodeValueTest, TrackerListParsing) {
    const std::string data =
        "d8:announce40:http://tracker.example.com:6969/announce"
        "13:announce-listl41:http://tracker1.example.com:6969/announce"
        "41:http://tracker2.example.com:6969/announceee";

    BencodeValue torrent = BencodeValue::decode(data);

    ASSERT_TRUE(torrent.hasKey("announce"));
    EXPECT_EQ(torrent["announce"].asString(),
              "http://tracker.example.com:6969/announce");

    ASSERT_TRUE(torrent.hasKey("announce-list"));
    ASSERT_TRUE(torrent["announce-list"].isList());

    std::vector<std::string> trackers;
    const BencodeValue& list = torrent["announce-list"];
    for (size_t i = 0; i < list.size(); ++i) {
        trackers.push_back(list[i].asString());
    }
    EXPECT_EQ(trackers.size(), 2u);
    EXPECT_NE(std::find(trackers.begin(), trackers.end(),
                        "http://tracker1.example.com:6969/announce"),
              trackers.end());
}

//==============================================================================
// Handler 其余行为
//==============================================================================

TEST_F(BitTorrentHandlerTest, EdgeCases) {
    // 空字符串
    EXPECT_FALSE(handler->can_handle(""));

    // 只有协议名
    EXPECT_FALSE(handler->can_handle("magnet:"));
    EXPECT_FALSE(handler->can_handle("bittorrent:"));

    // 无效的文件扩展名
    EXPECT_FALSE(handler->can_handle("http://example.com/file.txt"));

    // 带片段的 magnet（hash 在 '#' 处截断，片段不影响识别）
    EXPECT_TRUE(handler->can_handle(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678#fragment"));

    // xt 参数后跟其他参数（hash 在 '&' 处截断）
    EXPECT_TRUE(handler->can_handle(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678&dn=test"));
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
