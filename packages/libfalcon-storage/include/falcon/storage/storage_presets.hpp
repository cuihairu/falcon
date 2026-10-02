/**
 * @file storage_presets.hpp
 * @brief 对象存储/网盘连接预设表（国外厂商补全 + 国内保持）
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 支持矩阵的配置面：每家厂商一条预设（endpoint/region 模板 + 说明 +
 * 密钥页指引），GUI 从这里生成预设下拉。协议面分两类：
 *   - category "s3"：S3 兼容 XML API（ListObjectsV2），由 S3Browser 承接
 *     ——AWS S3 / Cloudflare R2 / Backblaze B2 / Wasabi / GCS(HMAC) /
 *     MinIO 等自建 S3 兼容全部同协议覆盖；
 *   - category "webdav"：WebDAV/ALIST，由 WebDavBrowser 承接——原生
 *     WebDAV（坚果云等）与 ALIST 聚合的网盘（Google Drive/OneDrive/
 *     Dropbox/pCloud/Mega 及国内盘）。
 * Azure Blob 无 S3 兼容协议面，经 ALIST 网关纳入（预设内注明）。
 * endpoint 模板中 <...> 为用户需替换的占位段。
 */

#pragma once

#include <string>
#include <vector>

namespace falcon {

/**
 * @brief 单条连接预设
 */
struct StoragePreset {
    std::string id;                ///< 稳定标识（GUI/配置持久化用）
    std::string display_name;      ///< 下拉显示名
    std::string category;          ///< "s3" | "webdav"
    std::string browser_protocol;  ///< BrowserFactory 协议键："s3" | "webdav"
    std::string description;       ///< 一句话说明（鉴权形态/边界）
    std::string endpoint_template; ///< endpoint 模板（空 = 用户自填）
    std::string region_default;    ///< 默认 region（无 region 概念则空）
    std::string username_hint;     ///< 用户名字段应填什么（可空）
    std::string docs_url;          ///< 控制台/密钥获取页
};

/// 全部预设（国内外一体，按 category 分组内相对稳定排序）
std::vector<StoragePreset> storage_presets();

/// 按 id 查预设；不存在返回 nullptr
const StoragePreset* find_storage_preset(const std::string& id);

} // namespace falcon
