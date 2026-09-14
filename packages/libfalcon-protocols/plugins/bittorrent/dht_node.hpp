/**
 * @file dht_node.hpp
 * @brief BitTorrent DHT (Distributed Hash Table) 节点实现
 * @author Falcon Team
 * @date 2026-05-07
 */

#pragma once

#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

namespace falcon {
namespace protocols {

/**
 * @brief DHT 节点 ID 类型 (20 字节)
 */
using DhtNodeId = std::array<uint8_t, 20>;

/**
 * @brief DHT 节点信息
 */
struct DhtNode {
    std::string ip;              // IP 地址
    uint16_t port = 0;           // 端口
    DhtNodeId id;                // 节点 ID
    std::chrono::steady_clock::time_point lastSeen;  // 最后活跃时间
    bool active = true;          // 是否活跃（路由表已加锁保护，无需原子操作）

    /**
     * @brief 计算与另一个节点 ID 的距离 (XOR 距离)
     */
    DhtNodeId distanceTo(const DhtNodeId& other) const;

    /**
     * @brief 比较节点距离（用于排序）
     */
    bool closerThan(const DhtNode& other, const DhtNodeId& target) const;
};

/**
 * @brief DHT 路由表桶 (K-bucket)
 */
class DhtBucket {
public:
    static constexpr size_t K = 8;  // 每个桶最多 8 个节点

    /**
     * @brief 添加节点到桶
     */
    bool addNode(const DhtNode& node);

    /**
     * @brief 移除节点
     */
    void removeNode(const DhtNodeId& nodeId);

    /**
     * @brief 获取所有节点
     */
    std::vector<DhtNode> getNodes() const;

    /**
     * @brief 获取活跃节点数量
     */
    size_t getActiveNodeCount() const;

    /**
     * @brief 查找最接近目标的节点
     */
    std::vector<DhtNode> findClosestNodes(const DhtNodeId& target, size_t count = K) const;

private:
    mutable std::mutex mutex_;
    std::vector<DhtNode> nodes_;
    std::vector<DhtNode> replacementCache_;  // 替换缓存
};

/**
 * @brief DHT 路由表
 */
class DhtRoutingTable {
public:
    /**
     * @brief 添加节点
     */
    void addNode(const DhtNode& node);

    /**
     * @brief 查找最接近目标的节点
     */
    std::vector<DhtNode> findClosestNodes(const DhtNodeId& target, size_t count = 8) const;

    /**
     * @brief 获取所有节点
     */
    std::vector<DhtNode> getAllNodes() const;

    /**
     * @brief 获取节点总数
     */
    size_t getTotalNodeCount() const;

private:
    // 根据 ID 前缀确定桶索引
    size_t getBucketIndex(const DhtNodeId& nodeId) const;

    mutable std::mutex mutex_;
    std::map<size_t, DhtBucket> buckets_;
};

/**
 * @brief DHT 消息类型
 */
enum class DhtMessageType : uint8_t {
    Query = 'q',           // 查询
    Response = 'r',        // 响应
    Error = 'e'            // 错误
};

/**
 * @brief DHT 查询类型
 */
enum class DhtQueryType : uint8_t {
    Ping = 0,
    FindNode = 1,
    GetPeers = 2,
    AnnouncePeer = 3,
    SampleInfohashes = 4
};

/**
 * @brief 查询类型到字符串的映射
 */
inline const char* queryTypeToString(DhtQueryType type) {
    switch (type) {
        case DhtQueryType::Ping: return "ping";
        case DhtQueryType::FindNode: return "find_node";
        case DhtQueryType::GetPeers: return "get_peers";
        case DhtQueryType::AnnouncePeer: return "announce_peer";
        case DhtQueryType::SampleInfohashes: return "sample_infohashes";
        default: return "unknown";
    }
}

/**
 * @brief 字符串到查询类型的映射
 */
inline DhtQueryType stringToQueryType(const std::string& str) {
    if (str == "ping") return DhtQueryType::Ping;
    if (str == "find_node") return DhtQueryType::FindNode;
    if (str == "get_peers") return DhtQueryType::GetPeers;
    if (str == "announce_peer") return DhtQueryType::AnnouncePeer;
    if (str == "sample_infohashes") return DhtQueryType::SampleInfohashes;
    return DhtQueryType::Ping;
}

/**
 * @brief DHT 消息
 */
struct DhtMessage {
    DhtMessageType type = DhtMessageType::Query;
    std::string transactionId;    // 事务 ID
    DhtNodeId nodeId;             // 发送者节点 ID

