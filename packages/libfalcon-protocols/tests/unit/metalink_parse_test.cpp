// Metalink 文档解析测试(libmetalink 接缝:格式预检 + 语义映射,不含网络)
//
// 覆盖:meta4 priority 升序与 metalink3 preference 降序双兼容、非
// HTTP/FTP 镜像过滤、路径穿越文件名拒绝(库 skip 语义 + 我方分隔符
// 二次检查)、未知哈希类型跳过、多 file 文档、XML 畸形行列号、垃圾
// 数值回落、v3/v4 命名空间矩阵、错误语义映射、v3 容器层级与 url
// type 必需三条库状态机门禁(迁移批次考古发现)。

#include "plugins/metalink/metalink_handler.hpp"

#include "plugins/metalink/metalink_xml_error.hpp"  // XmlParseError

#include <gtest/gtest.h>

using falcon::HashAlgorithm;
using falcon::protocols::metalink::MetalinkFileParser;
using falcon::protocols::metalink::XmlParseError;
using falcon::protocols::metalink::kNoPriority;

namespace {

/// 构造一个最小 meta4(RFC 5854, v4 命名空间)文档
std::string meta4(const std::string& inner) {
    return "<?xml version=\"1.0\"?><metalink xmlns=\"urn:ietf:params:xml:ns:metalink\">"
           + inner + "</metalink>";
}

/// 构造一个 Metalink3(旧命名空间)文档——v3 的 file 必须包在
/// <files> 容器内(v4 无此层)
std::string metalink3(const std::string& inner) {
    return "<?xml version=\"1.0\"?><metalink xmlns=\"http://www.metalinker.org/\">"
           "<files>" + inner + "</files></metalink>";
}

/// 捕获 runtime_error 家族异常并返回消息(供消息断言)
std::string parse_error_message(const std::string& doc) {
    try {
        MetalinkFileParser::parse(doc);
    } catch (const XmlParseError& e) {
        return e.what();
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
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

TEST(MetalinkParseTest, PriorityAscendingWithMissingPriorityAsSet) {
    // priority 升序;缺省 priority 在库内回落哨兵 → kNoPriority 殿后。
    // 同 rank 的库内 qsort 不保证稳定,殿后两条只做集合断言
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
    EXPECT_EQ(urls[2].priority, kNoPriority);
    EXPECT_EQ(urls[3].priority, kNoPriority);
    const std::string tail[2] = {urls[2].url, urls[3].url};
    EXPECT_TRUE(tail[0] == "http://no-prio-1/" ||
                tail[1] == "http://no-prio-1/");
    EXPECT_TRUE(tail[0] == "http://no-prio-2/" ||
                tail[1] == "http://no-prio-2/");
}

TEST(MetalinkParseTest, Metalink3PreferenceDescending) {
    // metalink3 文档(v3 命名空间):库把 preference 换算为
    // priority = 1000000 - preference,数值越大越优先的语义不变
    const auto files = MetalinkFileParser::parse(metalink3(
        "<file name=\"f\">"
        "<resources>"
        "<url type=\"http\" preference=\"50\">http://mid/</url>"
        "<url type=\"http\" preference=\"90\">http://top/</url>"
        "<url type=\"http\" preference=\"10\">http://low/</url>"
        "</resources></file>"));
    ASSERT_EQ(files.size(), 1u);
    const auto& urls = files.front().urls;
    ASSERT_EQ(urls.size(), 3u);
    EXPECT_EQ(urls[0].url, "http://top/");
    EXPECT_EQ(urls[1].url, "http://mid/");
    EXPECT_EQ(urls[2].url, "http://low/");
}

TEST(MetalinkParseTest, PriorityBeatsPreferenceWhenBothPresent) {
    // v4 文档的 url 元素不读 preference 属性(库 v4 状态机只取
    // location/priority)——带 preference 的条目 priority 落哨兵
    // kNoPriority 殿后,显式 priority 获胜
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url preference=\"99\">http://by-pref/</url>"
        "<url priority=\"0\">http://by-prio/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 2u);
    EXPECT_EQ(files.front().urls[0].url, "http://by-prio/");
    EXPECT_EQ(files.front().urls[0].priority, 0);
    EXPECT_EQ(files.front().urls[1].url, "http://by-pref/");
    EXPECT_EQ(files.front().urls[1].priority, kNoPriority);
}

TEST(MetalinkParseTest, EqualRankAllNoPriority) {
    // 全部缺省 rank:两镜像都在,组内顺序不钉(库 qsort 不稳定),
    // 只断言集合与 rank
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url>http://first/</url><url>http://second/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    const auto& urls = files.front().urls;
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_EQ(urls[0].priority, kNoPriority);
    EXPECT_EQ(urls[1].priority, kNoPriority);
    EXPECT_TRUE(urls[0].url == "http://first/" &&
                urls[1].url == "http://second/");
}

TEST(MetalinkParseTest, QueryAndFragmentPreservedInUrl) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><url>http://a/d?token=x&amp;y=1#frag</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/d?token=x&y=1#frag");
}

TEST(MetalinkParseTest, CdataUrlText) {
    // expat 预检层:CDATA 段内容按字符数据解析
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><url><![CDATA[http://a/cdata]]></url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/cdata");
}

TEST(MetalinkParseTest, NonHttpFtpUrlsFiltered) {
    // magnet/自定义 scheme 直接跳过;v4 不读 type 属性,过滤按
    // scheme 兜底——委托模式下不可达的镜像不进列表
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url>magnet:?xt=urn:sha1:ABC</url>"
        "<url type=\"ftp\">ftp://real/f</url>"
        "<url type=\"HTTP\">http://real2/f</url>"
        "</file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 2u);
    EXPECT_EQ(files.front().urls[0].url, "ftp://real/f");
    EXPECT_EQ(files.front().urls[1].url, "http://real2/f");
}

TEST(MetalinkParseTest, Metalink3BittorrentTypeFiltered) {
    // v3 状态机读 type 属性:type 标注非 http/ftp(如 bittorrent)
    // 的条目跳过
    const auto files = MetalinkFileParser::parse(metalink3(
        "<file name=\"f\">"
        "<resources>"
        "<url type=\"bittorrent\">http://seed/</url>"
        "<url type=\"http\">http://real/f</url>"
        "</resources></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://real/f");
    EXPECT_EQ(files.front().urls[0].type, "http");
}

TEST(MetalinkParseTest, HttpSchemeCaseInsensitive) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><url>HTTP://a/x</url>"
        "<url>FtP://b/y</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 2u);
}

TEST(MetalinkParseTest, PathTraversalNameRejected) {
    // 前缀穿越(..)/裸 ../反斜杠:库 check_safe_path 拒绝该 file
    // (静默跳过),文档内无其余 file → 「不含 file 元素」拒绝;
    // 嵌入分隔符 sub/dir 库放行、由我方二次检查拒绝——四形态
    // 均落 runtime_error 家族
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

TEST(MetalinkParseTest, LibrarySkipsUnsafeNameFile) {
    // 库对不安全文件名的 file 是静默跳过(非报错):唯一 file 被
    // 跳过后文档无 file → 「不含 file 元素」——区分库 skip 与
    // 我方二次检查两条拒绝路径
    EXPECT_NE(parse_error_message(meta4(
        "<file name=\"../evil\"><url>http://a/</url></file>"))
                  .find("不含 file 元素"), std::string::npos);
}

TEST(MetalinkParseTest, StrictPathCheckRejectsEmbeddedSlash) {
    // 库 check_safe_path 只检查最后一段,放行嵌入 '/' 的名字;
    // 我方 is_safe_filename 拒绝——转换层的路径安全契约不变
    EXPECT_NE(parse_error_message(meta4(
        "<file name=\"sub/dir\"><url>http://a/</url></file>"))
                  .find("路径穿越"), std::string::npos);
}

TEST(MetalinkParseTest, MissingNameAttrSkipsFile) {
    // 缺 name 属性:库静默跳过该 file → 文档无 file → 拒绝
    EXPECT_NE(parse_error_message(meta4(
        "<file><url>http://a/</url></file>"))
                  .find("不含 file 元素"), std::string::npos);
    // 空 name:我方 is_safe_filename 拒绝空串
    EXPECT_THROW(MetalinkFileParser::parse(meta4(
        "<file name=\"\"><url>http://a/</url></file>")),
                 std::runtime_error);
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
// 命名空间矩阵(v4 / v3 / 缺失)
//==============================================================================

TEST(MetalinkParseTest, MissingNamespaceYieldsInvalidDocument) {
    // 库对无命名空间文档整体静默跳过(version=UNKNOWN)→ files 空
    // → 「不是有效的 metalink 文档」——与「含 file 元素」的拒绝
    // 消息可区分
    EXPECT_NE(parse_error_message(
        "<?xml version=\"1.0\"?><metalink>"
        "<file name=\"f\"><url>http://a/</url></file></metalink>")
                  .find("不是有效的 metalink 文档"), std::string::npos);
}

TEST(MetalinkParseTest, UnknownNamespaceYieldsInvalidDocument) {
    EXPECT_NE(parse_error_message(
        "<?xml version=\"1.0\"?><metalink xmlns=\"urn:not-metalink\">"
        "<file name=\"f\"><url>http://a/</url></file></metalink>")
                  .find("不是有效的 metalink 文档"), std::string::npos);
}

TEST(MetalinkParseTest, Metalink3NamespaceAccepted) {
    // v3 命名空间被库识别(version=3):无 size/hash 也能解析
    const auto files = MetalinkFileParser::parse(metalink3(
        "<file name=\"v3.iso\">"
        "<resources><url type=\"http\">http://a/v3.iso</url></resources></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().name, "v3.iso");
    EXPECT_EQ(files.front().size, 0u);
    EXPECT_TRUE(files.front().hashes.empty());
}

TEST(MetalinkParseTest, Metalink3PreferenceMappingValues) {
    // preference → priority 换算可观测:100 → 999900、0 → 1000000
    const auto files = MetalinkFileParser::parse(metalink3(
        "<file name=\"f\">"
        "<resources>"
        "<url type=\"http\" preference=\"100\">http://top/</url>"
        "<url type=\"http\" preference=\"0\">http://bottom/</url>"
        "</resources></file>"));
    ASSERT_EQ(files.size(), 1u);
    const auto& urls = files.front().urls;
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_EQ(urls[0].url, "http://top/");
    EXPECT_EQ(urls[0].priority, 999900);
    EXPECT_EQ(urls[0].preference, 100);
    EXPECT_EQ(urls[1].url, "http://bottom/");
    EXPECT_EQ(urls[1].priority, 1000000);
    EXPECT_EQ(urls[1].preference, 0);
}

TEST(MetalinkParseTest, Metalink3LocationAndTypeAttributes) {
    const auto files = MetalinkFileParser::parse(metalink3(
        "<file name=\"f\">"
        "<resources>"
        "<url type=\"http\" location=\"JP\">http://a/x</url>"
        "</resources></file>"));
    ASSERT_EQ(files.size(), 1u);
    const auto& u = files.front().urls.front();
    EXPECT_EQ(u.location, "JP");
    EXPECT_EQ(u.type, "http");
}

//==============================================================================
// 数值垃圾与边界
//==============================================================================

TEST(MetalinkParseTest, AttrGarbageAndOverflowFallBack) {
    // 库数值解析语义:size 经 strtoll(垃圾前导解析/溢出/负数→0);
    // priority 缺省/溢出/超 int 上限回落库哨兵 999999 → kNoPriority;
    // "12abc" 垃圾尾随按 strtol 前导解析得 12(不致命)
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
    EXPECT_EQ(urls[0].url, "http://c/");   // priority 3
    EXPECT_EQ(urls[1].url, "http://a/");   // priority 12(垃圾尾随容忍)
    EXPECT_EQ(urls[1].priority, 12);
    // b/d 均回落 kNoPriority,组内顺序不钉
    EXPECT_EQ(urls[2].priority, kNoPriority);
    EXPECT_EQ(urls[3].priority, kNoPriority);
    const std::string tail[2] = {urls[2].url, urls[3].url};
    EXPECT_TRUE(tail[0] == "http://b/" || tail[1] == "http://b/");
    EXPECT_TRUE(tail[0] == "http://d/" || tail[1] == "http://d/");
}

TEST(MetalinkParseTest, PriorityNegativeFallsBackToNoPriority) {
    // 负 priority:库回落哨兵 999999 → kNoPriority
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url priority=\"-5\">http://neg/</url>"
        "<url priority=\"1\">http://pos/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 2u);
    EXPECT_EQ(files.front().urls[0].url, "http://pos/");
    EXPECT_EQ(files.front().urls[1].url, "http://neg/");
    EXPECT_EQ(files.front().urls[1].priority, kNoPriority);
}

TEST(MetalinkParseTest, PriorityZeroAccepted) {
    // priority=0 是合法最高优先(非缺省)
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\">"
        "<url priority=\"2\">http://second/</url>"
        "<url priority=\"0\">http://first/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://first/");
    EXPECT_EQ(files.front().urls[0].priority, 0);
}

TEST(MetalinkParseTest, SizeNegativeFallsToZero) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><size>-5</size><url>http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().size, 0u);
}

TEST(MetalinkParseTest, SizeOverflowFallsToZero) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><size>99999999999999999999</size>"
        "<url>http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().size, 0u);
}

TEST(MetalinkParseTest, SizeLargeValueAccepted) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><size>4294967296</size>"
        "<url>http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().size, 4294967296ull);
}

TEST(MetalinkParseTest, MissingSizeDefaultsZero) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><url>http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().size, 0u);
}

TEST(MetalinkParseTest, UrlTextWhitespaceTrimmed) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f\"><url>  http://a/  </url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/");
}

