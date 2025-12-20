/**
 * @file bittorrent_plugin.hpp
 * @brief BitTorrent/Magnet 协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include "../base_protocol_plugin.hpp"
#include <string>
#include <vector>
#include <memory>
#include <map>

// 如果使用 libtorrent
#ifdef FALCON_USE_LIBTORRENT
#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/magnet_uri.hpp>
#endif

namespace falcon {
namespace plugins {

/**
 * @class BitTorrentPlugin
 * @brief BitTorrent/Magnet 协议处理器
 *
 * 支持 .torrent 文件和 magnet:// 链接下载
 * 基于自定义实现或 libtorrent 库
 */
class BitTorrentPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    BitTorrentPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~BitTorrentPlugin();

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "bittorrent"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief BitTorrent 下载任务
     */
    class BitTorrentDownloadTask : public IDownloadTask {
    public:
        BitTorrentDownloadTask(const std::string& url, const DownloadOptions& options);
        virtual ~BitTorrentDownloadTask();

        // IDownloadTask 接口实现
        void start() override;
        void pause() override;
        void resume() override;
        void cancel() override;
        TaskStatus getStatus() const override;
        float getProgress() const override;
        uint64_t getTotalBytes() const override;
        uint64_t getDownloadedBytes() const override;
        uint64_t getSpeed() const override;
        std::string getErrorMessage() const override;

    private:
#ifdef FALCON_USE_LIBTORRENT
        libtorrent::session session_;
        libtorrent::torrent_handle handle_;
        libtorrent::add_torrent_params params_;
#endif

        std::string url_;
        DownloadOptions options_;
        TaskStatus status_;
        std::string errorMessage_;

        // 统计信息
        uint64_t totalSize_;
        uint64_t downloadedBytes_;
        uint64_t uploadBytes_;
        uint64_t downloadSpeed_;
        uint64_t uploadSpeed_;

        // 文件信息
        struct FileInfo {
            std::string name;
            uint64_t size;
            std::string path;
        };
        std::vector<FileInfo> files_;

        // 线程安全
        mutable std::mutex mutex_;

        /**
         * @brief 解析 torrent 文件
         */
        bool parseTorrentFile(const std::string& filePath);

        /**
         * @brief 解析 magnet 链接
         */
        bool parseMagnetUri(const std::string& magnetUri);

        /**
         * @brief 更新下载统计
         */
        void updateStats();

        /**
         * @brief 选择要下载的文件
         */
        void selectFiles();

        /**
         * @brief 设置下载优先级
         */
        void setFilePriorities();

        /**
         * @brief 处理警报
         */
        void handleAlerts();
    };

    /**
     * @brief 解析 B 编码数据
     */
    struct BValue {
        enum Type { String, Integer, List, Dict };
        Type type;
        std::string strValue;
        int64_t intValue;
        std::vector<BValue> listValue;
        std::map<std::string, BValue> dictValue;
    };

    /**
     * @brief 解析 B 编码
     */
    BValue parseBencode(const std::string& data, size_t& pos);

    /**
     * @brief B 编码到字符串
     */
    std::string bencodeToString(const BValue& value);

    /**
     * @brief 计算 SHA1 哈希
     */
    std::string sha1(const std::string& data);

    /**
     * @base32 解码（用于 magnet 链接中的 info_hash）
     */
    std::string base32Decode(const std::string& input);

    /**
     * @brief 验证 torrent 文件
     */
    bool validateTorrent(const BValue& torrent);

    /**
     * @brief 获取 tracker 列表
     */
    std::vector<std::string> getTrackers(const BValue& torrent);

    /**
     * @brief 创建 DHT 节点 ID
     */
    std::string generateNodeId();
};

} // namespace plugins
} // namespace falcon