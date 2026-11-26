/**
 * @file pex_protocol.hpp
 * @brief BitTorrent PEX (Peer Exchange) 协议实现
 * @author Falcon Team
 * @date 2026-05-07
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdint>

namespace falcon {
namespace protocols {

/**
 * @brief Peer 地址信息
 */
struct PexPeer {
    std::string ip;          // IPv4 或 IPv6 地址
    uint16_t port = 0;       // 端口号

    // PEX 标志
    bool isSeed = false;     // 是否为种子
    bool isPreferred = false;// 是否为优先 peer
    bool isUpgraded = false; // 是否支持 uTorrent/BitTorrent 协议升级
};

/**
 * @brief PEX 消息类型 (BEP 0010)
 */
enum class PexMessageType : uint8_t {
    Add = 1,       // 新增 peers
    Add6 = 2,      // 新增 IPv6 peers
    Drop = 3,      // 移除 peers
    Drop6 = 4      // 移除 IPv6 peers
};

/**
 * @brief PEX 消息
 */
struct PexMessage {
    PexMessageType type;
    std::string peerId;      // 发送者的 peer ID (20 字节)
    std::vector<PexPeer> peers;

    /**
     * @brief 编码为二进制格式
     */
    std::vector<uint8_t> encode() const;

    /**
     * @brief 从二进制格式解码
     */
    static PexMessage decode(const std::vector<uint8_t>& data);
};

/**
 * @brief PEX 扩展消息 ID (BEP 0010)
 */
struct PexExtensionIds {
    uint8_t utPex = 0;       // uTorrent PEX
    uint8_t ltPex = 0;       // libtorrent PEX
};

/**
 * @brief PEX 扩展握手消息
 */
struct PexHandshake {
    int32_t supportFlags = 0;  // 支持的标志位
    PexExtensionIds extensionIds;
    std::string metadataSize;  // 元数据大小
    std::string requestQueue;  // 请求队列大小

    /**
     * @brief 编码为 B 编码字典
     */
    std::string encode() const;

    /**
     * @brief 从 B 编码字典解码
     */
    static PexHandshake decode(const std::string& data);
};

/**
 * @brief Peer 连接状态
 */
enum class PeerConnectionState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Handshaking,
    Active
};

/**
 * @brief Peer 信息 (用于 PEX 管理)
 */
struct PexManagedPeer {
    std::string ip;
    uint16_t port;
    std::string peerId;
    PeerConnectionState state = PeerConnectionState::Disconnected;
    std::chrono::steady_clock::time_point lastSeen;
    std::chrono::steady_clock::time_point lastAdded;
    uint8_t failCount = 0;
    bool isSeed = false;
};

/**
 * @brief PEX 管理器
 */
class PexManager {
public:
    /**
     * @brief 回调函数类型
     */
    using PeerDiscoveredCallback = std::function<void(const PexPeer&)>;
    using PeerDroppedCallback = std::function<void(const std::string& ip, uint16_t port)>;

    /**
     * @brief 构造函数
     */
    explicit PexManager(const std::string& infoHash);

    /**
     * @brief 析构函数
     */
    ~PexManager() = default;

    /**
     * @brief 添加已连接的 peer
     */
    void addConnectedPeer(const std::string& ip, uint16_t port, const std::string& peerId);

    /**
     * @brief 移除 peer
     */
    void removePeer(const std::string& ip, uint16_t port);

    /**
     * @brief 处理 PEX 消息
     */
    void handlePexMessage(const PexMessage& message);

    /**
     * @brief 创建 PEX 消息（发送给其他 peer）
     */
    PexMessage createPexMessage(PexMessageType type, size_t maxPeers = 50) const;

    /**
     * @brief 设置回调
     */
    void setPeerDiscoveredCallback(PeerDiscoveredCallback callback);
    void setPeerDroppedCallback(PeerDroppedCallback callback);

    /**
     * @brief 获取管理的 peer 数量
     */
    size_t getManagedPeerCount() const;

    /**
     * @brief 获取候选 peer 数量
     */
    size_t getCandidatePeerCount() const;

    /**
     * @brief 获取候选 peers
     */
    std::vector<PexPeer> getCandidatePeers(size_t maxCount = 50) const;

    /**
     * @brief 更新 peer 状态
     */
    void updatePeerState(const std::string& ip, uint16_t port, PeerConnectionState state, bool isSeed = false);