TEST(MetalinkParseTest, Utf8FileNameAccepted) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"下载.bin\"><url>http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().name, u8"下载.bin");
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
    // 格式层(expat 预检):错误带行列号,保住诊断契约——libmetalink
    // 对格式错误只回笼统 PARSER_ERROR(无位置)
    try {
        MetalinkFileParser::parse("<metalink><file name=\"f\">");
        FAIL() << "expected XmlParseError";
    } catch (const XmlParseError& e) {
        EXPECT_GT(e.line(), 0u);
    }
}

TEST(MetalinkParseTest, MalformedXmlMessageHasLineCol) {
    const std::string msg = parse_error_message("<metalink><file>");
    EXPECT_NE(msg.find("(line "), std::string::npos);
    EXPECT_NE(msg.find(", col "), std::string::npos);
}

TEST(MetalinkParseTest, MalformedXmlTrailingGarbageRejected) {
    try {
        MetalinkFileParser::parse(
            "<?xml version=\"1.0\"?><metalink xmlns=\"urn:ietf:params:xml:ns:metalink\">"
            "<file name=\"f\"><url>http://a/</url></file></metalink>junk");
        FAIL() << "expected XmlParseError";
    } catch (const XmlParseError& e) {
        EXPECT_GT(e.line(), 0u);
    }
}

