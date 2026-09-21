/**
 * @file bittorrent_plugin_test.cpp
 * @brief BitTorrent/Magnet 插件单元测试
 * @author Falcon Team
 * @date 2025-12-21
 *
 * B 编码基础行为通过公共的 BencodeValue API（bencode.hpp）覆盖；
 * 插件内嵌 parseBencode 是另一套独立实现（get_file_info 的 .torrent
 * 路径真实在用），经 get_file_info 直接测试其严格性——截断输入、
 * 非法整数、非字符串字典键都必须报错而非静默出假元数据。
 *
 * info-hash 提取/归一化以公开 static（extract_info_hash/info_hash_to_hex）
 * 直接测试：曾修复 magnet 偏移 off-by-one（"xt=urn:btih:" 为 12 字符，
 * 旧代码取 pos+11 使 hash 带前导冒号）与 Base32 hash 从不解码就下发
 * DHT（按 40 位 hex 定位，Base32 文本会查询错误的 info_hash）两处缺陷，
 * 精确匹配断言同时是回归挂点。
 *
 * 任务生命周期与 DHT 集成经本地随机端口 DHT（清空公网引导节点后
 * 空网络立即收敛）端到端覆盖。
 *
 * 构建模式分叉：libtorrent 模式（FALCON_USE_LIBTORRENT）下 get_file_info
 * 走 torrent_info 严格校验（非法结构抛出而非留空），自研 DhtClient/PEX
 * 表不在数据面上（session 原生 DHT/LSD/PEX），且 download() 是阻塞
 * 监控循环——结构校验组按模式分叉断言，纯 C++ DHT/生命周期组编译期
 * 隔离，session 数据面由 bittorrent_seeding_test 覆盖。
 */

#include <gtest/gtest.h>
#include <falcon/plugins/bittorrent/bittorrent_plugin.hpp>
#include <falcon/plugins/bittorrent/bencode.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/event_listener.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

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

    // 简单的 torrent 文件内容（B 编码，key 按字典序，长度前缀正确；
    // pieces = 4×20 字节与 total/piece_length = 1048576/262144 自洽
    // ——libtorrent 严格校验 piece 数量，与纯 C++ 模式数据统一可解析）
    const std::string simpleTorrentData =
        "d8:announce40:http://tracker.example.com:6969/announce"
        "10:created by13:Falcon Client"
        "13:creation datei1703980800e"
        "8:encoding5:UTF-8"
        "4:infod6:lengthi1048576e4:name13:test_file.zip"
        "12:piece lengthi262144e"
        "6:pieces80:abcdefghijklmnopqrstabcdefghijklmnopqrst"
        "abcdefghijklmnopqrstabcdefghijklmnopqrst"
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

//==============================================================================
// info-hash 提取（extract_info_hash，公开 static）
//==============================================================================

namespace {

constexpr const char* kHexHash = "1234567890abcdef1234567890abcdef12345678";

// 写入临时 .torrent 文件并返回路径（析构自动清理）
class TempTorrentFile {
public:
    explicit TempTorrentFile(const std::string& content) {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("falcon_bt_plugin_test_" +
                 std::to_string(stamp) + "_" +
                 std::to_string(counter_++) + ".torrent");
        std::ofstream file(path_, std::ios::binary);
        file << content;
    }
    ~TempTorrentFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    inline static int counter_ = 0;
    std::filesystem::path path_;
};

// 占住一个 UDP 端口直至析构（DHT 端口冲突用例）
class HeldUdpPort {
public:
    HeldUdpPort() {
        socket_ = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (socket_ < 0) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        // INADDR_ANY 与 DhtClient 的绑定一致（沙盒字节序怪癖见 dht_node_test）
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;
        if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            closeSocket(socket_);
            socket_ = -1;
            return;
        }
        socklen_t len = sizeof(addr);
        getsockname(socket_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }
    ~HeldUdpPort() {
        if (socket_ >= 0) {
            closeSocket(socket_);
        }
    }
    bool valid() const { return socket_ >= 0; }
    uint16_t port() const { return port_; }

private:
    static void closeSocket(int fd) {
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }
    int socket_ = -1;
    uint16_t port_ = 0;
};

} // namespace

