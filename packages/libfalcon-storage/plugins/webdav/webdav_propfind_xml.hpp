/**
 * @file webdav_propfind_xml.hpp
 * @brief WebDAV PROPFIND 207 multistatus 解析（协议面）
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 原生 WebDAV 服务器与 ALIST 对 PROPFIND 一律应答 207 multistatus XML。
 * 本解析基于共享扫描器 src/xml_scan.hpp，提取每个 <response> 的：
 *   <href/>（百分号编码的路径或绝对 URL）
 *   <propstat><prop> 内的 displayname/getcontentlength/getlastmodified/
 *   resourcetype（含 <collection/> 即目录）/getetag/getcontenttype
 *   以及配额字段 quota-used-bytes/quota-available-bytes（RFC 4331）。
 * 命名空间前缀（d:/D:/DAV: 等）由扫描器按局部名容忍；字段缺失按空处理
 * （ALIST 等实现会对不认识的属性回 404 propstat）。
 */

#pragma once

#include "xml_scan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace falcon::detail {

struct DavEntry {
    std::string href;        ///< 原始 href（未解码）
    bool is_dir = false;
    std::string name;        ///< displayname 优先，否则 href 末段（已解码）
    std::string path;        ///< 由 href 解码出的路径（已剥查询串）
    uint64_t size = 0;
    std::string last_modified;
    std::string etag;
    std::string content_type;
};

/// %XX 解码（href 还原；不做 '+' 转空格——路径里 '+' 是字面字符）
inline std::string percent_decode(const std::string& s) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_val(s[i + 1]);
            const int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

/// href → 纯路径：绝对 URL 剥掉 scheme://host[:port]，保留 path；剥查询串
inline std::string href_to_path(const std::string& href) {
    std::string s = href;
    const auto scheme_end = s.find("://");
    if (scheme_end != std::string::npos) {
        const auto path_start = s.find('/', scheme_end + 3);
        s = (path_start == std::string::npos) ? "/" : s.substr(path_start);
    }
    const auto query = s.find('?');
    if (query != std::string::npos) {
        s = s.substr(0, query);
    }
    return percent_decode(s);
}

/// 单个 <prop> 内取字段：边界限制在 prop 元素内部，防跨 propstat 误取
inline std::string prop_field(const std::string& xml, const XmlElement& prop,
                              const std::string& local_name) {
    XmlElement field;
    if (xml_find_element(xml, prop.content_begin, local_name, &field) &&
        field.content_end <= prop.content_end) {
        return field.text;
    }
    return "";
}

/// 解析 multistatus 应答。仅当含 multistatus 标签时视为 207 形态；
/// 否则返回 false 交由调用方按错误路径处理。
inline bool parse_dav_propfind_response(const std::string& xml,
                                        std::vector<DavEntry>* out) {
    if (xml.find("multistatus") == std::string::npos) {
        return false;
    }
    out->clear();

    XmlElement response;
    size_t pos = 0;
    while (xml_find_element(xml, pos, "response", &response)) {
        pos = response.next;
        DavEntry entry;

        XmlElement href;
        if (xml_find_element(xml, response.content_begin, "href", &href) &&
            href.content_end <= response.content_end) {
            entry.href = href.text;
        }
        if (entry.href.empty()) {
            continue;  // 无 href 的 response 无法定位资源，丢弃
        }
        entry.path = href_to_path(entry.href);

        // resourcetype 的 text 内含 <collection/>（扫描器按文本保留）
        XmlElement propstat;
        size_t ps_pos = response.content_begin;
        while (xml_find_element(xml, ps_pos, "propstat", &propstat) &&
               propstat.content_begin < response.content_end) {
            ps_pos = propstat.next;
            XmlElement prop;
            if (!xml_find_element(xml, propstat.content_begin, "prop", &prop) ||
                prop.content_end > response.content_end) {
                continue;
            }
            const std::string rt = prop_field(xml, prop, "resourcetype");
            if (rt.find("collection") != std::string::npos) {
                entry.is_dir = true;
            }
            const std::string length = prop_field(xml, prop, "getcontentlength");
            if (!length.empty()) {
                try {
                    entry.size = static_cast<uint64_t>(std::stoull(length));
                } catch (...) {
                    entry.size = 0;
                }
            }
            const std::string modified =
                prop_field(xml, prop, "getlastmodified");
            if (!modified.empty()) {
                entry.last_modified = modified;
            }
            const std::string displayname =
                prop_field(xml, prop, "displayname");
            if (!displayname.empty()) {
                entry.name = displayname;
            }
            const std::string etag = prop_field(xml, prop, "getetag");
            if (!etag.empty()) {
                entry.etag = etag;
            }
            const std::string ctype = prop_field(xml, prop, "getcontenttype");
            if (!ctype.empty()) {
                entry.content_type = ctype;
            }
        }

        out->push_back(std::move(entry));
    }
    return true;
}

/// RFC 4331 配额字段（PROPFIND Depth:0 的 prop 内）；缺失返回 false
inline bool parse_dav_quota(const std::string& xml, uint64_t* used,
                            uint64_t* available) {
    XmlElement elem;
    bool found = false;
    if (xml_find_element(xml, 0, "quota-used-bytes", &elem)) {
        try {
            *used = static_cast<uint64_t>(std::stoull(elem.text));
            found = true;
        } catch (...) {
        }
    }
    if (xml_find_element(xml, 0, "quota-available-bytes", &elem)) {
        try {
            *available = static_cast<uint64_t>(std::stoull(elem.text));
            found = true;
        } catch (...) {
        }
    }
    return found;
}

}  // namespace falcon::detail