TEST(MetalinkParseTest, EntityDecodedInName) {
    // 预置实体在 name 属性中解码;解码后仍要过路径安全检查
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"a&amp;b.bin\"><url>http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().name, "a&b.bin");
}

// ---------------------------------------------------------------------------
// v3 容器层级与 url type 必需(手写解析器时代宽松遍历不会暴露的三条
// 库状态机门禁——迁移批次考古发现,作为行为契约钉住)
// ---------------------------------------------------------------------------

TEST(MetalinkParseTest, Metalink3FilesContainerRequired) {
    // v3 的 <file> 必须包在 <files> 容器内;直接挂在根下会被库状态机
    // 静默跳过(与 v4 的直挂形态不同)→ 落「不含 file 元素」
    const std::string msg = parse_error_message(
        "<?xml version=\"1.0\"?>"
        "<metalink xmlns=\"http://www.metalinker.org/\">"
        "<file name=\"f.bin\"><resources>"
        "<url type=\"http\">http://a/f.bin</url>"
        "</resources></file></metalink>");
    EXPECT_NE(msg.find("不含 file 元素"), std::string::npos);
}

TEST(MetalinkParseTest, Metalink3ResourcesContainerRequired) {
    // v3 的 <url> 必须包在 <resources> 容器内;直挂 file 下被库跳过
    // → file 有 name 但无任何资源 → 落「无可用 HTTP/FTP 镜像」
    const std::string msg = parse_error_message(metalink3(
        "<file name=\"f.bin\">"
        "<url type=\"http\">http://a/f.bin</url>"
        "</file>"));
    EXPECT_NE(msg.find("无可用 HTTP/FTP 镜像"), std::string::npos);
}

