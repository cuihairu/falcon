// Metalink 文档解析测试(模型 + 排序 + 过滤,不含网络)
//
// 覆盖:meta4 priority 升序与 metalink3 preference 降序双兼容、同序
// 文档稳定、非 HTTP/FTP 镜像过滤、路径穿越文件名拒绝、未知哈希类型
// 跳过、多 file 文档、各畸形文档拒绝。

#include "plugins/metalink/metalink_handler.hpp"

#include "plugins/metalink/mini_xml_parser.hpp"  // XmlParseError

#include <gtest/gtest.h>

using falcon::HashAlgorithm;
using falcon::protocols::metalink::MetalinkFileParser;
using falcon::protocols::metalink::kNoPriority;

namespace {

/// 构造一个最小 meta4 文档
std::string meta4(const std::string& inner) {
    return "<?xml version=\"1.0\"?><metalink xmlns=\"urn:ietf:params:xml:ns:metalink\">"
           + inner + "</metalink>";
}

} // namespace

TEST(MetalinkParseTest, BasicMeta4Fields) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"ubuntu.iso\">"
        "<size>1468006</size>"
        "<hash type=\"sha-256\">1912dd</hash>"
        "<url location=\"JP\" priority=\"1\">http://a/f.iso</url>"
        "<url location=\"US\" priority=\"2\">ftp://b/f.iso</url>"
        "</file>"));
    ASSERT_EQ(files.size(), 1u);
    const auto& f = files.front();
    EXPECT_EQ(f.name, "ubuntu.iso");
    EXPECT_EQ(f.size, 1468006u);
    ASSERT_EQ(f.hashes.size(), 1u);
    EXPECT_EQ(f.hashes.front().first, "1912dd");
    EXPECT_EQ(f.hashes.front().second, HashAlgorithm::SHA256);
    ASSERT_EQ(f.urls.size(), 2u);
    EXPECT_EQ(f.urls[0].url, "http://a/f.iso");
    EXPECT_EQ(f.urls[0].priority, 1);
    EXPECT_EQ(f.urls[0].location, "JP");
    EXPECT_EQ(f.urls[1].url, "ftp://b/f.iso");
}

TEST(MetalinkParseTest, PriorityAscendingWithMissingPriorityLast) {
    // priority 升序;缺省 priority(=kNoPriority)殿后,保持文档序
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><url priority=\"5\">http://c/</url>"
        "<url>http://no-prio-1/</url>"
        "<url priority=\"1\">http://a/</url>"
        "<url>http://no-prio-2/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    const auto& urls = files.front().urls;
    ASSERT_EQ(urls.size(), 4u);
    EXPECT_EQ(urls[0].url, "http://a/");
    EXPECT_EQ(urls[1].url, "http://c/");
    EXPECT_EQ(urls[2].url, "http://no-prio-1/");
    EXPECT_EQ(urls[3].url, "http://no-prio-2/");
    EXPECT_EQ(urls[2].priority, kNoPriority);
}

TEST(MetalinkParseTest, Metalink3PreferenceDescending) {
    // metalink3 无 priority/preference 属性名差异:preference 数值越大越优先
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url preference=\"50\">http://mid/</url>"
        "<url preference=\"90\">http://top/</url>"
        "<url preference=\"10\">http://low/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    const auto& urls = files.front().urls;
    ASSERT_EQ(urls.size(), 3u);
    EXPECT_EQ(urls[0].url, "http://top/");
    EXPECT_EQ(urls[1].url, "http://mid/");
    EXPECT_EQ(urls[2].url, "http://low/");
}

TEST(MetalinkParseTest, PriorityBeatsPreferenceWhenBothPresent) {
    // 排序键:有 priority 用 priority,否则换算 preference——两者并存时
    // priority 获胜
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url preference=\"99\">http://by-pref/</url>"
        "<url priority=\"0\">http://by-prio/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 2u);
    EXPECT_EQ(files.front().urls[0].url, "http://by-prio/");
    EXPECT_EQ(files.front().urls[1].url, "http://by-pref/");
}

TEST(MetalinkParseTest, EqualRankKeepsDocumentOrder) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url>http://first/</url><url>http://second/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://first/");
    EXPECT_EQ(files.front().urls[1].url, "http://second/");
}

TEST(MetalinkParseTest, QueryAndFragmentPreservedInUrl) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><url>http://a/d?token=x&amp;y=1#frag</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/d?token=x&y=1#frag");
}

TEST(MetalinkParseTest, NonHttpFtpUrlsFiltered) {
    // magnet/自定义 scheme 直接跳过;type 标注非 http/ftp(如 bittorrent)
    // 的条目跳过——委托模式下不可达的镜像不进列表
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url type=\"bittorrent\">http://seed/</url>"
        "<url>magnet:?xt=urn:sha1:ABC</url>"
        "<url type=\"ftp\">ftp://real/f</url>"
        "<url type=\"HTTP\">http://real2/f</url>"
        "</file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 2u);
    EXPECT_EQ(files.front().urls[0].url, "ftp://real/f");
    EXPECT_EQ(files.front().urls[1].url, "http://real2/f");
}

