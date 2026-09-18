/**
 * @file mini_xml_parser.cpp
 * @brief 手写严格模式 mini XML 解析器实现
 * @author Falcon Team
 * @date 2026-09-18
 */

#include "mini_xml_parser.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cctype>

namespace falcon::protocols::metalink {

//==============================================================================
// XmlNode
//==============================================================================

const XmlNode* XmlNode::child(const std::string& name) const {
    for (const auto& c : children) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::children_of(
    const std::string& name) const {
    std::vector<const XmlNode*> found;
    for (const auto& c : children) {
        if (c->name == name) found.push_back(c.get());
    }
    return found;
}

const std::string* XmlNode::attr(const std::string& name) const {
    for (const auto& [n, v] : attributes) {
        if (n == name) return &v;
    }
    return nullptr;
}

namespace {

//==============================================================================
// Scanner:字节级扫描 + 行列号跟踪
//==============================================================================

class Scanner {
public:
    explicit Scanner(const std::string& input) : input_(input) {}

    [[nodiscard]] bool eof() const { return pos_ >= input_.size(); }

    [[nodiscard]] char peek(std::size_t offset = 0) const {
        const std::size_t at = pos_ + offset;
        return at < input_.size() ? input_[at] : '\0';
    }

    /// 从当前位置起匹配字面前缀(不消费)
    [[nodiscard]] bool starts_with(const char* literal) const {
        const std::size_t len = std::strlen(literal);
        return input_.compare(pos_, len, literal) == 0;
    }

    /// 消费 n 个字节并同步行列号
    void advance(std::size_t n = 1) {
        for (std::size_t i = 0; i < n && pos_ < input_.size(); ++i) {
            if (input_[pos_] == '\n') {
                ++line_;
                column_ = 1;
            } else {
                ++column_;
            }
            ++pos_;
        }
    }

    /// 跳过空白;开头一次性消费 UTF-8 BOM
    void skip_insignificant() {
        if (pos_ == 0 && input_.size() >= 3 &&
            static_cast<unsigned char>(input_[0]) == 0xEF &&
            static_cast<unsigned char>(input_[1]) == 0xBB &&
            static_cast<unsigned char>(input_[2]) == 0xBF) {
            advance(3);
        }
        while (!eof() &&
               std::isspace(static_cast<unsigned char>(peek())) != 0) {
            advance();
        }
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw XmlParseError(message, line_, column_);
    }

    [[nodiscard]] std::size_t line() const { return line_; }
    [[nodiscard]] std::size_t column() const { return column_; }
    [[nodiscard]] std::size_t pos() const { return pos_; }

private:
    const std::string& input_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

bool is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
           c == ':';
}

bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

/// 限定名去命名空间前缀:m:name → name;无前缀原样返回
std::string local_name(const std::string& qname) {
    const auto colon = qname.rfind(':');
    return colon == std::string::npos ? qname : qname.substr(colon + 1);
}

/// 码点 → UTF-8(字符引用可产生任意 Unicode 码点)
void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool hex_value(char c, std::uint32_t& out) {
    if (c >= '0' && c <= '9') {
        out = static_cast<std::uint32_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = static_cast<std::uint32_t>(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = static_cast<std::uint32_t>(c - 'A' + 10);
        return true;
    }
    return false;
}

/// 解析数字字面量(十进制或十六进制);防溢出且拒绝超 Unicode 上限
bool parse_number(const std::string& digits, bool hex, std::uint32_t& out) {
    if (digits.empty()) return false;
    const std::uint32_t base = hex ? 16u : 10u;
    std::uint32_t value = 0;
    for (const char c : digits) {
        std::uint32_t d = 0;
        if (hex) {
            if (!hex_value(c, d)) return false;
        } else {
            if (c < '0' || c > '9') return false;
            d = static_cast<std::uint32_t>(c - '0');
        }
        // 乘基前判溢出:value*base + d 不得超过 Unicode 上限
        if (value > (0x10FFFFu - d) / base) return false;
        value = value * base + d;
    }
    out = value;
    return true;
}

//==============================================================================
// 语法单元消费
//==============================================================================

/// 消费字面字符串;不匹配时报错(不消费)
void expect(Scanner& s, const char* literal) {
    if (!s.starts_with(literal)) {
        s.fail(std::string("期望 '") + literal + "'");
    }
    s.advance(std::strlen(literal));
}

/// 实体级错误的统一收口(行列号锚在 '&' 处)
[[noreturn]] void entity_fail(const std::string& why, std::size_t line,
                              std::size_t col) {
    throw XmlParseError(why, line, col);
}

/// Scanner 停在 '&',消费完整实体引用并返回解码结果
std::string consume_entity(Scanner& s) {
    const std::size_t line = s.line();
    const std::size_t col = s.column();

    s.advance(); // '&'
    std::string name;
    while (!s.eof() && s.peek() != ';' && name.size() <= 10) {
        name.push_back(s.peek());
        s.advance();
    }
    if (s.eof() || s.peek() != ';') entity_fail("未闭合的实体引用", line, col);
    s.advance(); // ';'

    if (name == "lt") return "<";
    if (name == "gt") return ">";
    if (name == "amp") return "&";
    if (name == "apos") return "'";
    if (name == "quot") return "\"";

    // 字符引用 &#123; / &#x1A;
    if (!name.empty() && name[0] == '#') {
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::string digits = name.substr(hex ? 2 : 1);
        std::uint32_t cp = 0;
        if (!parse_number(digits, hex, cp)) {
            entity_fail("非法字符引用", line, col);
        }
        if (cp == 0 || (cp < 0x20 && cp != 0x9 && cp != 0xA && cp != 0xD) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            // XML 1.0 Char 产生式:排除控制字符与裸代理区(无法编码为
            // 合法 UTF-8,落盘即损坏)
            entity_fail("禁止的字符引用", line, col);
        }
        std::string decoded;
        append_utf8(decoded, cp);
        return decoded;
    }

    entity_fail("未知实体: '" + name + "'", line, col);
}

/// 读取限定名(可含命名空间前缀)
std::string consume_name(Scanner& s) {
    if (s.eof() || !is_name_start(s.peek())) s.fail("期望 XML 名字");
    std::string name;
    while (!s.eof() && is_name_char(s.peek())) {
        name.push_back(s.peek());
        s.advance();
    }
    return name;
}

/// 消费引号包裹的属性值(实体解码;属性值内 '<' 非法)
std::string consume_attr_value(Scanner& s) {
    if (s.eof() || (s.peek() != '"' && s.peek() != '\'')) {
        s.fail("期望属性值引号");
    }
    const char quote = s.peek();
    s.advance();
    std::string value;
    while (!s.eof() && s.peek() != quote) {
        if (s.peek() == '&') {
            value += consume_entity(s);
        } else if (s.peek() == '<') {
            s.fail("属性值中不允许 '<'");
        } else {
            value.push_back(s.peek());
            s.advance();
        }
    }
    if (s.eof()) s.fail("属性值未闭合");
    s.advance(); // 闭引号
    return value;
}

/// 消费 <!-- ... -->;注释体内 '--' 严格拒绝(XML 1.0 §2.5)
void consume_comment(Scanner& s) {
    expect(s, "<!--");
    while (true) {
        if (s.eof()) s.fail("注释未闭合");
        if (s.starts_with("-->")) {
            s.advance(3);
            return;
        }
        if (s.starts_with("--")) s.fail("注释中不允许 '--'");
        s.advance();
    }
}

/// 消费 <?xml ... ?> 声明或拒绝其他处理指令
void consume_pi(Scanner& s) {
    const std::size_t line = s.line();
    const std::size_t col = s.column();
    expect(s, "<?");
    if (!s.starts_with("xml")) {
        throw XmlParseError("不支持的处理指令", line, col);
    }
    s.advance(3);
    // xml 后必须是空白或 '?>'(防止匹配 xmlfoo 之类的伪声明)
    if (!s.eof() && s.peek() != '?' &&
        std::isspace(static_cast<unsigned char>(s.peek())) == 0) {
        throw XmlParseError("不支持的处理指令", line, col);
    }
    // 收集声明体(XML 1.0 [23] XMLDecl::= '<?xml' VersionInfo ...):
    // version 属性必填,只写 encoding/standalone 的伪声明拒绝
    std::string body;
    while (true) {
        if (s.eof()) throw XmlParseError("声明未闭合", line, col);
        if (s.starts_with("?>")) break;
        body.push_back(s.peek());
        s.advance();
    }
    s.advance(2);
    bool has_version = false;
    for (std::size_t i = 0; i + 7 <= body.size(); ++i) {
        if (body.compare(i, 7, "version") != 0) continue;
        const bool boundary = i == 0 ||
                              std::isspace(static_cast<unsigned char>(body[i - 1])) != 0;
        if (!boundary) continue;
        std::size_t j = i + 7;
        while (j < body.size() &&
               std::isspace(static_cast<unsigned char>(body[j])) != 0) ++j;
        if (j + 1 < body.size() && body[j] == '=' &&
            (body[j + 1] == '"' || body[j + 1] == '\'')) {
            has_version = true;
            break;
        }
    }
    if (!has_version) {
        throw XmlParseError("声明缺少 version 属性", line, col);
    }
}

/// 消费属性序列直到 '>' 或 '/>';属性名去前缀,重名严格拒绝
void consume_attributes(Scanner& s, XmlNode& elem) {
    while (true) {
        s.skip_insignificant();
        if (s.eof()) s.fail("元素未闭合");
        if (s.peek() == '>' || s.starts_with("/>")) return;
        const std::string qname = consume_name(s);
        s.skip_insignificant();
        expect(s, "=");
        s.skip_insignificant();
        const std::string lname = local_name(qname);
        for (const auto& [n, v] : elem.attributes) {
            (void)v;
            if (n == lname) s.fail("重复属性: '" + lname + "'");
        }
        elem.attributes.emplace_back(lname, consume_attr_value(s));
    }
}

//==============================================================================
// 元素/内容递归下降
//==============================================================================

class Parser {
public:
    explicit Parser(const std::string& input) : s_(input) {}

    /// 文档级解析:prolog + 恰一个根元素 + epilog
    std::unique_ptr<XmlNode> parse_document() {
        s_.skip_insignificant();

        std::unique_ptr<XmlNode> root;
        while (!s_.eof()) {
            if (s_.starts_with("<?")) {
                consume_pi(s_);
            } else if (s_.starts_with("<!--")) {
                consume_comment(s_);
            } else if (s_.starts_with("<!DOCTYPE")) {
                s_.fail("不支持 DOCTYPE");
            } else if (s_.peek() == '<') {
                if (root) s_.fail("文档只允许一个根元素");
                root = parse_element();
            } else {
                // 根外只允许空白(已跳过)——到达这里即非空白内容
                s_.fail("根元素外不允许文本");
            }
            s_.skip_insignificant();
        }
        return root;
    }

private:
    /// Scanner 停在 '<';解析 <name .../> 或 <name>content</name>
    std::unique_ptr<XmlNode> parse_element() {
        s_.advance(); // '<'
        auto elem = std::make_unique<XmlNode>();
        elem->name = local_name(consume_name(s_));
        consume_attributes(s_, *elem);

        if (s_.starts_with("/>")) {
            s_.advance(2);
            return elem;
        }
        expect(s_, ">");
        parse_content(*elem);

        // 结束标签
        expect(s_, "</");
        const std::string closing = consume_name(s_);
        if (local_name(closing) != elem->name) {
            s_.fail("结束标签与起始标签不匹配: '" + closing + "' ≠ '" +
                    elem->name + "'");
        }
        s_.skip_insignificant();
        expect(s_, ">");
        return elem;
    }

    void parse_content(XmlNode& elem) {
        while (true) {
            if (s_.eof()) s_.fail("元素未闭合: '" + elem.name + "'");
            if (s_.starts_with("</")) return; // 交回 parse_element 收口

            if (s_.starts_with("<!--")) {
                consume_comment(s_);
            } else if (s_.starts_with("<![CDATA[")) {
                consume_cdata(elem);
            } else if (s_.starts_with("<?")) {
                consume_pi(s_);
            } else if (s_.peek() == '<') {
                elem.children.push_back(parse_element());
            } else {
                consume_text(elem);
            }
        }
    }

    /// CDATA 内容原样并入元素文本(不解码实体)
    void consume_cdata(XmlNode& elem) {
        expect(s_, "<![CDATA[");
        while (true) {
            if (s_.eof()) s_.fail("CDATA 未闭合");
            if (s_.starts_with("]]>")) {
                s_.advance(3);
                return;
            }
            elem.text.push_back(s_.peek());
            s_.advance();
        }
    }

    /// 普通文本:实体解码并入元素文本;']]>' 在文本中非法(XML 1.0 §2.4)
    void consume_text(XmlNode& elem) {
        while (!s_.eof() && s_.peek() != '<') {
            if (s_.starts_with("]]>")) s_.fail("文本中不允许 ']]>'");
            if (s_.peek() == '&') {
                elem.text += consume_entity(s_);
            } else {
                elem.text.push_back(s_.peek());
                s_.advance();
            }
        }
    }

    Scanner s_;
};

} // namespace

//==============================================================================
// MiniXmlParser
//==============================================================================

std::unique_ptr<XmlNode> MiniXmlParser::parse(const std::string& input) {
    Parser parser(input);
    return parser.parse_document();
}

} // namespace falcon::protocols::metalink