TEST(MetalinkParseTest, Metalink3UrlTypeAttrRequired) {
    // v3 的 url 元素缺 type 属性被库静默跳过(不报错,resources 为空)
    // → 「无可用 HTTP/FTP 镜像」。与 v4 形成对照:v4 无此要求
    const std::string msg = parse_error_message(metalink3(
        "<file name=\"f.bin\"><resources>"
        "<url>http://a/f.bin</url>"
        "</resources></file>"));
    EXPECT_NE(msg.find("无可用 HTTP/FTP 镜像"), std::string::npos);
}

TEST(MetalinkParseTest, Metalink4UrlTypeBittorrentIgnoredByLibrary) {
    // v4 文档的 type 属性 libmetalink 不解析(mu.type 恒空)——
    // type="bittorrent" 不会触发 type 过滤,http href 靠 scheme 兜底
    // 照常采用。与 v3 的 type 过滤(Metalink3BittorrentTypeFiltered)
    // 形成双版本行为对照
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\">"
        "<url type=\"bittorrent\" priority=\"1\">http://a/f.bin</url>"
        "</file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/f.bin");
}

TEST(MetalinkParseTest, Metalink4UrlTypeAttributeRecordedWhenPresent) {
    // 若库对 v4 也填充 type 字段则原样记录;无论填充与否
    // type="http" 都不触发过滤(与 scheme 判断同向,行为稳定)
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\">"
        "<url type=\"http\" priority=\"1\">http://a/f.bin</url>"
        "</file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/f.bin");
}