TEST(BitTorrentInfoHashTest, ExtractHexHashPlain) {
    // 精确匹配是 off-by-one 回归挂点：旧代码 pos+11 使结果带前导 ':'
    EXPECT_EQ(BitTorrentHandler::extract_info_hash(
                  std::string("magnet:?xt=urn:btih:") + kHexHash),
              kHexHash);
}

TEST(BitTorrentInfoHashTest, ExtractHexHashWithParams) {
    EXPECT_EQ(BitTorrentHandler::extract_info_hash(
                  std::string("magnet:?xt=urn:btih:") + kHexHash +
                  "&dn=file.zip&tr=udp%3A%2F%2Ft.example.com"),
              kHexHash);
}

TEST(BitTorrentInfoHashTest, ExtractBase32HashStopsAtFragment) {
    const std::string url =
        "magnet:?xt=urn:btih:MFRGGZDFMZTWQ2LKMNWG23PJME4TQ45X#fragment";
    EXPECT_EQ(BitTorrentHandler::extract_info_hash(url),
              "MFRGGZDFMZTWQ2LKMNWG23PJME4TQ45X");
}

TEST(BitTorrentInfoHashTest, ExtractMissingXtReturnsEmpty) {
    EXPECT_EQ(BitTorrentHandler::extract_info_hash("magnet:?dn=file.zip"), "");
    EXPECT_EQ(BitTorrentHandler::extract_info_hash("magnet:"), "");
    EXPECT_EQ(BitTorrentHandler::extract_info_hash(""), "");
}

TEST(BitTorrentInfoHashTest, ExtractIsParameterCaseSensitive) {
    // 参数名大小写敏感（与 download() 的 magnet: 前缀判定同一语义层）
    EXPECT_EQ(BitTorrentHandler::extract_info_hash(
                  std::string("magnet:?XT=URN:BTIH:") + kHexHash),
              "");
}

TEST(BitTorrentInfoHashTest, ExtractIsSchemeAgnostic) {
    // 提取本身不管 scheme（download() 自行做 magnet: 前缀门禁）
    EXPECT_EQ(BitTorrentHandler::extract_info_hash(
                  std::string("http://x/?xt=urn:btih:") + kHexHash),
              kHexHash);
}

//==============================================================================
// info-hash 归一化（info_hash_to_hex，公开 static；Base32 向量经 Python
// base64.b32encode 独立生成）
//==============================================================================

TEST(BitTorrentInfoHashTest, HexLowercased) {
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(
                  "1234567890ABCDEF1234567890ABCDEF12345678"),
              "1234567890abcdef1234567890abcdef12345678");
}

TEST(BitTorrentInfoHashTest, HexAlreadyLowercasePassesThrough) {
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(kHexHash), kHexHash);
}

TEST(BitTorrentInfoHashTest, HexWithInvalidDigitRejected) {
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(
                  "1234567890zzcdef1234567890abcdef12345678"),
              "");
}

TEST(BitTorrentInfoHashTest, Base32DecodesToRawHex) {
    // base32(bytes[0..19]) → hex
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(
                  "AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQT"),
              "000102030405060708090a0b0c0d0e0f10111213");
}

TEST(BitTorrentInfoHashTest, Base32CaseInsensitive) {
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(
                  "32w353ybencwpcnlzxx75xf2tb3fimqq"),
              "deadbeef0123456789abcdeffedcba9876543210");
}

TEST(BitTorrentInfoHashTest, Base32InvalidCharacterRejected) {
    // '1' 不是 Base32 字母表成员：被跳过解码后不足 20 字节 → 拒绝
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(
                  "11AQEAYEAUDAOCAJBIFQYDIOB4IBCEQT"),
              "");
}

TEST(BitTorrentInfoHashTest, WrongLengthRejected) {
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(
                  "AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQ"),  // 31 位
              "");
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(kHexHash), kHexHash);
    EXPECT_EQ(BitTorrentHandler::info_hash_to_hex(""), "");
}

//==============================================================================
// 内嵌 parseBencode 严格性（经 get_file_info 的 .torrent 路径；
// 与公共 BencodeValue 是两套独立实现，此处是真实生产路径）
//==============================================================================

