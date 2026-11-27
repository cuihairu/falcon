/**
 * @file dht_node.cpp
 * @brief BitTorrent DHT (Distributed Hash Table) 节点实现
 * @author Falcon Team
 * @date 2026-05-07
 */

// 取消 Windows 可能定义的 ERROR 宏，避免与日志宏冲突
#ifdef ERROR
#undef ERROR
#endif

#include "dht_node.hpp"
#include "bencode.hpp"
#include <falcon/detail/injection.hpp>
#include <falcon/logger.hpp>

#include <random>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <array>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace {

// 跨平台前导零计数
inline int clzByte(uint8_t byte) {
    if (byte == 0) return 8;
#ifdef _WIN32
    unsigned long index = 0;
    _BitScanReverse(&index, byte);
    return 7 - static_cast<int>(index);
#else
    return __builtin_clz(byte) - 24;
#endif
}

} // anonymous namespace

namespace falcon {
namespace protocols {

namespace {

// 每轮并发查询的节点数（Kademlia alpha）
constexpr size_t kLookupAlpha = 3;
// 查找维护的最接近候选数（Kademlia k）
constexpr size_t kLookupK = 8;
// 候选集上限（防御响应中的超量 nodes 条目把候选表撑爆）
constexpr size_t kMaxCandidates = 64;

// 解析 compact node info（BEP-005：26 字节 = 20 id + 2 port + 4 ip，大端序）
std::vector<DhtNode> parseCompactNodes(const std::string& nodes) {
    std::vector<DhtNode> result;
    size_t nodeCount = nodes.size() / 26;
    result.reserve(nodeCount);

    for (size_t i = 0; i < nodeCount; ++i) {
        size_t offset = i * 26;

        DhtNode node;
        std::memcpy(node.id.data(), nodes.data() + offset, 20);

        // IP 和端口（大端序）
        uint32_t ipBigEndian;
        std::memcpy(&ipBigEndian, nodes.data() + offset + 20, 4);
        node.ip = std::to_string(ipBigEndian & 0xFF) + "." +
                  std::to_string((ipBigEndian >> 8) & 0xFF) + "." +
                  std::to_string((ipBigEndian >> 16) & 0xFF) + "." +
                  std::to_string((ipBigEndian >> 24) & 0xFF);

        uint16_t portBigEndian;
        std::memcpy(&portBigEndian, nodes.data() + offset + 24, 2);
        node.port = ntohs(portBigEndian);

        node.lastSeen = std::chrono::steady_clock::now();
        result.push_back(std::move(node));
    }
    return result;
}

// 解析 compact peer info（BEP-005：6 字节 = 4 ip + 2 port）
std::vector<std::pair<std::string, uint16_t>> parseCompactPeers(const std::string& values) {
    std::vector<std::pair<std::string, uint16_t>> peers;
    size_t peerCount = values.size() / 6;
    peers.reserve(peerCount);

    for (size_t i = 0; i < peerCount; ++i) {
        size_t offset = i * 6;

        // IP 地址
        std::string ip = std::to_string(static_cast<uint8_t>(values[offset])) + "." +
                         std::to_string(static_cast<uint8_t>(values[offset + 1])) + "." +
                         std::to_string(static_cast<uint8_t>(values[offset + 2])) + "." +
                         std::to_string(static_cast<uint8_t>(values[offset + 3]));

        // 端口（大端序）
        uint16_t port = (static_cast<uint8_t>(values[offset + 4]) << 8) |
                        static_cast<uint8_t>(values[offset + 5]);

        peers.emplace_back(std::move(ip), port);
    }
    return peers;
}

} // anonymous namespace

namespace DhtUtils {

DhtNodeId generateRandomNodeId() {
    DhtNodeId id;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 255);

