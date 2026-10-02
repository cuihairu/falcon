/**
 * @file xml_scan.hpp
 * @brief 手工 XML 元素扫描器（storage 内部共享，零新增依赖）
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 服务于真实协议面的 XML 解析：
 *   - S3 ListBucketResult（ListObjectsV2——真 S3/MinIO/R2/B2/Wasabi/GCS 应答）
 *   - WebDAV 207 multistatus（PROPFIND——ALIST/原生 WebDAV 服务器应答）
 *
 * 只覆盖这两个协议面用到的子集：命名空间前缀容忍（取 ':' 后局部名匹配）、
 * 属性跳过（引号内的 '>' 不误判为标签结束）、五个预定义实体 + 数字引用解码。
 * 刻意不做通用 XML 解析、不引入 XML 库依赖（vcpkg 面外扩）——两处协议面的
 * 应答结构都是服务端机器生成的规范形态，无 CDATA/PI/注释嵌套需求。
 */

#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>

namespace falcon::detail {

/// 取标签局部名（命名空间前缀之后）："d:getcontentlength" → "getcontentlength"
inline std::string xml_local_name(const std::string& tag) {
    const auto colon = tag.rfind(':');
    return colon == std::string::npos ? tag : tag.substr(colon + 1);
}

/// 解码五个预定义实体与十/十六进制数字引用；未知实体原样保留（不猜测）。
inline std::string xml_decode_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        const auto semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out.push_back('&');  // 无界引用：按普通 '&' 处理
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "lt") {
            out.push_back('<');
        } else if (ent == "gt") {
            out.push_back('>');
        } else if (ent == "amp") {
            out.push_back('&');
        } else if (ent == "quot") {
            out.push_back('"');
        } else if (ent == "apos") {
            out.push_back('\'');
        } else if (ent.size() >= 3 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const std::string digits = ent.substr(hex ? 2 : 1);
            long code = -1;
            try {
                code = std::stol(digits, nullptr, hex ? 16 : 10);
            } catch (...) {
                code = -1;
            }
            if (code > 0 && code < 128) {
                out.push_back(static_cast<char>(code));
            } else {
                out += s.substr(i, semi - i + 1);  // 越界/非 ASCII：原样保留
            }
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

/// 一次元素提取的结果
struct XmlElement {
    size_t open_start = std::string::npos;  ///< 开标签 '<' 位置
    size_t content_begin = 0;               ///< inner text 起点（开标签 '>' 之后）
    size_t content_end = 0;                 ///< inner text 终点（闭标签 '<' 位置）
    size_t next = std::string::npos;        ///< 扫描续点（闭标签之后）
    std::string text;                       ///< 实体解码后的 inner text
};

/// 从 from 起找第一个局部名 == local_name 的元素并提取 inner text。
/// 自闭合标签（<Prefix/>）视为空文本元素；未找到返回 false。
inline bool xml_find_element(const std::string& xml, size_t from,
                             const std::string& local_name, XmlElement* out) {
    size_t pos = from;
    while (true) {
        const auto lt = xml.find('<', pos);
        if (lt == std::string::npos) {
            return false;
        }
        if (lt + 1 >= xml.size()) {
            return false;
        }
        const char c = xml[lt + 1];
        if (c == '/' || c == '!' || c == '?') {
            pos = lt + 1;  // 闭标签/注释/CDATA/PI：跳过
            continue;
        }
        // 读完整标签名（到空白、'>'、'/' 为止）
        size_t i = lt + 1;
        while (i < xml.size() && xml[i] != ' ' && xml[i] != '\t' &&
               xml[i] != '\r' && xml[i] != '\n' && xml[i] != '>' && xml[i] != '/') {
            ++i;
        }
        const std::string full_tag = xml.substr(lt + 1, i - lt - 1);
        if (xml_local_name(full_tag) != local_name) {
            pos = lt + 1;
            continue;
        }
        // 跳过属性找 '>'（引号内的 '>' 不是标签结束）
        bool in_quote = false;
        while (i < xml.size()) {
            const char ch = xml[i];
            if (ch == '"') {
                in_quote = !in_quote;
            } else if (ch == '>' && !in_quote) {
                break;
            }
            ++i;
        }
        if (i >= xml.size()) {
            return false;
        }
        if (i > lt && xml[i - 1] == '/') {
            out->open_start = lt;
            out->content_begin = out->content_end = i + 1;
            out->next = i + 1;
            out->text.clear();
            return true;
        }
        const std::string close = "</" + full_tag + ">";
        const auto close_pos = xml.find(close, i + 1);
        if (close_pos == std::string::npos) {
            return false;
        }
        out->open_start = lt;
        out->content_begin = i + 1;
        out->content_end = close_pos;
        out->next = close_pos + close.size();
        out->text = xml_decode_entities(xml.substr(i + 1, close_pos - i - 1));
        return true;
    }
}

}  // namespace falcon::detail