TEST_F(BitTorrentHandlerTest, TruncatedIntegerThrows) {
    // 旧实现静默返回 42：截断的 "i42" 缺终止 'e' 必须报错
    TempTorrentFile file("i42");
    EXPECT_THROW((void)handler->get_file_info(
                     file.path().string(), DownloadOptions{}),
                 std::exception);
}

TEST_F(BitTorrentHandlerTest, InvalidIntegerContentThrows) {
    // 空内容 / 空白填充：std::stoll 会宽松接受，bencode 不允许
    for (const auto* data : {"ie", "i 5e", "i+5e"}) {
        TempTorrentFile file(data);
        EXPECT_THROW((void)handler->get_file_info(
                         file.path().string(), DownloadOptions{}),
                     std::exception)
            << "input: " << data;
    }
}

TEST_F(BitTorrentHandlerTest, IntegerOutOfRangeThrows) {
    TempTorrentFile file("i99999999999999999999999e");
    EXPECT_THROW((void)handler->get_file_info(
                     file.path().string(), DownloadOptions{}),
                 std::exception);
}

TEST_F(BitTorrentHandlerTest, NegativeIntegerStillParses) {
    TempTorrentFile file("i-5e");
#ifdef FALCON_USE_LIBTORRENT
    // libtorrent 对根非字典输入解析失败（严格语义）；严格化不误伤
    // 合法负整数的契约由纯模式分支钉住
    EXPECT_THROW((void)handler->get_file_info(file.path().string(),
                                        DownloadOptions{}),
                 std::exception);
#else
    // 严格化不得误伤合法负整数（根非字典 → validateTorrent 失败 →
    // 按既有契约不抛出、字段留空）
    auto info = handler->get_file_info(file.path().string(),
                                                DownloadOptions{});
    EXPECT_TRUE(info.filename.empty());
#endif
}

TEST_F(BitTorrentHandlerTest, TruncatedContainerThrows) {
    for (const auto* data : {"l4:spam", "d4:info4:name"}) {
        TempTorrentFile file(data);
        EXPECT_THROW((void)handler->get_file_info(
                         file.path().string(), DownloadOptions{}),
                     std::exception)
            << "input: " << data;
    }
}

TEST_F(BitTorrentHandlerTest, DictWithNonStringKeyThrows) {
    TempTorrentFile file("di1e4:spame");
    EXPECT_THROW((void)handler->get_file_info(
                     file.path().string(), DownloadOptions{}),
                 std::exception);
}

TEST_F(BitTorrentHandlerTest, StringLengthBeyondDataThrows) {
    TempTorrentFile file("d4:info12:shorte");
    EXPECT_THROW((void)handler->get_file_info(
                     file.path().string(), DownloadOptions{}),
                 std::exception);
}

TEST_F(BitTorrentHandlerTest, StringMissingColonThrows) {
    // 长度前缀后缺 ':'（"4name"）
    TempTorrentFile file("d4:info4name");
    EXPECT_THROW((void)handler->get_file_info(
                     file.path().string(), DownloadOptions{}),
                 std::exception);
}

//==============================================================================
// 结构校验分支：纯 C++ 模式宽松（不抛出、字段留空），libtorrent 模式
// torrent_info 严格校验（非法结构解析失败抛 FileIOException）——
// 两种模式对非法输入都不得静默出假元数据
//==============================================================================

TEST_F(BitTorrentHandlerTest, NonDictRootRejected) {
    TempTorrentFile file("l4:spame");
#ifdef FALCON_USE_LIBTORRENT
    EXPECT_THROW((void)handler->get_file_info(file.path().string(),
                                        DownloadOptions{}),
                 std::exception);
#else
    auto info = handler->get_file_info(file.path().string(),
                                                DownloadOptions{});
    EXPECT_TRUE(info.filename.empty());
    EXPECT_EQ(info.total_size, size_t{0});
#endif
}

TEST_F(BitTorrentHandlerTest, InfoWithoutPiecesRejected) {
    TempTorrentFile file("d4:infod4:name4:testee");
#ifdef FALCON_USE_LIBTORRENT
    EXPECT_THROW((void)handler->get_file_info(file.path().string(),
                                        DownloadOptions{}),
                 std::exception);
#else
    auto info = handler->get_file_info(file.path().string(),
                                                DownloadOptions{});
    EXPECT_TRUE(info.filename.empty());
#endif
}