    for (auto& byte : id) {
        byte = static_cast<uint8_t>(dis(gen));
    }
    return id;
}

DhtNodeId nodeIdFromString(const std::string& str) {
    DhtNodeId id{};
    // 40 位合法 hex → 20 字节解码（info_hash / 消息 id 字段的文本编码形式），
    // 与 nodeIdToString 构成往返
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (str.size() == id.size() * 2) {
        bool valid = true;
        for (size_t i = 0; i < id.size(); ++i) {
            int hi = hexVal(str[i * 2]);
            int lo = hexVal(str[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                valid = false;
                break;
            }
            id[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        if (valid) {
            return id;
        }
    }
    // 非 hex 形态回退：按原始字节截断/补零
    // （(std::min)：加括号防止 Windows.h 的 min 宏展开）
    size_t len = (std::min)(str.size(), id.size());
    std::memcpy(id.data(), str.data(), len);
    return id;
}

std::string nodeIdToString(const DhtNodeId& id) {
    std::ostringstream oss;
    for (auto byte : id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

DhtNodeId xorDistance(const DhtNodeId& a, const DhtNodeId& b) {
    DhtNodeId result;
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = a[i] ^ b[i];
    }
    return result;
}

bool distanceLessThan(const DhtNodeId& a, const DhtNodeId& b, const DhtNodeId& target) {
    auto distA = xorDistance(a, target);
    auto distB = xorDistance(b, target);
    return distA < distB;
}

} // namespace DhtUtils

//==============================================================================
// DhtNode
//==============================================================================


DhtNodeId DhtNode::distanceTo(const DhtNodeId& other) const {
    return DhtUtils::xorDistance(id, other);
}

bool DhtNode::closerThan(const DhtNode& other, const DhtNodeId& target) const {
    return DhtUtils::distanceLessThan(id, other.id, target);
}

//==============================================================================
// DhtBucket
//==============================================================================

bool DhtBucket::addNode(const DhtNode& node) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查是否已存在
    for (auto& n : nodes_) {
        if (n.id == node.id) {
            n.lastSeen = std::chrono::steady_clock::now();
            return true;
        }
    }

    // 如果桶未满，直接添加
    if (nodes_.size() < K) {
        nodes_.push_back(node);
        return true;
    }

    // 查找最少活跃的节点
    auto oldestIt = std::min_element(nodes_.begin(), nodes_.end(),
        [](const DhtNode& a, const DhtNode& b) {
            return a.lastSeen < b.lastSeen;
        });

    // 如果旧节点长时间未活跃，替换它
    auto now = std::chrono::steady_clock::now();
    if (now - oldestIt->lastSeen > std::chrono::minutes(15)) {
        *oldestIt = node;
        return true;
    }

    // 否则添加到替换缓存
    if (replacementCache_.size() < K) {
        replacementCache_.push_back(node);
    }
    return false;
}

void DhtBucket::removeNode(const DhtNodeId& nodeId) {
    std::lock_guard<std::mutex> lock(mutex_);
    nodes_.erase(
        std::remove_if(nodes_.begin(), nodes_.end(),
            [&nodeId](const DhtNode& n) { return n.id == nodeId; }),
        nodes_.end()
    );
}

std::vector<DhtNode> DhtBucket::getNodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_;
}

size_t DhtBucket::getActiveNodeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(std::count_if(nodes_.begin(), nodes_.end(),
        [](const DhtNode& n) { return n.active; }));
}

std::vector<DhtNode> DhtBucket::findClosestNodes(const DhtNodeId& target, size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DhtNode> result = nodes_;

    // 按距离排序
    std::sort(result.begin(), result.end(),
        [&target](const DhtNode& a, const DhtNode& b) {
            return a.closerThan(b, target);
        });

    // 返回前 count 个
    if (result.size() > count) {
        result.resize(count);
    }
    return result;
}

//==============================================================================
// DhtRoutingTable
//==============================================================================

void DhtRoutingTable::addNode(const DhtNode& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t bucketIdx = getBucketIndex(node.id);
    buckets_[bucketIdx].addNode(node);
}

std::vector<DhtNode> DhtRoutingTable::findClosestNodes(const DhtNodeId& target, size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 从所有桶中收集节点
    std::vector<DhtNode> allNodes;
    for (const auto& bucketPair : buckets_) {
        const auto& bucket = bucketPair.second;
        auto nodes = bucket.getNodes();
        allNodes.insert(allNodes.end(), nodes.begin(), nodes.end());
    }

    // 按距离排序
    std::sort(allNodes.begin(), allNodes.end(),
        [&target](const DhtNode& a, const DhtNode& b) {
            return a.closerThan(b, target);
        });

    // 返回前 count 个
    if (allNodes.size() > count) {
        allNodes.resize(count);
    }
    return allNodes;
}

std::vector<DhtNode> DhtRoutingTable::getAllNodes() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<DhtNode> allNodes;
    for (const auto& bucketPair : buckets_) {
        const auto& bucket = bucketPair.second;
        auto nodes = bucket.getNodes();
        allNodes.insert(allNodes.end(), nodes.begin(), nodes.end());
    }
    return allNodes;
}

size_t DhtRoutingTable::getTotalNodeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t total = 0;
    for (const auto& bucketPair : buckets_) {
        const auto& bucket = bucketPair.second;
        total += bucket.getActiveNodeCount();
    }
    return total;
}

