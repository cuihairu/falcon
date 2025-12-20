/**
 * @file thunder_plugin.hpp
 * @brief 迅雷 Thunder 协议插件
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
 * @class ThunderPlugin
 * @brief 迅雷 Thunder 协议处理器
 *
 * 支持 thunder:// 和 thunderxl:// 格式的链接解析和下载
 * 迅雷链接是对原始 URL 经过 Base64 编码和特定算法处理后的结果
 */
class ThunderPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    ThunderPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~ThunderPlugin() = default;

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "thunder"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief 解析迅雷链接
     * @param thunderUrl 迅雷链接（thunder:// 或 thunderxl://）
     * @return 解析后的原始 URL
     */
    std::string parseThunderUrl(const std::string& thunderUrl);

    /**
     * @brief 解码经典迅雷链接（thunder://）
     * @param encoded 编码部分
     * @return 原始 URL
     */
    std::string decodeClassicThunder(const std::string& encoded);

    /**
     * @brief 解码迅雷离线链接（thunderxl://）
     * @param encoded 编码部分
     * @return 原始 URL 或下载信息
     */
    std::string decodeThunderXL(const std::string& encoded);

    /**
     * @brief 验证解析后的 URL
     * @param url 原始 URL
     * @return 是否有效
     */
    bool isValidUrl(const std::string& url);

    /**
     * @brief 获取链接类型
     * @param url 迅雷链接
     * @return 链接类型（"classic", "xl", "private"）
     */
    std::string getLinkType(const std::string& url);

    /**
     * @brief Base64解码
     * @param encoded 编码字符串
     * @return 解码后的字符串
     */
    std::string base64_decode(const std::string& encoded);
};

} // namespace plugins
} // namespace falcon