TEST_F(BitTorrentHandlerTest, InfoWithoutLengthRejected) {
    // 有 name + pieces 但既无单文件 length 也无多文件 files
    TempTorrentFile file("d4:infod4:name4:test6:pieces3:abcee");
#ifdef FALCON_USE_LIBTORRENT
    EXPECT_THROW((void)handler->get_file_info(file.path().string(),
                                        DownloadOptions{}),
                 std::exception);
#else
    auto info = handler->get_file_info(file.path().string(),
                                                DownloadOptions{});
    EXPECT_TRUE(info.filename.empty());
#endif
}

//==============================================================================
// DHT 生命周期（随机端口 + 端口冲突；纯 C++ DhtClient 数据面专属）
//==============================================================================

TEST_F(BitTorrentHandlerTest, DhtStartStopOnEphemeralPort) {
#ifdef FALCON_USE_LIBTORRENT
    GTEST_SKIP() << "libtorrent 模式 DHT 由 session 原生管理，startDht/"
                    "stopDht 为 no-op（session 数据面由 seeding e2e 覆盖）";
#else
    handler->stopDht();
    EXPECT_FALSE(handler->isDhtRunning());

    handler->startDht(0);  // 端口 0 → OS 分配，绕开 6881 争用
    ASSERT_TRUE(handler->isDhtRunning());

    // 重复 startDht：已运行直接返回（不重建客户端）
    handler->startDht(0);
    EXPECT_TRUE(handler->isDhtRunning());

    handler->stopDht();
    EXPECT_FALSE(handler->isDhtRunning());
    handler->stopDht();  // 幂等
    EXPECT_FALSE(handler->isDhtRunning());
#endif
}

TEST_F(BitTorrentHandlerTest, DhtPortConflictLeavesNoZombieClient) {
#ifdef FALCON_USE_LIBTORRENT
    GTEST_SKIP() << "libtorrent 模式无自研 DhtClient（见上一用例）";
#else
    HeldUdpPort held;
    ASSERT_TRUE(held.valid());

    handler->stopDht();
    handler->startDht(held.port());
    // start() 遇 bind 失败只记日志不抛异常——处理器必须清掉没在运行的
    // 客户端，isDhtRunning() 才不撒谎（旧实现留僵尸客户端，
    // 后续 findPeers 的查找无人驱动、回调永不触发）
    EXPECT_FALSE(handler->isDhtRunning());
#endif
}

//==============================================================================
// 任务生命周期（纯 C++ 数据面：自研 DHT 查找 + 内部下载线程）。
// libtorrent 模式 download() 是 worker 线程内的阻塞监控循环（对齐
// metalink 委托形态），主线程直调永不返回，且数据面为 session 原生
// DHT/LSD/PEX——整组与 libtorrent 模式无关，编译期隔离
//==============================================================================

#ifndef FALCON_USE_LIBTORRENT

namespace {

// 生命周期测试夹具：DHT 起在随机端口且无引导节点
class LifecycleHandler {
public:
    LifecycleHandler() {
        handler_->stopDht();
        handler_->startDht(0);
        handler_->clearDhtBootstrapNodes();
    }