// ---------------------------------------------------------------------------
// 库 skip 语义的粒度:单 file 缺陷不污染同文档其余条目
// ---------------------------------------------------------------------------

TEST(MetalinkParseTest, MultiFilePartialSkipRestParsed) {
    // 三条 file,中间一条缺 name 被库 skip——其余两条照常返回
    // (skip 是 per-file 的,不是整文档失败)
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"a.bin\"><url priority=\"1\">http://a/</url></file>"
        "<file><url priority=\"1\">http://x/</url></file>"
        "<file name=\"c.bin\"><url priority=\"1\">http://c/</url></file>"));
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].name, "a.bin");
    EXPECT_EQ(files[1].name, "c.bin");
}

// ---------------------------------------------------------------------------
// size / hash / url 文本边界
// ---------------------------------------------------------------------------

TEST(MetalinkParseTest, SizeGarbageTextFallsToZero) {
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\"><size>abc</size>"
        "<url priority=\"1\">http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().size, 0u);
}

TEST(MetalinkParseTest, WhitespaceHashTextSkipped) {
    // hash 文本 trim 后为空 → 该条跳过,不进 hashes
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\">"
        "<hash type=\"sha-256\">   </hash>"
        "<hash type=\"sha-256\">1912dd</hash>"
        "<url priority=\"1\">http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().hashes.size(), 1u);
    EXPECT_EQ(files.front().hashes.front().first, "1912dd");
}

