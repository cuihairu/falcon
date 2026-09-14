/**
 * @file bittorrent_plugin.hpp
 * @brief BitTorrent/Magnet 协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/protocol_handler.hpp>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

// DHT 和 PEX 支持
#include "dht_node.hpp"
#include "pex_protocol.hpp"

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
namespace protocols {

/**
 * @class BitTorrentHandler
 * @brief BitTorrent/Magnet 协议处理器
 *
 * 支持 .torrent 文件和 magnet:// 链接下载
 * 基于自定义实现或 libtorrent 库
 */
class BitTorrentHandler : public IProtocolHandler {
public:
    /**
     * @brief 构造函数
     */
    BitTorrentHandler();

    /**
     * @brief 析构函数
     */
    ~BitTorrentHandler() override;

    // Non-copyable
    BitTorrentHandler(const BitTorrentHandler&) = delete;
    BitTorrentHandler& operator=(const BitTorrentHandler&) = delete;

    // IProtocolHandler 接口实现
    [[nodiscard]] std::string protocol_name() const override { return "bittorrent"; }

    [[nodiscard]] std::vector<std::string> supported_schemes() const override;

    [[nodiscard]] bool can_handle(const std::string& url) const override;

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                         const DownloadOptions& options) override;

    void download(DownloadTask::Ptr task, IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;

    void resume(DownloadTask::Ptr task, IEventListener* listener) override;

    void cancel(DownloadTask::Ptr task) override;

    [[nodiscard]] bool supports_resume() const override { return true; }

    [[nodiscard]] int priority() const override { return 50; }

    /**
     * @brief 启动 DHT 客户端
     */
    void startDht(uint16_t port = 6881);

    /**
     * @brief 停止 DHT 客户端
     */
    void stopDht();

    /**
     * @brief 检查 DHT 是否已启动（仅当客户端真正绑定并运行时为 true）
     */
    bool isDhtRunning() const { return dhtClient_ != nullptr; }

    /**
     * @brief 清空 DHT 引导节点（预置的公网引导节点一并清除，
     * 供纯私有网络/测试场景使用；须在 DHT 启动后调用）
     */
    void clearDhtBootstrapNodes();

    /**
     * @brief 从 URL 提取 xt=urn:btih: 携带的 info-hash 文本（原样大小写，
     * 在 '&' 或 '#' 处截断）；无该参数返回空串
     */
    static std::string extract_info_hash(const std::string& url);

    /**
     * @brief info-hash 文本归一化为 40 位小写十六进制：hex 输入校验后
     * 小写化；32 位 Base32（RFC 4648，大小写不敏感）解码为原始 20 字节
     * 再转 hex。格式非法或长度不符返回空串
     */
    static std::string info_hash_to_hex(const std::string& hash);

    /**
     * @brief 启用/禁用 PEX
     */
    void setPexEnabled(bool enabled) { pexEnabled_ = enabled; }

    /**
     * @brief 检查 PEX 是否启用
     */
    bool isPexEnabled() const { return pexEnabled_; }

    /**
     * @brief 获取指定 info_hash 的 PEX 处理器
     */
    PexExtensionHandler* getPexHandler(const std::string& infoHash);

    /**
     * @brief 移除 PEX 处理器
     */
    void removePexHandler(const std::string& infoHash);

private:
#ifdef FALCON_USE_LIBTORRENT
    libtorrent::session session_;
    std::map<TaskId, libtorrent::torrent_handle> torrentHandles_;
    std::mutex handlesMutex_;
#else
    // 纯 C++ 实现所需的数据结构
    struct TorrentFileInfo {
        std::string name;
        uint64_t size;
        std::string path;
    };

    struct TorrentInfo {
        std::string name;
        std::string infoHash;      // 20 字节 SHA1 哈希
        uint64_t totalSize = 0;
        uint64_t pieceLength = 0;
        int pieceCount = 0;
        std::vector<std::string> pieces;  // SHA1 哈希列表
        std::vector<TorrentFileInfo> files;
        std::vector<std::string> trackers;
        std::string comment;
        std::string createdBy;
    };

    struct PeerInfo {
        std::string ip;
        uint16_t port;
        std::string peerId;        // 20 字节
        bool isSeed = false;
        uint64_t downloaded = 0;
        uint64_t uploaded = 0;
    };

    struct PieceState {
        std::vector<bool> havePiece;      // 已下载的 piece
        std::vector<bool> requestedPiece; // 已请求的 piece
        std::vector<bool> downloadingPiece; // 正在下载的 piece
        std::vector<std::vector<uint8_t>> pieceData; // piece 数据
    };

    struct TaskContext {
        std::string url;
        DownloadTask::Ptr task;
        IEventListener* listener = nullptr;
        TorrentInfo torrentInfo;
        std::vector<PeerInfo> peers;
        PieceState pieceState;
        std::thread downloadThread;
        std::atomic<bool> running{false};
        std::atomic<bool> paused{false};
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        std::condition_variable cv;

        // 统计信息
        uint64_t totalSize = 0;
        uint64_t downloadedBytes = 0;
        uint64_t uploadBytes = 0;
        uint64_t downloadSpeed = 0;
        uint64_t uploadSpeed = 0;
    };

    std::map<TaskId, std::unique_ptr<TaskContext>> activeTasks_;
    std::mutex tasksMutex_;

    // DHT 客户端
    std::unique_ptr<DhtClient> dhtClient_;
    std::atomic<bool> dhtEnabled_{true};
    uint16_t dhtPort_{6881};

    // PEX 支持
    std::map<std::string, std::unique_ptr<PexExtensionHandler>> pexHandlers_;
    std::mutex pexHandlersMutex_;
    std::atomic<bool> pexEnabled_{true};
#endif

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
     * @base32 解码（大小写不敏感，忽略 '=' 填充；用于 Base32 info-hash）
     */
    static std::string base32Decode(const std::string& input);

    /**
     * @brief 验证 torrent 文件
     */
    bool validateTorrent(const BValue& torrent);

};

/// Factory function to create BitTorrent handler
std::unique_ptr<IProtocolHandler> create_bittorrent_handler();

} // namespace protocols
} // namespace falcon