size_t DhtRoutingTable::getBucketIndex(const DhtNodeId& nodeId) const {
    // 计算前导零的数量来确定桶索引
    size_t leadingZeros = 0;
    for (size_t i = 0; i < nodeId.size(); ++i) {
        uint8_t byte = nodeId[i];
        if (byte == 0) {
            leadingZeros += 8;
        } else {
            leadingZeros += static_cast<size_t>(clzByte(byte));
            break;
        }
    }
    return (std::min)(leadingZeros, size_t{159}); // 160 个桶 (0-159)
}

//==============================================================================
// DhtMessage
//==============================================================================

std::string DhtMessage::encode() const {
    using namespace falcon::protocols;

    BencodeValue dict;
    dict["t"].setString(transactionId);

    // 添加节点 ID
    dict["id"].setString(DhtUtils::nodeIdToString(nodeId));

    switch (type) {
        case DhtMessageType::Query: {
            dict["y"].setString("q");
            dict["q"].setString(queryTypeToString(queryType));

            BencodeValue args;
            for (const auto& pair : arguments) {
                args[pair.first].setString(pair.second);
            }
            dict["a"] = args;
            break;
        }

        case DhtMessageType::Response: {
            dict["y"].setString("r");

            BencodeValue resp;
            for (const auto& pair : response) {
                resp[pair.first].setString(pair.second);
            }
            dict["r"] = resp;
            break;
        }

        case DhtMessageType::Error: {
            dict["y"].setString("e");

            // 错误格式: [error_code, error_msg]
            BencodeValue errorList;
            errorList.setList({
                BencodeValue(static_cast<int64_t>(0)),
                BencodeValue(response.empty() ? "Unknown error" : response.begin()->second)
            });
            dict["e"] = errorList;
            break;
        }
    }

    return dict.encode();
}

DhtMessage DhtMessage::decode(const std::string& data) {
    using namespace falcon::protocols;

    DhtMessage msg;
    try {
        BencodeValue dict = BencodeValue::decode(data);

        if (!dict.isDict()) {
            return msg; // 无效消息
        }

        // 获取事务 ID
        if (dict.hasKey("t") && dict["t"].isString()) {
            msg.transactionId = dict["t"].asString();
        }

        // 获取节点 ID
        if (dict.hasKey("id") && dict["id"].isString()) {
            msg.nodeId = DhtUtils::nodeIdFromString(dict["id"].asString());
        }

        // 确定消息类型
        if (dict.hasKey("y") && dict["y"].isString()) {
            std::string y = dict["y"].asString();

            if (y == "q") {
                msg.type = DhtMessageType::Query;
                if (dict.hasKey("q") && dict["q"].isString()) {
                    msg.queryType = stringToQueryType(dict["q"].asString());
                }
                if (dict.hasKey("a") && dict["a"].isDict()) {
                    for (const auto& pair : dict["a"].asDict()) {
                        if (pair.second.isString()) {
                            msg.arguments[pair.first] = pair.second.asString();
                        }
                    }
                }

            } else if (y == "r") {
                msg.type = DhtMessageType::Response;
                if (dict.hasKey("r") && dict["r"].isDict()) {
                    for (const auto& pair : dict["r"].asDict()) {
                        if (pair.second.isString()) {
                            msg.response[pair.first] = pair.second.asString();
                        }
                    }
                }

            } else if (y == "e") {
                msg.type = DhtMessageType::Error;
                if (dict.hasKey("e") && dict["e"].isList() && dict["e"].size() >= 2) {
                    if (dict["e"][1].isString()) {
                        msg.response["error"] = dict["e"][1].asString();
                    }
                }
            }
        }

    } catch (const BencodeException&) {
        // 解析失败，返回空消息
    }

    return msg;
}