TEST(MetalinkParseTest, DuplicateHashTypeBothKept) {
    // 同算法两条 hash 都收进 vector(交付端 verify 首条命中即用,
    // 收集层不做去重)
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\">"
        "<hash type=\"sha-256\">aaaa</hash>"
        "<hash type=\"sha-256\">bbbb</hash>"
        "<url priority=\"1\">http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().hashes.size(), 2u);
    EXPECT_EQ(files.front().hashes[0].first, "aaaa");
    EXPECT_EQ(files.front().hashes[1].first, "bbbb");
}

TEST(MetalinkParseTest, HashValueCasePreserved) {
    // hash 期望值原样大小写保留(交付端按流式哈希 hex 对拍,不做
    // 大小写归一)
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\">"
        "<hash type=\"sha-256\">ABCDEF0123</hash>"
        "<url priority=\"1\">http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().hashes.front().first, "ABCDEF0123");
}

TEST(MetalinkParseTest, EmptyUrlTextSkipped) {
    // url 文本 trim 后为空 → 该条跳过;唯此一条时落「无可用镜像」
    const std::string msg = parse_error_message(meta4(
        "<file name=\"f.bin\">"
        "<url priority=\"1\">   </url>"
        "</file>"));
    EXPECT_NE(msg.find("无可用 HTTP/FTP 镜像"), std::string::npos);
}

TEST(MetalinkParseTest, UrlEntityDecoded) {
    // url 文本中的预置实体解码(query 中的 &amp; → &)
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\">"
        "<url priority=\"1\">http://a/f?x=1&amp;y=2</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/f?x=1&y=2");
}

// ---------------------------------------------------------------------------
// 忽略面:未识别属性与 XML 杂项不干扰解析(expat 的 well-formedness
// 之外全部宽松——与手写解析器的严格模式行为差异,按库语义固化)
// ---------------------------------------------------------------------------

TEST(MetalinkParseTest, V4ExtensionAttributesIgnored) {
    // v4 file 上的扩展属性(maxconnections 等)未识别即忽略
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\" maxconnections=\"2\">"
        "<url priority=\"1\" maxconnections=\"4\">http://a/</url></file>"));
    ASSERT_EQ(files.size(), 1u);
    ASSERT_EQ(files.front().urls.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/");
}

TEST(MetalinkParseTest, V3OsLanguageAttributesIgnored) {
    // v3 file 的 os/language 属性忽略,解析照常
    const auto files = MetalinkFileParser::parse(metalink3(
        "<file name=\"f.bin\" os=\"linux\" language=\"zh-CN\">"
        "<resources><url type=\"http\" preference=\"80\">"
        "http://a/f.bin</url></resources></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().name, "f.bin");
    ASSERT_EQ(files.front().urls.size(), 1u);
}

TEST(MetalinkParseTest, CommentAndProcessingInstructionIgnored) {
    // 文档中段注释与处理指令不影响解析(expat well-formedness 之外
    // 宽松)
    const auto files = MetalinkFileParser::parse(meta4(
        "<?xml-stylesheet type=\"text/xsl\" href=\"style.xsl\"?>"
        "<!-- 中段注释 -->"
        "<file name=\"f.bin\"><url priority=\"1\">http://a/</url></file>"
        "<!-- 尾注释 -->"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().name, "f.bin");
}

TEST(MetalinkParseTest, CdataInNameText) {
    // CDATA 出现在元素文本(url)中的形态——expat 解出原文
    const auto files = MetalinkFileParser::parse(meta4(
        "<file name=\"f.bin\">"
        "<url priority=\"1\"><![CDATA[http://a/f.bin?x=1]]></url></file>"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front().urls[0].url, "http://a/f.bin?x=1");
}