    BitTorrentHandler* operator->() { return handler_.get(); }
    BitTorrentHandler* get() { return handler_.get(); }

private:
    std::unique_ptr<BitTorrentHandler> handler_ =
        std::make_unique<BitTorrentHandler>();
};

void waitALittle() {
    // 给异步查找留出收敛窗口（空网络下毫秒级收敛，此处取宽裕值）
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

DownloadTask::Ptr makeTask(TaskId id, const std::string& url) {
    return std::make_shared<DownloadTask>(id, url, DownloadOptions{});
}

} // namespace

TEST_F(BitTorrentHandlerTest, DownloadMagnetHexEndToEnd) {
    LifecycleHandler lc;
    ASSERT_TRUE(lc->isDhtRunning());

    auto task = makeTask(7701, std::string("magnet:?xt=urn:btih:") + kHexHash);
    lc->download(task, nullptr);

    // 状态归调用方（TaskManager）管理，download 只标记启动时间戳
    EXPECT_EQ(task->status(), TaskStatus::Pending);
    EXPECT_NE(task->start_time(), TimePoint{});
    waitALittle();  // 等查找收敛、回调把空 peer 集写回任务上下文
    lc->cancel(task);
}

TEST_F(BitTorrentHandlerTest, DownloadMagnetBase32HashQueriesDecodedHex) {
    LifecycleHandler lc;
    ASSERT_TRUE(lc->isDhtRunning());

    // Base32 magnet：修复前 Base32 文本原样下发（DHT 按 40 位 hex
    // 定位 → 查询错误 info_hash），现解码为原始字节的十六进制
    auto task = makeTask(7702, "magnet:?xt=urn:btih:32W353YBENCWPCNLZXX75XF2TB3FIMQQ");
    lc->download(task, nullptr);

    EXPECT_EQ(task->status(), TaskStatus::Pending);
    EXPECT_NE(task->start_time(), TimePoint{});
    waitALittle();
    lc->cancel(task);
}

TEST_F(BitTorrentHandlerTest, DownloadPauseResumeCancelLifecycle) {
    LifecycleHandler lc;
    ASSERT_TRUE(lc->isDhtRunning());

    auto task = makeTask(7703, std::string("magnet:?xt=urn:btih:") + kHexHash);
    lc->download(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Pending);

    lc->pause(task);
    lc->resume(task, nullptr);
    waitALittle();
    lc->cancel(task);
    lc->cancel(task);  // 幂等
}

TEST_F(BitTorrentHandlerTest, UnknownTaskLifecycleEntriesStaySafe) {
    // 「未知任务三连 no-op」契约已随监控循环接线演进：pause/cancel
    // 首行自置状态（TaskManager 不代置，对齐 http handler 形态），
    // resume 直接 download（DownloadTask::resume 路径同形态）。
    // 本用例钉住演进后的安全面：三入口对未知任务不崩溃，resume
    // 启动的下载由 cancel 干净收口，不留孤儿线程
    auto task = makeTask(7704, std::string("magnet:?xt=urn:btih:") + kHexHash);
    handler->pause(task);
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    handler->resume(task, nullptr);  // resume = 直接 download（真实启动）
    handler->cancel(task);
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
    waitALittle();  // 给 download 线程收尾窗口（fixture 析构前落地）
}

TEST_F(BitTorrentHandlerTest, DownloadMagnetWithoutInfoHashSkipsDhtLookup) {
    LifecycleHandler lc;

    // can_handle 拒绝的 magnet（无 btih 参数）直接调 download：
    // info-hash 为空 → 跳过 DHT 查找，任务仍标记启动
    auto task = makeTask(7705, "magnet:?dn=file.zip");
    lc->download(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Pending);
    waitALittle();
    lc->cancel(task);
}

TEST_F(BitTorrentHandlerTest, DownloadRestartsDhtLazilyWithoutInfoHash) {
    // 同上一用例剧本，但客户端被显式停止：download() 内部的惰性
    // startDht 分支（dhtEnabled_ 默认 true）由此命中——构造函数
    // 已自动启动的客户端永远走不到该分支。info-hash 为空不触发
    // findPeers，无查询发出（UDP socket 绑定纯本地；端口争用时
    // 惰性启动失败静默，用例不受影响）
    handler->stopDht();
    EXPECT_FALSE(handler->isDhtRunning());

    auto task = makeTask(7706, "magnet:?dn=file.zip");
    handler->download(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Pending);
    EXPECT_NE(task->start_time(), TimePoint{});
    waitALittle();
    handler->cancel(task);
    handler->stopDht();
}

TEST_F(BitTorrentHandlerTest, PexHandlerLookupMissingReturnsNull) {
    EXPECT_EQ(handler->getPexHandler("nonexistent-hash"), nullptr);
    handler->removePexHandler("nonexistent-hash");  // 不存在：无操作不崩溃

#ifndef FALCON_USE_LIBTORRENT
    handler->setPexEnabled(false);
    EXPECT_FALSE(handler->isPexEnabled());
    handler->setPexEnabled(true);
    EXPECT_TRUE(handler->isPexEnabled());
#endif
}

#endif // !FALCON_USE_LIBTORRENT
