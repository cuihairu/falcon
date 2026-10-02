/**
 * @file s3_list_xml.hpp
 * @brief S3 ListBucketResult XML 解析（ListObjectsV2 协议面）
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 真 S3/MinIO/R2/B2/Wasabi/GCS 对 list-type=2 一律应答 XML——此前仅有的
 * nlohmann JSON 解析只对早期测试 mock 的 {"Contents":[...]} 通，与真实
 * 协议面不符（MinIO 实测 list_directory 恒空）。本解析基于共享扫描器
 * src/xml_scan.hpp，提取：
 *   <Contents><Key/><Size/><LastModified/><ETag/><StorageClass/></Contents>
 *   <CommonPrefixes><Prefix/></CommonPrefixes>
 * 字段缺失按空处理（不同实现/存储类别的应答字段裁剪不一）。
 */

#pragma once

#include "xml_scan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace falcon::detail {

struct S3ListEntry {
    bool is_prefix = false;
    std::string key;        ///< 对象 key，或去尾斜杠后的公共前缀
    uint64_t size = 0;
    std::string last_modified;
    std::string etag;
    std::string storage_class;
};

/// 解析 ListBucketResult 应答。仅当含 ListBucketResult 标签时视为 XML 形态
/// （空桶也返回该标签、零条目成功）；否则返回 false 交由调用方走历史兼容路径。
inline bool parse_s3_list_bucket_result(const std::string& xml,
                                        std::vector<S3ListEntry>* out) {
    if (xml.find("ListBucketResult") == std::string::npos) {
        return false;
    }
    out->clear();

    XmlElement elem;
    size_t pos = 0;
    while (xml_find_element(xml, pos, "Contents", &elem)) {
        pos = elem.next;
        S3ListEntry entry;
        XmlElement field;
        if (xml_find_element(xml, elem.content_begin, "Key", &field) &&
            field.content_end <= elem.content_end) {
            entry.key = std::move(field.text);
        }
        if (xml_find_element(xml, elem.content_begin, "Size", &field) &&
            field.content_end <= elem.content_end) {
            try {
                entry.size = static_cast<uint64_t>(std::stoull(field.text));
            } catch (...) {
                entry.size = 0;
            }
        }
        if (xml_find_element(xml, elem.content_begin, "LastModified", &field) &&
            field.content_end <= elem.content_end) {
            entry.last_modified = std::move(field.text);
        }
        if (xml_find_element(xml, elem.content_begin, "ETag", &field) &&
            field.content_end <= elem.content_end) {
            entry.etag = std::move(field.text);
        }
        if (xml_find_element(xml, elem.content_begin, "StorageClass", &field) &&
            field.content_end <= elem.content_end) {
            entry.storage_class = std::move(field.text);
        }
        if (!entry.key.empty()) {
            out->push_back(std::move(entry));
        }
    }

    pos = 0;
    while (xml_find_element(xml, pos, "CommonPrefixes", &elem)) {
        pos = elem.next;
        XmlElement prefix;
        if (xml_find_element(xml, elem.content_begin, "Prefix", &prefix) &&
            prefix.content_end <= elem.content_end) {
            std::string p = std::move(prefix.text);
            if (!p.empty() && p.back() == '/') {
                p.pop_back();
            }
            if (!p.empty()) {
                S3ListEntry entry;
                entry.is_prefix = true;
                entry.key = std::move(p);
                out->push_back(std::move(entry));
            }
        }
    }
    return true;
}

}  // namespace falcon::detail
