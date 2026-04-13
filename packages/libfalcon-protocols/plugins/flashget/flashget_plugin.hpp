/**
 * @file flashget_plugin.hpp
 * @brief 快车 FlashGet 协议插件
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
 * @class FlashGetPlugin
 * @brief 快车 FlashGet 协议处理器
 *
 * 支持 flashget:// 格式的链接解析和下载
 * FlashGet 链接通常包含 URL 和引用站点信息
 */
class FlashGetPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    FlashGetPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~FlashGetPlugin() = default;

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "flashget"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief 解析快车链接
     * @param flashgetUrl 快车链接
     * @return 解析后的原始 URL
     */
    std::string parseFlashGetUrl(const std::string& flashgetUrl);

    /**
     * @brief 解码快车链接
     * @param encoded 编码部分
     * @param referrer 引用页面（如果有）
     * @return 原始 URL
     */
    std::string decodeFlashGetUrl(const std::string& encoded,
                                 const std::string& referrer = "");

    /**
     * @brief URL解码
     * @param encoded 编码的URL
     * @return 解码后的URL
     */
    std::string urlDecode(const std::string& encoded);

    /**
     * @brief 解析镜像链接
     * @param url 原始URL
     * @return 镜像列表
     */
    std::vector<std::string> parseMirrors(const std::string& url);

  /**
   * @brief Base64解码
   * @param encoded 编码字符串
   * @return 解码后的字符串
   */
  std::string base64_decode(const std::string& encoded);
};

} // namespace plugins
} // namespace falcon