    // Query 字段
    DhtQueryType queryType;
    std::map<std::string, std::string> arguments;

    // Response 字段
    std::map<std::string, std::string> response;

    /**
     * @brief 编码为 B 编码字符串
     */
    std::string encode() const;

    /**
     * @brief 从 B 编码字符串解码
     */
    static DhtMessage decode(const std::string& data);
};

/**
 * @brief DHT 客户端
 */
class DhtClient {
public:
    /**
     * @brief 回调函数类型
     */
    using FoundPeersCallback = std::function<void(const std::string& infoHash,
                                                   const std::vector<std::pair<std::string, uint16_t>>& peers)>;
    using NodeFoundCallback = std::function<void(const DhtNode& node)>;

    /**
     * @brief 构造函数
     */
    explicit DhtClient(uint16_t port = 6881);

    /**
     * @brief 析构函数
     */
    ~DhtClient();

    /**
     * @brief 启动 DHT 客户端
     */
    void start();

    /**
     * @brief 停止 DHT 客户端
     */
    void stop();

    /**
     * @brief 客户端是否处于运行状态（socket 已绑定、收发线程存活；
     * start() 遇端口占用等失败时为 false——失败只记日志不抛异常）
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief 添加引导节点
     */
    void addBootstrapNode(const std::string& ip, uint16_t port);

    /**
     * @brief 清空引导节点列表（构造时预置的公网引导节点一并清除，
     * 供纯私有网络/测试场景使用；须在 start() 之前调用）
     */
    void clear_bootstrap_nodes() { bootstrapNodes_.clear(); }

    /**
     * @brief 查找 peers (通过 info_hash)
     */
    void findPeers(const std::string& infoHash, FoundPeersCallback callback);

    /**
     * @brief 查找节点 (通过 node_id)
     */
    void findNode(const DhtNodeId& targetId, NodeFoundCallback callback);

    /**
     * @brief 设置单次查找的超时时间（默认 30 秒）
     *
     * 超时的查找会提前终结：已收集到的 peers/节点仍上报给回调。
     * 须在 start() 之前调用（无内部同步）。
     */
    void set_lookup_timeout(std::chrono::seconds timeout) {
        lookupTimeoutSeconds_.store(timeout.count());
    }

    /**
     * @brief 获取本地节点 ID
     */
    const DhtNodeId& getNodeId() const { return nodeId_; }

    /**
     * @brief 获取路由表
     */
    const DhtRoutingTable& getRoutingTable() const { return routingTable_; }

private:
    /**
     * @brief 单次 Kademlia 迭代查找的上下文
     *
     * 生命周期：findPeers/findNode 创建 → continueLookup 驱动迭代 →
     * 候选耗尽（收敛）或超时后 finalizeLookup 终结并触发回调。
     */
    struct LookupContext {
        DhtNodeId target;
        std::string infoHash;                // get_peers 查找时非空
        bool isFindPeers = false;
        FoundPeersCallback peersCallback;
        NodeFoundCallback nodeCallback;
        std::vector<DhtNode> candidates;     // 候选节点（含已查询）
        // 已查询/待响应以端点（ip:port）为键而非节点 ID——引导节点的 ID
        // 未知（随机占位），响应中带回的真实 ID 不得导致同一节点重复查询
        std::set<std::string> queriedEndpoints;
        std::set<std::string> outstandingEndpoints;
        std::vector<std::pair<std::string, uint16_t>> foundPeers;  // 已发现的 peers
        std::vector<DhtNode> respondedNodes; // 已响应的节点
        std::chrono::steady_clock::time_point startTime;

