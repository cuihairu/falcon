/**
 * @file bencode_edge_test.cpp
 * @brief B 编码边界与错误路径测试
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 覆盖类型不匹配访问、越界/缺键访问、非法输入解码等异常路径，
 * 以及 BencodeUtils 快捷函数（既有 bittorrent_plugin_test 覆盖正常路径）。
 */

#include <falcon/plugins/bittorrent/bencode.hpp>

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace falcon::protocols;

namespace {

BencodeValue makeSampleDict() {
    BencodeValue dict;
    dict["name"].setString("falcon");
    dict["port"].setInt(6881);
    BencodeValue list;
    list.setList({BencodeValue(std::string("a")), BencodeValue(static_cast<int64_t>(2))});
    dict["items"] = list;
    return dict;
}

} // namespace

//==============================================================================
// 类型不匹配访问 → BencodeException
//==============================================================================

TEST(BencodeEdgeTest, AccessorsThrowOnTypeMismatch) {
    const BencodeValue str(std::string("text"));
    const BencodeValue num(static_cast<int64_t>(7));

    EXPECT_THROW(str.asInt(), BencodeException);
    EXPECT_THROW(num.asString(), BencodeException);
    EXPECT_THROW(str.asList(), BencodeException);
    EXPECT_THROW(str.asDict(), BencodeException);
    EXPECT_THROW(num.asList(), BencodeException);
    EXPECT_THROW(num.asDict(), BencodeException);
}

TEST(BencodeEdgeTest, SetListRoundTrip) {
    BencodeValue list;
    list.setList({BencodeValue(std::string("x")), BencodeValue(static_cast<int64_t>(1))});
    ASSERT_TRUE(list.isList());
    ASSERT_EQ(list.size(), size_t{2});
    EXPECT_EQ(list[0].asString(), "x");
    EXPECT_EQ(list[1].asInt(), 1);
}

//==============================================================================
// 字典/列表访问边界
//==============================================================================

TEST(BencodeEdgeTest, ConstDictAccessMissingKeyThrows) {
    const BencodeValue dict = makeSampleDict();
    EXPECT_TRUE(dict.hasKey("name"));
    EXPECT_FALSE(dict.hasKey("missing"));  // hasKey false 分支
    EXPECT_THROW(dict["missing"].asString(), BencodeException);  // Key not found
    EXPECT_THROW(dict["missing2"].asString(), BencodeException);
}

TEST(BencodeEdgeTest, DictAccessOnNonDictThrows) {
    const BencodeValue str(std::string("text"));
    EXPECT_THROW(str["key"], BencodeException);
}

TEST(BencodeEdgeTest, ListIndexOutOfRangeThrows) {
    const BencodeValue list = makeSampleDict()["items"];
    EXPECT_THROW(list[99].asString(), BencodeException);  // const 版本越界

    BencodeValue mutableList;
    mutableList.setList({BencodeValue(std::string("only"))});
    EXPECT_THROW(mutableList[5].asString(), BencodeException);  // 非常量版本越界
}

TEST(BencodeEdgeTest, ListIndexOnNonListThrows) {
    const BencodeValue str(std::string("text"));
    EXPECT_THROW(str[0], BencodeException);
}

TEST(BencodeEdgeTest, SizeDependsOnType) {
    EXPECT_EQ(makeSampleDict().size(), size_t{3});      // dict：键值对数
    EXPECT_EQ(BencodeValue(std::string("abcd")).size(), size_t{4});  // string：字节数
    EXPECT_EQ(BencodeValue().size(), size_t{0});        // integer：0
}

//==============================================================================
// 解码错误路径
//==============================================================================

TEST(BencodeEdgeTest, DecodeMalformedInputsThrow) {
    // 字符串长度前缀后无冒号
    EXPECT_THROW(BencodeValue::decode("3a"), BencodeException);
    // 声明长度超出剩余数据
    EXPECT_THROW(BencodeValue::decode("9:abc"), BencodeException);
    // 整数体非法
    EXPECT_THROW(BencodeValue::decode("ixey"), BencodeException);
    // 列表未终止
    EXPECT_THROW(BencodeValue::decode("li1e"), BencodeException);
    // 字典键不是字符串
    EXPECT_THROW(BencodeValue::decode("di1ei2ee"), BencodeException);
    // 字典未终止（空字典体）
    EXPECT_THROW(BencodeValue::decode("d"), BencodeException);
    // 字典未终止（有键值）
    EXPECT_THROW(BencodeValue::decode("d4:blah4:test"), BencodeException);
    // 空输入
    EXPECT_THROW(BencodeValue::decode(""), BencodeException);
    // 未知类型标记
    EXPECT_THROW(BencodeValue::decode("x"), BencodeException);
}

TEST(BencodeEdgeTest, EncodeDecodeNestedStructures) {
    BencodeValue root;
    root["id"].setInt(42);
    root["url"].setString("http://tracker/announce");
    BencodeValue tiers;
    tiers.setList({BencodeValue(std::string("udp://a")),
                   BencodeValue(std::string("udp://b"))});
    root["tiers"] = tiers;

    auto decoded = BencodeValue::decode(root.encode());
    EXPECT_EQ(decoded["id"].asInt(), 42);
    EXPECT_EQ(decoded["url"].asString(), "http://tracker/announce");
    ASSERT_EQ(decoded["tiers"].size(), size_t{2});
    EXPECT_EQ(decoded["tiers"][1].asString(), "udp://b");
}

//==============================================================================
// BencodeUtils 快捷函数
//==============================================================================

TEST(BencodeUtilsTest, EncodeDictProducesBencodedDict) {
    std::map<std::string, std::string> dict{{"k1", "v1"}, {"k2", "v2"}};
    std::string encoded = BencodeUtils::encodeDict(dict);

    auto decoded = BencodeUtils::decodeDictToStringMap(encoded);
    ASSERT_EQ(decoded.size(), size_t{2});
    EXPECT_EQ(decoded["k1"], "v1");
    EXPECT_EQ(decoded["k2"], "v2");
}

TEST(BencodeUtilsTest, DecodeDictToStringMapRejectsNonDict) {
    BencodeValue list;
    list.setList({BencodeValue(std::string("a"))});
    EXPECT_THROW(BencodeUtils::decodeDictToStringMap(list), BencodeException);
    EXPECT_THROW(BencodeUtils::decodeDictToStringMap("i5e"), BencodeException);
}

TEST(BencodeUtilsTest, GenericEncodeDecodeRoundTrip) {
    BencodeValue value(static_cast<int64_t>(12345));
    auto decoded = BencodeUtils::decode(BencodeUtils::encode(value));
    EXPECT_EQ(decoded.asInt(), 12345);
}
