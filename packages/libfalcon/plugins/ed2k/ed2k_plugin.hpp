/**
 * @file ed2k_plugin.hpp
 * @brief ED2K (eDonkey2000) 协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include "../base_protocol_plugin.hpp"
#include <string>
#include <vector>
#include <memory>
#include <array>

namespace falcon {
namespace plugins {

/**
 * @class ED2KPlugin
 * @brief ED2K (eDonkey2000) 协议处理器
 *
 * 支持 ed2k:// 格式的链接解析和下载
 * ED2K 是 eDonkey2000 网络使用的协议，也被称为电驴协议
 */
class ED2KPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    ED2KPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~ED2KPlugin() = default;

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "ed2k"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief ED2K文件信息结构
     */
    struct ED2KFileInfo {
        std::string filename;     // 文件名
        uint64_t filesize;        // 文件大小（字节）
        std::array<uint8_t, 16> hash;  // MD4哈希
        std::vector<std::string> sources;  // 源地址列表
        std::string aich;         // AICH哈希（可选）
        uint32_t priority;        // 优先级（可选）
    };

    /**
     * @brief 解析ED2K链接
     * @param ed2kUrl ED2K链接
     * @return 文件信息
     */
    ED2KFileInfo parseED2KUrl(const std::string& ed2kUrl);

    /**
     * @brief 解析文件链接
     * @param params 参数列表
     * @return 文件信息
     */
    ED2KFileInfo parseFileLink(const std::vector<std::string>& params);

    /**
     * @brief 解析服务器链接
     * @param params 参数列表
     * @return 服务器信息
     */
    struct ServerInfo {
        std::string host;
        uint16_t port;
        std::string name;
    };
    ServerInfo parseServerLink(const std::vector<std::string>& params);

    /**
     * @brief URL解码
     * @param encoded 编码字符串
     * @return 解码后的字符串
     */
    std::string urlDecode(const std::string& encoded);

    /**
     * @brief 解析MD4哈希
     * @param hashStr 哈希字符串
     * @return MD4哈希数组
     */
    std::array<uint8_t, 16> parseMD4Hash(const std::string& hashStr);

    /**
     * @brief 解析源地址
     * @param sourcesStr 源地址字符串
     * @return 源地址列表
     */
    std::vector<std::string> parseSources(const std::string& sourcesStr);

    /**
     * @brief 连接到ED2K网络
     * @param servers 服务器列表
     * @return 连接句柄
     */
    void* connectToNetwork(const std::vector<ServerInfo>& servers);

    /**
     * @brief 搜索源
     * @param fileInfo 文件信息
     * @return 源列表
     */
    std::vector<std::string> searchSources(const ED2KFileInfo& fileInfo);

    /**
     * @brief 创建下载任务
     * @param fileInfo 文件信息
     * @param options 下载选项
     * @return 下载任务
     */
    std::unique_ptr<IDownloadTask> createDownloadTask(const ED2KFileInfo& fileInfo,
                                                     const DownloadOptions& options);

    /**
     * @brief URL编码
     * @param str 原始字符串
     * @return 编码后的字符串
     */
    std::string urlEncode(const std::string& str);

    /**
     * @brief 哈希转字符串
     * @param hash MD4哈希数组
     * @return 哈希字符串
     */
    std::string hashToString(const std::array<uint8_t, 16>& hash);
};

} // namespace plugins
} // namespace falcon