//==============================================================================
// DhtClient
//==============================================================================

DhtClient::DhtClient(uint16_t port) : port_(port) {
    nodeId_ = DhtUtils::generateRandomNodeId();

    // 添加默认引导节点
    addBootstrapNode("router.bittorrent.com", 6881);
    addBootstrapNode("dht.transmissionbt.com", 6881);
    addBootstrapNode("router.utorrent.com", 6881);
}

DhtClient::~DhtClient() {
    stop();
}

void DhtClient::start() {
    if (running_.load()) {
        return;
    }

    // 创建 UDP socket
    socket_ = ::falcon::detail::inject_failure(
                  ::falcon::detail::InjectPoint::DhtSocketCreate)
                  ? -1
                  : socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        falcon::detail::log_errorf("Failed to create DHT socket: {}", strerror(errno));
        return;
    }

    // 设置非阻塞模式
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket_, FIONBIO, &mode);
#else
    int flags = fcntl(socket_, F_GETFL, 0);
    fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
#endif

    // 绑定端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        falcon::detail::log_errorf("Failed to bind DHT socket to port {}", port_);
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
        socket_ = -1;
        return;
    }

    running_.store(true);

    // 启动接收线程
    receiveThread_ = std::thread(&DhtClient::receiveLoop, this);

    // 启动维护线程
    maintenanceThread_ = std::thread([this]() {
        while (running_.load()) {
            // 分片休眠（100ms 粒度，总计约 5 分钟），保证 stop() 能及时 join
            for (int i = 0; i < 3000 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_.load()) {
                break;
            }

            // 定期刷新路由表（无回调的后台查找）
            auto targetId = DhtUtils::generateRandomNodeId();
            startLookup(targetId, false, "", nullptr, nullptr);
        }
    });

    falcon::detail::log_infof("DHT client started on port {}", port_);
}

void DhtClient::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    if (maintenanceThread_.joinable()) {
        maintenanceThread_.join();
    }

    if (socket_ >= 0) {
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
        socket_ = -1;
    }

    falcon::detail::log_infof("DHT client stopped");
}

void DhtClient::addBootstrapNode(const std::string& ip, uint16_t port) {
    DhtNode node;
    node.ip = ip;
    node.port = port;
    node.id = DhtUtils::generateRandomNodeId();  // 未知 ID，使用随机 ID
    node.lastSeen = std::chrono::steady_clock::now();

    bootstrapNodes_.push_back(node);
}

void DhtClient::findPeers(const std::string& infoHash, FoundPeersCallback callback) {
    // 异步 Kademlia 查找：响应在 receiveLoop 线程驱动迭代，
    // 收敛/超时后回调一次性上报全部发现的 peers（可能为空）
    startLookup(DhtUtils::nodeIdFromString(infoHash), true, infoHash,
                std::move(callback), nullptr);
}

void DhtClient::findNode(const DhtNodeId& targetId, NodeFoundCallback callback) {
    startLookup(targetId, false, "", nullptr, std::move(callback));
}


} // namespace protocols
} // namespace falcon
