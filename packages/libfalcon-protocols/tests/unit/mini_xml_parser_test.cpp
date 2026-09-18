// mini XML 解析器单元测试
//
// 覆盖:基本结构(嵌套/自闭合/属性保序)、实体解码(命名/十进制/十六
// 进制/多字节 UTF-8)、CDATA 原样、声明/注释跳过、命名空间前缀剥离;
// Reject 系列严格拒绝(未闭合/错配/重复属性/注释内 --/未知 PI/文本
// 中 ]]/非法实体/多根),错误携带行列号。

#include "plugins/metalink/mini_xml_parser.hpp"

#include <gtest/gtest.h>

using falcon::protocols::metalink::MiniXmlParser;
using falcon::protocols::metalink::XmlNode;
using falcon::protocols::metalink::XmlParseError;

namespace {

const XmlNode* must_child(const XmlNode& node, const std::string& name) {
    const XmlNode* c = node.child(name);
    EXPECT_NE(c, nullptr) << "child not found: " << name;
    return c;
}

} // namespace

//==============================================================================
// 基本结构
//==============================================================================

TEST(MiniXmlParserTest, ParseSimpleDocument) {
    const auto root = MiniXmlParser::parse("<a>hello</a>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->name, "a");
    EXPECT_EQ(root->text, "hello");
    EXPECT_TRUE(root->children.empty());
}

TEST(MiniXmlParserTest, NestedElements) {
    const auto root = MiniXmlParser::parse(
        "<metalink><file name=\"x\"><size>10</size></file></metalink>");
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->children.size(), 1u);
    const XmlNode* file = must_child(*root, "file");
    EXPECT_EQ(*file->attr("name"), "x");
    const XmlNode* size = must_child(*file, "size");
    EXPECT_EQ(size->text, "10");
}

TEST(MiniXmlParserTest, SelfClosingElement) {
    const auto root = MiniXmlParser::parse("<r><br/><url/></r>");
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->children.size(), 2u);
    EXPECT_EQ(root->children[0]->name, "br");
    EXPECT_TRUE(root->children[0]->children.empty());
    EXPECT_EQ(root->children[0]->text, "");
}

TEST(MiniXmlParserTest, AttributesPreserveOrderAndSupportSingleQuotes) {
    const auto root = MiniXmlParser::parse(
        "<url priority='3' location='JP' type=\"http\"/>");
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->attributes.size(), 3u);
    EXPECT_EQ(root->attributes[0].first, "priority");
    EXPECT_EQ(root->attributes[0].second, "3");
    EXPECT_EQ(root->attributes[1].first, "location");
    EXPECT_EQ(root->attributes[2].first, "type");
    EXPECT_EQ(*root->attr("location"), "JP");
    EXPECT_EQ(root->attr("missing"), nullptr);
}

TEST(MiniXmlParserTest, DeclarationAndCommentsSkipped) {
    const auto root = MiniXmlParser::parse(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<!-- a comment -->\n"
        "<r><x/></r>\n"
        "<!-- trailing -->");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->name, "r");
    ASSERT_EQ(root->children.size(), 1u);
}

TEST(MiniXmlParserTest, EmptyAndCommentOnlyInputReturnNull) {
    EXPECT_EQ(MiniXmlParser::parse(""), nullptr);
    EXPECT_EQ(MiniXmlParser::parse("   \n\t "), nullptr);
    EXPECT_EQ(MiniXmlParser::parse("<!-- nothing -->"), nullptr);
}

