/**
 * @file qqdl_plugin.hpp
 * @brief QQ旋风 QQDL 协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include "../base_protocol_plugin.hpp"
#include <string>
#include <vector>
#include <memory>

namespace falcon {
namespace plugins {

/**
 * @class QQDLPlugin
 * @brief QQ旋风 QQDL 协议处理器
 *
 * 支持 qqlink:// 格式的链接解析和下载
 * QQ旋风链接是对原始 URL 经过特定编码和加密后的结果
 */
class QQDLPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    QQDLPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~QQDLPlugin() = default;

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "qqdl"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief 解析QQ旋风链接
     * @param qqUrl QQ旋风链接（qqlink://）
     * @return 解析后的原始 URL
     */
    std::string parseQQUrl(const std::string& qqUrl);

    /**
     * @brief 解码QQ旋风链接
     * @param encoded 编码部分
     * @return 原始 URL
     */
    std::string decodeQQUrl(const std::string& encoded);

    /**
     * @brief 验证GID（QQ旋风的群组ID）
     * @param gid 群组ID
     * @return 是否有效
     */
    bool isValidGid(const std::string& gid);

    /**
     * @brief 解析快速链接格式
     * @param url 原始链接
     * @return 解析后的下载信息
     */
    struct DownloadInfo {
        std::string url;
        std::string filename;
        std::string filesize;
        std::string cid;
    };

    DownloadInfo parseDownloadInfo(const std::string& url);

  /**
   * @brief Base64解码
   * @param encoded 编码字符串
   * @return 解码后的字符串
   */
  std::string base64_decode(const std::string& encoded);
};

} // namespace plugins
} // namespace falcon