TEST(MetalinkParseTest, PathTraversalNameRejected) {
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file name=\"../evil\"><url>http://a/</url></file>")),
                 std::runtime_error);
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file name=\"sub/dir\"><url>http://a/</url></file>")),
                 std::runtime_error);
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file name=\"..\"><url>http://a/</url></file>")),
                 std::runtime_error);
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file name=\"back\\\\slash\"><url>http://a/</url></file>")),
                 std::runtime_error);
}

TEST(MetalinkParseTest, MissingOrEmptyNameRejected) {
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file><url>http://a/</url></file>")), std::runtime_error);
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file name=\"\"><url>http://a/</url></file>")), std::runtime_error);
}

TEST(MetalinkParseTest, UnknownHashTypesSkipped) {
    // 只认 md5/sha-1/sha-256/sha-512(含无连字符别名,大小写不敏感);
    // 未知类型(sha3/blake2)跳过;空 hash 文本跳过
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<hash type=\"sha3\">deadbeef</hash>"
        "<hash type=\"SHA-1\">a9993e36</hash>"
        "<hash type=\"sha256\"></hash>"
        "<hash type=\"md5\">0123456789abcdef0123456789abcdef</hash>"
        "<url>http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().hashes.size(), 2u);
    EXPECT_EQ(files.front().hashes[0].second, HashAlgorithm::SHA1);
    EXPECT_EQ(files.front().hashes[1].second, HashAlgorithm::MD5);
}

TEST(MetalinkParseTest, HashTypeAliasesAndCaseInsensitive) {
    HashAlgorithm algo = HashAlgorithm::MD5;
    ASSERT_TRUE(MetalinkFileParser::hash_type_to_algorithm("sha-512", algo));
    EXPECT_EQ(algo, HashAlgorithm::SHA512);
    ASSERT_TRUE(MetalinkFileParser::hash_type_to_algorithm("SHA256", algo));
    EXPECT_EQ(algo, HashAlgorithm::SHA256);
    ASSERT_TRUE(MetalinkFileParser::hash_type_to_algorithm(" Sha-1 ", algo));
    EXPECT_EQ(algo, HashAlgorithm::SHA1);
    EXPECT_FALSE(MetalinkFileParser::hash_type_to_algorithm("crc32", algo));
}

TEST(MetalinkParseTest, MultipleFilesParsed) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"a\"><url>http://a/</url></file>"
        "<file name=\"b\"><url>http://b/</url></file>"));
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].name, "a");
    EXPECT_EQ(files[1].name, "b");
}

TEST(MetalinkParseTest, PieceHashIgnored) {
    // <pieces> 分片哈希阶段1 忽略,不影响解析
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<pieces length=\"262144\" type=\"sha-1\">AAECAwQFBgc=</pieces>"
        "<url>http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_TRUE(files.front().hashes.empty());
}

//==============================================================================
// 拒绝系列
//==============================================================================

TEST(MetalinkParseTest, NonMetalinkRootRejected) {
    EXPECT_THROW(MetalinkFileParser::parse("<foo><file name=\"x\"/></foo>"),
                 std::runtime_error);
    EXPECT_THROW(MetalinkFileParser::parse(""), std::runtime_error);
}

TEST(MetalinkParseTest, NoFileElementsRejected) {
    EXPECT_THROW(MetalinkFileParser::parse(meta4("<generator>x</generator>")),
                 std::runtime_error);
}

TEST(MetalinkParseTest, FileWithoutUsableUrlRejected) {
    // file 有 name 但所有 URL 均不可用(空/非 http-ftp)→ 拒绝整个文档
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><url></url>"
        "<url>magnet:?x=1</url></file>")), std::runtime_error);
    // 完全没有 url 子元素同样拒绝
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><size>1</size></file>")), std::runtime_error);
}

TEST(MetalinkParseTest, MalformedXmlPropagatesWithPosition) {
    try {
        MetalinkFileParser::parse("<metalink><file name=\"f\">");
        FAIL() << "expected XmlParseError";
    } catch (const falcon::protocols::metalink::XmlParseError& e) {
        EXPECT_GT(e.line(), 0u);
    }
}

TEST(MetalinkParseTest, AttrGarbageAndOverflowFallBack) {
    // priority/size 的垃圾值与溢出值一律回落默认值:不抛出、不致命
    // ("12abc" 尾随垃圾 / 9999...9 超出 long / 3000000000 超出 int /
    //  size 非数字),回落条目视为缺省 rank 保持文档序
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<size>not-a-number</size>"
        "<url priority=\"3\">http://c/</url>"
        "<url priority=\"12abc\">http://a/</url>"
        "<url priority=\"3000000000\">http://b/</url>"
        "<url priority=\"99999999999999999999\">http://d/</url>"
        "</file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().size, 0u);
    const auto& urls = files.front().urls;
    ASSERT_EQ(urls.size(), 4u);
    EXPECT_EQ(urls[0].url, "http://c/");  // 唯一有效 priority
    EXPECT_EQ(urls[1].url, "http://a/");  // 三条回落 kNoPriority,文档序
    EXPECT_EQ(urls[2].url, "http://b/");
    EXPECT_EQ(urls[3].url, "http://d/");
    EXPECT_EQ(urls[1].priority, kNoPriority);
    EXPECT_EQ(urls[2].priority, kNoPriority);
    EXPECT_EQ(urls[3].priority, kNoPriority);
}