TEST(MiniXmlParserTest, NamespacePrefixStripped) {
    const auto root = MiniXmlParser::parse(
        "<m:metalink xmlns:m=\"urn:ietf:params:xml:ns:metalink\">"
        "<m:file>mtext</m:file></m:metalink>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->name, "metalink");
    const XmlNode* file = must_child(*root, "file");
    EXPECT_EQ(file->text, "mtext");
    // 属性名同样去前缀:xmlns:m 按限定名规则存为本地名 m
    ASSERT_NE(root->attr("m"), nullptr);
    EXPECT_EQ(*root->attr("m"), "urn:ietf:params:xml:ns:metalink");
}

//==============================================================================
// 文本与实体
//==============================================================================

TEST(MiniXmlParserTest, MixedContentTextConcatenated) {
    // 元素内的直接文本(含子元素前后的)拼接进 text;子元素不混入
    const auto root = MiniXmlParser::parse("<r>A<x/>B</r>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->text, "AB");
    ASSERT_EQ(root->children.size(), 1u);
    EXPECT_EQ(root->children[0]->name, "x");
}

TEST(MiniXmlParserTest, NamedEntitiesDecoded) {
    const auto root = MiniXmlParser::parse(
        "<r>&lt;a&gt; &amp; &apos;b&apos; &quot;c&quot;</r>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->text, "<a> & 'b' \"c\"");
}

TEST(MiniXmlParserTest, NumericEntitiesDecoded) {
    // 十进制 65='A';十六进制 0x42='B';0x4E2D=中(U+4E2D,三字节 UTF-8)
    const auto root = MiniXmlParser::parse("<r>&#65;&#x42;&#x4e2d;</r>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->text, "AB\xe4\xb8\xad");
}

TEST(MiniXmlParserTest, RawUtf8Passthrough) {
    const auto root = MiniXmlParser::parse("<r>\xe4\xb8\xad\xe6\x96\x87 ok</r>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->text, "\xe4\xb8\xad\xe6\x96\x87 ok");
}

TEST(MiniXmlParserTest, CdataPreservedVerbatim) {
    // CDATA 内容原样(含 < & 与 "]]" 字符对);含 "]]>" 的内容须按
    // XML 标准拆成两段 CDATA(`]]]]><![CDATA[>`),两段文本拼接
    const auto root = MiniXmlParser::parse(
        "<r><![CDATA[raw <tags> & ]]]]><![CDATA[>9]]></r>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->text, "raw <tags> & ]]>9");
}

//==============================================================================
// Reject 系列(严格模式)
//==============================================================================

TEST(MiniXmlParserTest, RejectUnclosedTag) {
    EXPECT_THROW(MiniXmlParser::parse("<r><x>abc</x>"), XmlParseError);
    EXPECT_THROW(MiniXmlParser::parse("<r"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectMismatchedCloseTag) {
    EXPECT_THROW(MiniXmlParser::parse("<r><x></y></r>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectDuplicateAttribute) {
    EXPECT_THROW(MiniXmlParser::parse("<r a='1' a='2'/>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectCommentWithDoubleDash) {
    EXPECT_THROW(MiniXmlParser::parse("<r><!-- a -- b --></r>"),
                 XmlParseError);
}

TEST(MiniXmlParserTest, RejectUnknownProcessingInstruction) {
    EXPECT_THROW(MiniXmlParser::parse("<r><?php echo 1; ?></r>"),
                 XmlParseError);
}

TEST(MiniXmlParserTest, RejectCdataEndInText) {
    EXPECT_THROW(MiniXmlParser::parse("<r>a]]>b</r>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectUnknownEntity) {
    EXPECT_THROW(MiniXmlParser::parse("<r>&nbsp;</r>"), XmlParseError);
    EXPECT_THROW(MiniXmlParser::parse("<r>&random;</r>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectOutOfRangeNumericEntity) {
    // 0x110000 超出 Unicode 上限;0xD800 裸代理区
    EXPECT_THROW(MiniXmlParser::parse("<r>&#1114112;</r>"), XmlParseError);
    EXPECT_THROW(MiniXmlParser::parse("<r>&#xD800;</r>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectUnclosedAttributeQuote) {
    EXPECT_THROW(MiniXmlParser::parse("<r a='1/>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectMultipleRootElements) {
    EXPECT_THROW(MiniXmlParser::parse("<r/><r/>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectTrailingGarbage) {
    EXPECT_THROW(MiniXmlParser::parse("<r/>garbage"), XmlParseError);
}

TEST(MiniXmlParserTest, ErrorCarriesLineAndColumn) {
    try {
        MiniXmlParser::parse("<r>\n  <x></y>\n</r>");
        FAIL() << "expected XmlParseError";
    } catch (const XmlParseError& e) {
        EXPECT_EQ(e.line(), 2u);
        EXPECT_GT(e.column(), 0u);
        EXPECT_NE(std::string(e.what()).find("line 2"), std::string::npos);
    }
}

//==============================================================================
// 边界补遗(覆盖率批次 X):UTF-8 BOM / 多字节实体 / 属性值实体与拒绝
//==============================================================================

TEST(MiniXmlParserTest, Utf8BomSkipped) {
    const auto root = MiniXmlParser::parse("\xef\xbb\xbf<r ok=\"1\"/>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->name, "r");
}

TEST(MiniXmlParserTest, TwoByteEntityEncoding) {
    // U+00E9(é)→ 2 字节 UTF-8(C3 A9)
    const auto root = MiniXmlParser::parse("<r>caf&#xe9;</r>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->text, "caf\xc3\xa9");
}

TEST(MiniXmlParserTest, FourByteEntityEncoding) {
    // U+1F600(😀)→ 4 字节 UTF-8(F0 9F 98 80)
    const auto root = MiniXmlParser::parse("<r>&#x1f600;</r>");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->text, "\xf0\x9f\x98\x80");
}

TEST(MiniXmlParserTest, RejectInvalidHexDigitInEntity) {
    EXPECT_THROW(MiniXmlParser::parse("<r>&#xG42;</r>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectAttributeMissingEquals) {
    // 属性名后缺 '='(expect("=") 失败路径)
    EXPECT_THROW(MiniXmlParser::parse("<r a \"1\"/>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectAttrValueMissingQuote) {
    EXPECT_THROW(MiniXmlParser::parse("<r a=x/>"), XmlParseError);
}

TEST(MiniXmlParserTest, AttrValueEntitiesDecoded) {
    const auto root = MiniXmlParser::parse("<r a=\"x&amp;y&#65;z\"/>");
    ASSERT_NE(root, nullptr);
    ASSERT_NE(root->attr("a"), nullptr);
    EXPECT_EQ(*root->attr("a"), "x&yAz");
}

TEST(MiniXmlParserTest, RejectLtInAttrValue) {
    EXPECT_THROW(MiniXmlParser::parse("<r a=\"a<b\"/>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectDoctype) {
    EXPECT_THROW(MiniXmlParser::parse("<!DOCTYPE note SYSTEM \"x.dtd\"><r/>"),
                 XmlParseError);
}

TEST(MiniXmlParserTest, RejectUnknownPiAtProlog) {
    // xml 伪前缀(xmlfoo):prolog 位置的未知 PI 拒绝
    EXPECT_THROW(MiniXmlParser::parse("<?xmlfoo bar?><r/>"), XmlParseError);
}

TEST(MiniXmlParserTest, RejectDeclarationMissingVersion) {
    // XML 声明缺 version 属性(期望 "version" 字面量失败)
    EXPECT_THROW(MiniXmlParser::parse("<?xml encoding=\"utf-8\"?><r/>"),
                 XmlParseError);
}