    /**
     * @brief 获取 info_hash
     */
    const std::string& getInfoHash() const { return infoHash_; }

private:
    /**
     * @brief 压缩 IPv4 地址 (6 字节 -> 4 字节紧凑格式)
     */
    static std::vector<uint8_t> compactIPv4(const std::string& ip, uint16_t port);

    /**
     * @brief 解压缩 IPv4 地址
     */
    static PexPeer uncompactIPv4(const std::vector<uint8_t>& data);

    /**
     * @brief 压缩 IPv6 地址 (18 字节 -> 6 字节紧凑格式)
     */
    static std::vector<uint8_t> compactIPv6(const std::string& ip, uint16_t port);

    /**
     * @brief 解压缩 IPv6 地址
     */
    static PexPeer uncompactIPv6(const std::vector<uint8_t>& data);

    std::string infoHash_;

    // 已连接的 peers (用于 PEX 交换)
    std::map<std::string, PexManagedPeer> connectedPeers_;
    mutable std::mutex connectedPeersMutex_;

    // 候选 peers (从 PEX 获得的但未连接)
    std::set<std::pair<std::string, uint16_t>> candidatePeers_;
    mutable std::mutex candidatePeersMutex_;

    // 最近发送/接收的 peers (用于避免重复)
    std::set<std::pair<std::string, uint16_t>> recentPeers_;
    mutable std::mutex recentPeersMutex_;

    PeerDiscoveredCallback onPeerDiscovered_;
    PeerDroppedCallback onPeerDropped_;

    // 配置
    static constexpr size_t MAX_CONNECTED_PEERS = 100;
    static constexpr size_t MAX_CANDIDATE_PEERS = 300;
    static constexpr size_t MAX_RECENT_PEERS = 1000;
};

/**
 * @brief PEX 扩展协议处理器
 */
class PexExtensionHandler {
public:
    /**
     * @brief 回调函数类型
     */
    using SendExtensionCallback = std::function<void(uint8_t extId, const std::vector<uint8_t>& data)>;

    /**
     * @brief 构造函数
     */
    explicit PexExtensionHandler(const std::string& infoHash);

    /**
     * @brief 析构函数
     */
    ~PexExtensionHandler() = default;

    /**
     * @brief 处理扩展握手消息
     */
    bool handleExtensionHandshake(const std::string& data);

    /**
     * @brief 处理 PEX 消息
     */
    void handlePexMessage(uint8_t extId, const std::vector<uint8_t>& data);

    /**
     * @brief 发送 PEX 消息
     */
    void sendPexMessage(PexMessageType type);

    /**
     * @brief 设置发送回调
     */
    void setSendCallback(SendExtensionCallback callback);

    /**
     * @brief 启用 PEX
     */
    void enable();

    /**
     * @brief 禁用 PEX
     */
    void disable();

    /**
     * @brief 是否启用
     */
    bool isEnabled() const { return enabled_; }

    /**
     * @brief 获取 PEX 管理器
     */
    PexManager& getManager() { return manager_; }

private:
    std::string infoHash_;
    PexManager manager_;
    PexExtensionIds extensionIds_;
    SendExtensionCallback sendCallback_;
    std::atomic<bool> enabled_{false};
};

/**
 * @brief PEX 工具函数
 */
namespace PexUtils {
    /**
     * @brief 解析 IPv6 地址为字符串
     */
    std::string ipv6ToString(const uint8_t* data);

    /**
     * @brief 将字符串转换为 IPv6 二进制格式
     */
    std::vector<uint8_t> stringToIPv6(const std::string& ip);

    /**
     * @brief 验证 peer ID
     */
    bool isValidPeerId(const std::string& peerId);

    /**
     * @brief 解析 compact peer info (IPv4)
     */
    std::vector<PexPeer> parseCompactPeersIPv4(const std::vector<uint8_t>& data);

    /**
     * @brief 解析 compact peer info (IPv6)
     */
    std::vector<PexPeer> parseCompactPeersIPv6(const std::vector<uint8_t>& data);

    /**
     * @brief 编码 compact peer info (IPv4)
     */
    std::vector<uint8_t> encodeCompactPeersIPv4(const std::vector<PexPeer>& peers);

    /**
     * @brief 编码 compact peer info (IPv6)
     */
    std::vector<uint8_t> encodeCompactPeersIPv6(const std::vector<PexPeer>& peers);
}

} // namespace protocols
} // namespace falcon
