/**
 * @file hls_plugin.hpp
 * @brief HLS (HTTP Live Streaming) 和 DASH 流媒体协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include "../base_protocol_plugin.hpp"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace falcon {
namespace plugins {

/**
 * @class HLSPlugin
 * @brief HLS 和 DASH 流媒体协议处理器
 *
 * 支持 HLS (.m3u8) 和 DASH (.mpd) 流媒体下载
 */
class HLSPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    HLSPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~HLSPlugin() = default;

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "hls"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief 媒体段信息
     */
    struct MediaSegment {
        std::string url;           // 段URL
        double duration;           // 时长（秒）
        std::string title;         // 标题（可选）
        uint64_t size;            // 文件大小（可选）
        std::map<std::string, std::string> attributes;  // 其他属性
    };

    /**
     * @brief 播放列表信息
     */
    struct PlaylistInfo {
        bool isLive;              // 是否为直播流
        double targetDuration;     // 目标段时长
        uint32_t version;          // 版本号
        std::vector<MediaSegment> segments;  // 媒体段列表
        std::map<std::string, std::string> variants;  // 变体流
    };

    /**
     * @brief DASH表现信息
     */
    struct DASHRepresentation {
        std::string id;            // 表现ID
        std::string mimeType;      // MIME类型
        std::string codecs;        // 编码格式
        uint32_t width;            // 宽度（视频）
        uint32_t height;           // 高度（视频）
        uint32_t bandwidth;        // 带宽
        std::vector<MediaSegment> segments;  // 媒体段列表
    };

    /**
     * @brief DASH适配信息
     */
    struct DASHAdaptation {
        std::string id;            // 适配ID
        std::string mimeType;      // MIME类型
        std::vector<DASHRepresentation> representations;  // 表现列表
    };

    /**
     * @brief 解析HLS M3U8播放列表
     * @param m3u8Content M3U8内容
     * @param baseUrl 基础URL
     * @return 播放列表信息
     */
    PlaylistInfo parseM3U8(const std::string& m3u8Content, const std::string& baseUrl);

    /**
     * @brief 解析DASH MPD清单
     * @param mpdContent MPD内容
     * @param baseUrl 基础URL
     * @return 适配信息列表
     */
    std::vector<DASHAdaptation> parseMPD(const std::string& mpdContent, const std::string& baseUrl);

    /**
     * @brief 解析EXT-X-STREAM-INF标签
     * @param attributes 属性字符串
     * @return 属性映射
     */
    std::map<std::string, std::string> parseStreamInf(const std::string& attributes);

    /**
     * @brief 解析EXTINF标签
     * @param extinf EXTINF标签内容
     * @return 时长和标题
     */
    std::pair<double, std::string> parseExtInf(const std::string& extinf);

    /**
     * @brief 解析加密信息
     * @param line EXT-X-KEY行
     * @return 加密信息
     */
    struct EncryptionInfo {
        std::string method;        // 加密方法
        std::string uri;           // 密钥URI
        std::string iv;            // 初始化向量
        std::string keyFormat;     // 密钥格式
    };
    EncryptionInfo parseEncryption(const std::string& line);

    /**
     * @brief 下载并解析主播放列表
     * @param masterUrl 主播放列表URL
     * @return 播放列表URL列表
     */
    std::vector<std::string> downloadMasterPlaylist(const std::string& masterUrl);

    /**
     * @brief 选择最佳质量流
     * @param streams 流列表
     * @param options 下载选项
     * @return 选中的流URL
     */
    std::string selectBestQuality(const std::vector<std::string>& streams,
                                 const DownloadOptions& options);

    /**
     * @brief 创建批量下载任务
     * @param segments 媒体段列表
     * @param outputDir 输出目录
     * @param options 下载选项
     * @return 批量任务
     */
    std::unique_ptr<IDownloadTask> createBatchTask(const std::vector<MediaSegment>& segments,
                                                  const std::string& outputDir,
                                                  const DownloadOptions& options);

    /**
     * @brief 合并媒体段
     * @param segmentFiles 段文件列表
     * @param outputFile 输出文件
     * @return 是否成功
     */
    bool mergeSegments(const std::vector<std::string>& segmentFiles,
                      const std::string& outputFile);

    /**
     * @brief 解析相对URL
     * @param url 相对URL
     * @param baseUrl 基础URL
     * @return 绝对URL
     */
    std::string resolveUrl(const std::string& url, const std::string& baseUrl);

    /**
     * @brief 检查是否为HLS流
     * @param url URL
     * @return 是否为HLS
     */
    bool isHLSStream(const std::string& url);

    /**
     * @brief 检查是否为DASH流
     * @param url URL
     * @return 是否为DASH
     */
    bool isDASHStream(const std::string& url);

    /**
     * @brief 获取流类型
     * @param url URL
     * @return 流类型（"hls", "dash", "unknown"）
     */
    std::string getStreamType(const std::string& url);
};

} // namespace plugins
} // namespace falcon