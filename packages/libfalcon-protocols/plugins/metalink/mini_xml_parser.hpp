/**
 * @file mini_xml_parser.hpp
 * @brief 手写严格模式 mini XML 解析器(Metalink 专用,零第三方依赖)
 * @author Falcon Team
 * @date 2026-09-18
 *
 * 为什么手写:仓库无 XML 库依赖,Metalink 只需 XML 的一个受限子集
 * (元素/属性/文本/CDATA/注释/声明)。引入 libxml2 换来的是体积与
 * 三平台 CI 负担;regex 方案(旧占位插件)被证明不可行。
 *
 * 覆盖范围(严格模式,畸形输入抛 XmlParseError 且带行列号):
 * - XML 声明 <?xml ... ?>、注释 <!-- -->、CDATA <![CDATA[...]]>
 * - 嵌套元素、自闭合元素、单/双引号属性
 * - 五种命名实体(&lt; &gt; &amp; &apos; &quot;)+ &#dec; + &#xhex;
 * - 命名空间前缀:元素/属性名按去前缀后的本地名暴露(m:name → name)
 * - 顶层只允许一个根元素(声明/注释/空白除外)
 *
 * 明确不支持(遇到即报错,不做宽松恢复):DOCTYPE、处理指令
 * (声明除外)、实体自定义定义、非法字符引用。
 */

#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace falcon::protocols::metalink {

/**
 * @class XmlParseError
 * @brief 解析失败异常,携带行列号便于诊断
 */
class XmlParseError : public std::runtime_error {
public:
    XmlParseError(const std::string& message, std::size_t line,
                  std::size_t column)
        : std::runtime_error(message + " (line " + std::to_string(line) +
                             ", col " + std::to_string(column) + ")")
        , line_(line)
        , column_(column) {}

    [[nodiscard]] std::size_t line() const noexcept { return line_; }
    [[nodiscard]] std::size_t column() const noexcept { return column_; }

private:
    std::size_t line_;
    std::size_t column_;
};

/**
 * @struct XmlNode
 * @brief 解析树节点(元素)
 *
 * name 是去命名空间前缀后的本地名;text 是该元素直接子文本
 * (实体已解码,CDATA 原样并入);attributes 保序。
 */
struct XmlNode {
    std::string name;
    std::string text;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::unique_ptr<XmlNode>> children;

    /// 第一个本地名为 name 的直接子元素,无则 nullptr
    [[nodiscard]] const XmlNode* child(const std::string& name) const;

    /// 所有本地名为 name 的直接子元素
    [[nodiscard]] std::vector<const XmlNode*> children_of(
        const std::string& name) const;

    /// 属性值,无则返回 nullptr
    [[nodiscard]] const std::string* attr(const std::string& name) const;

    /// 直接子文本拼接(等价 text,语义化别名)
    [[nodiscard]] const std::string& all_text() const { return text; }
};

/**
 * @class MiniXmlParser
 * @brief 严格模式 XML 解析器
 *
 * 用法: auto root = MiniXmlParser::parse(xml_text);
 *       if (root && root->name == "metalink") { ... }
 * 空输入/纯注释输入返回 nullptr;其余畸形输入抛 XmlParseError。
 */
class MiniXmlParser {
public:
    /// 解析整个文档,返回根元素节点;空文档(无元素)返回 nullptr
    static std::unique_ptr<XmlNode> parse(const std::string& input);
};

} // namespace falcon::protocols::metalink