        static std::string endpointKey(const std::string& ip, uint16_t port) {
            return ip + ":" + std::to_string(port);
        }
        static std::string endpointKey(const DhtNode& node) {
            return endpointKey(node.ip, node.port);
        }
    };

    /**
     * @brief 创建查找上下文并启动首轮查询
     */
    void startLookup(const DhtNodeId& target, bool isFindPeers, const std::string& infoHash,
                     FoundPeersCallback peersCallback, NodeFoundCallback nodeCallback);

    /**
     * @brief 推进一轮迭代：向最近的未查询候选（至多 alpha 个）发查询；
     * 无候选且无待响应时终结查找
     */
    void continueLookup(uint64_t lookupId);

    /**
     * @brief 处理查找请求的响应：吸收响应中的节点/peers，继续迭代
     */
    void handleLookupResponse(uint64_t lookupId, const DhtNode& queriedNode,
                              const DhtMessage& message);

    /**
     * @brief 将发送失败的节点从待响应集合移除（查找因此可能立即收敛，
     * 而不必等到超时）
     */
    void noteUnreachable(uint64_t lookupId, const std::string& endpoint);

    /**
     * @brief 终结查找：清理事务映射并触发回调（收敛或超时共用）
     */
    void finalizeLookup(uint64_t lookupId);

    /**
     * @brief 将超过超时的查找终结（receiveLoop 周期调用）
     */
    void expireStaleLookups();

    /**
     * @brief UDP 接收循环
     */
    void receiveLoop();

    /**
     * @brief 处理接收到的消息
     */
    void handleMessage(const DhtMessage& message, const std::string& senderIp, uint16_t senderPort);

    /**
     * @brief 发送消息（返回 false 表示发送失败：socket 无效、
     * 目标非数字 IP 或系统调用失败）
     */
    bool sendMessage(const DhtMessage& message, const std::string& ip, uint16_t port);

    /**
     * @brief 生成事务 ID
     */
    std::string generateTransactionId();

    uint16_t port_;
    DhtNodeId nodeId_;
    DhtRoutingTable routingTable_;
    std::vector<DhtNode> bootstrapNodes_;

    int socket_ = -1;
    std::atomic<bool> running_{false};
    std::thread receiveThread_;
    std::thread maintenanceThread_;

    mutable std::mutex mutex_;
    // 事务 ID → 响应回调（查找请求注册；一请求一响应，消费即删）
    std::map<std::string, std::function<void(const DhtMessage&)>> pendingRequests_;
    // 事务 ID → (查找 ID, 被查询节点)，用于终结时清理未响应的注册
    std::map<std::string, std::pair<uint64_t, DhtNodeId>> lookupTransactions_;
    std::map<uint64_t, LookupContext> lookups_;
    uint64_t nextLookupId_ = 0;
    std::atomic<int64_t> lookupTimeoutSeconds_{30};

    static constexpr size_t TRANSACTION_ID_SIZE = 4;
    std::atomic<uint32_t> transactionCounter_{0};
};

/**
 * @brief DHT 工具函数
 */
namespace DhtUtils {
    /**
     * @brief 生成随机节点 ID
     */
    DhtNodeId generateRandomNodeId();

    /**
     * @brief 从字符串创建节点 ID
     */
    DhtNodeId nodeIdFromString(const std::string& str);

    /**
     * @brief 节点 ID 转字符串
     */
    std::string nodeIdToString(const DhtNodeId& id);

    /**
     * @brief 计算 XOR 距离
     */
    DhtNodeId xorDistance(const DhtNodeId& a, const DhtNodeId& b);

    /**
     * @brief 比较两个距离（返回 true 表示 a < b）
     */
    bool distanceLessThan(const DhtNodeId& a, const DhtNodeId& b, const DhtNodeId& target);
}

} // namespace protocols
} // namespace falcon
