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

void DhtClient::receiveLoop() {
    uint8_t buffer[4096];
    sockaddr_in senderAddr{};
    socklen_t senderAddrLen = sizeof(senderAddr);
    auto lastExpireCheck = std::chrono::steady_clock::now();

    while (running_.load()) {
        // 查找超时检查（节流至每秒一次）
        auto now = std::chrono::steady_clock::now();
        if (now - lastExpireCheck >= std::chrono::seconds(1)) {
            lastExpireCheck = now;
            expireStaleLookups();
        }

        // Windows（Winsock）recvfrom 返回 int，POSIX 返回 ssize_t
#ifdef _WIN32
        int bytesRead = recvfrom(socket_, reinterpret_cast<char*>(buffer),
                                 sizeof(buffer), 0,
                                 reinterpret_cast<sockaddr*>(&senderAddr),
                                 &senderAddrLen);
#else
        ssize_t bytesRead = recvfrom(socket_, reinterpret_cast<char*>(buffer),
                                     sizeof(buffer), 0,
                                     reinterpret_cast<sockaddr*>(&senderAddr),
                                     &senderAddrLen);
#endif

        if (bytesRead <= 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::string senderIp(inet_ntoa(senderAddr.sin_addr));
        uint16_t senderPort = ntohs(senderAddr.sin_port);

        try {
            std::string data(buffer, buffer + bytesRead);
            DhtMessage message = DhtMessage::decode(data);
            handleMessage(message, senderIp, senderPort);
        } catch (const std::exception&) {
            // 解析失败，忽略消息
        }
    }
}

void DhtClient::handleMessage(const DhtMessage& message, const std::string& senderIp, uint16_t senderPort) {
    // 更新路由表
    DhtNode node;
    node.ip = senderIp;
    node.port = senderPort;
    node.id = message.nodeId;
    node.lastSeen = std::chrono::steady_clock::now();
    routingTable_.addNode(node);

    // 处理响应
    if (message.type == DhtMessageType::Response) {
        // 锁内取出注册的响应回调并消费（一请求一响应），锁外调用——
        // 回调内部会再获取 mutex_（handleLookupResponse），锁内调用即死锁
        std::function<void(const DhtMessage&)> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pendingRequests_.find(message.transactionId);
            if (it != pendingRequests_.end() && it->second) {
                pending = it->second;
                pendingRequests_.erase(it);
                lookupTransactions_.erase(message.transactionId);
            }
        }
        if (pending) {
            pending(message);
        }

        // 解析节点信息并入路由表（compact node info）
        auto nodesIt = message.response.find("nodes");
        if (nodesIt != message.response.end()) {
            for (const auto& newNode : parseCompactNodes(nodesIt->second)) {
                routingTable_.addNode(newNode);
            }
        }

        // peer 信息（compact peer info）：查找路径的回调上报由
        // handleLookupResponse 处理，此处仅解析、无副作用
        auto valuesIt = message.response.find("values");
        if (valuesIt != message.response.end()) {
            (void)parseCompactPeers(valuesIt->second);
        }
    }
}

bool DhtClient::sendMessage(const DhtMessage& message, const std::string& ip, uint16_t port) {
    if (socket_ < 0) {
        return false;
    }

    std::string data = message.encode();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        // 非数字 IP（如默认引导节点的域名）当前不做 DNS 解析，
        // 按发送失败处理——查找不悬挂，等待真正的可联系节点
        return false;
    }

    return sendto(socket_, data.data(), data.size(), 0,
                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) >= 0;
}

std::string DhtClient::generateTransactionId() {
    uint32_t id = transactionCounter_.fetch_add(1);
    std::string tid;
    tid.resize(TRANSACTION_ID_SIZE);
    std::memcpy(tid.data(), &id, TRANSACTION_ID_SIZE);
    return tid;
}

//==============================================================================
// DhtClient — Kademlia 迭代查找
//==============================================================================

void DhtClient::startLookup(const DhtNodeId& target, bool isFindPeers, const std::string& infoHash,
                            FoundPeersCallback peersCallback, NodeFoundCallback nodeCallback) {
    LookupContext ctx;
    ctx.target = target;
    ctx.isFindPeers = isFindPeers;
    ctx.infoHash = infoHash;
    ctx.peersCallback = std::move(peersCallback);
    ctx.nodeCallback = std::move(nodeCallback);
    ctx.startTime = std::chrono::steady_clock::now();

    // 种子候选：路由表最近 k 个；表空时退回引导节点
    if (routingTable_.getTotalNodeCount() == 0 && !bootstrapNodes_.empty()) {
        ctx.candidates = bootstrapNodes_;
    } else {
        ctx.candidates = routingTable_.findClosestNodes(target, kLookupK);
    }

    uint64_t lookupId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lookupId = nextLookupId_++;
        lookups_.emplace(lookupId, std::move(ctx));
    }

    continueLookup(lookupId);
}

void DhtClient::continueLookup(uint64_t lookupId) {
    struct PendingSend {
        std::string ip;
        uint16_t port;
        std::string endpoint;
        DhtMessage msg;
    };
    std::vector<PendingSend> sends;
    bool finalizeNow = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lookups_.find(lookupId);
        if (it == lookups_.end()) {
            return;
        }
        auto& ctx = it->second;

        // 候选按 XOR 距离排序，取最近的未查询节点
        std::sort(ctx.candidates.begin(), ctx.candidates.end(),
                  [&ctx](const DhtNode& a, const DhtNode& b) {
                      return a.closerThan(b, ctx.target);
                  });

        for (const auto& node : ctx.candidates) {
            if (sends.size() >= kLookupAlpha) {
                break;
            }
            const auto endpoint = ctx.endpointKey(node);
            if (ctx.queriedEndpoints.count(endpoint) > 0) {
                continue;
            }
            if (node.ip.empty() || node.port == 0) {
                // 无效条目直接标记已处理，避免死循环
                ctx.queriedEndpoints.insert(endpoint);
                continue;
            }

            DhtMessage msg;
            msg.type = DhtMessageType::Query;
            msg.nodeId = nodeId_;
            msg.transactionId = generateTransactionId();

            if (ctx.isFindPeers) {
                msg.queryType = DhtQueryType::GetPeers;
                // BEP-005：info_hash 为 20 字节原始值（ctx.infoHash 的 hex
                // 形态仅用于回调上报）
                msg.arguments["info_hash"] =
                    std::string(reinterpret_cast<const char*>(ctx.target.data()),
                                ctx.target.size());
            } else {
                msg.queryType = DhtQueryType::FindNode;
                msg.arguments["target"] = DhtUtils::nodeIdToString(ctx.target);
            }

            ctx.queriedEndpoints.insert(endpoint);
            ctx.outstandingEndpoints.insert(endpoint);
            DhtNode queried = node;

            // 注册响应回调：响应到达时驱动该查找继续迭代
            pendingRequests_[msg.transactionId] =
                [this, lookupId, queried](const DhtMessage& response) {
                    handleLookupResponse(lookupId, queried, response);
                };
            lookupTransactions_[msg.transactionId] = {lookupId, queried.id};

            sends.push_back({node.ip, node.port, endpoint, std::move(msg)});
        }

        // 无未查询候选且全部响应已收齐 → 收敛终结
        finalizeNow = sends.empty() && ctx.outstandingEndpoints.empty();
    }

    // 网络发送在锁外（sendto 系统调用不持锁）；发送失败（域名无法解析、
    // 网络不可达等）的节点按已终结处理，不悬挂到超时
    for (auto& send : sends) {
        if (!sendMessage(send.msg, send.ip, send.port)) {
            noteUnreachable(lookupId, send.endpoint);
        }
    }

    if (finalizeNow) {
        finalizeLookup(lookupId);
    }
}

void DhtClient::noteUnreachable(uint64_t lookupId, const std::string& endpoint) {
    bool finalizeNow = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lookups_.find(lookupId);
        if (it == lookups_.end()) {
            return;  // 查找已终结
        }
        auto& ctx = it->second;
        ctx.outstandingEndpoints.erase(endpoint);

        bool noUnqueried = std::none_of(ctx.candidates.begin(), ctx.candidates.end(),
                                        [&ctx](const DhtNode& n) {
                                            return ctx.queriedEndpoints.count(
                                                       ctx.endpointKey(n)) == 0;
                                        });
        finalizeNow = noUnqueried && ctx.outstandingEndpoints.empty();
    }

    if (finalizeNow) {
        finalizeLookup(lookupId);
    }
}

void DhtClient::handleLookupResponse(uint64_t lookupId, const DhtNode& queriedNode,
                                     const DhtMessage& message) {
    bool finalizeNow = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lookups_.find(lookupId);
        if (it == lookups_.end()) {
            return;  // 查找已被终结（超时/收敛），迟到的响应直接丢弃
        }
        auto& ctx = it->second;

        ctx.outstandingEndpoints.erase(ctx.endpointKey(queriedNode));

        // 记录响应者（节点 ID 以响应声明的为准）
        DhtNode responder = queriedNode;
        if (message.nodeId != DhtNodeId{}) {
            responder.id = message.nodeId;
        }
        responder.lastSeen = std::chrono::steady_clock::now();
        responder.active = true;
        ctx.respondedNodes.push_back(std::move(responder));

        // 吸收响应中的节点作为新候选（BEP-005 compact 格式）
        auto nodesIt = message.response.find("nodes");
        if (nodesIt != message.response.end()) {
            for (auto& node : parseCompactNodes(nodesIt->second)) {
                if (ctx.candidates.size() >= kMaxCandidates) {
                    break;
                }
                if (ctx.queriedEndpoints.count(ctx.endpointKey(node)) > 0) {
                    continue;  // 已查询过的不重复入队
                }
                ctx.candidates.push_back(std::move(node));
            }
        }

        // 吸收响应中的 peers（get_peers 命中时返回 values）
        auto valuesIt = message.response.find("values");
        if (valuesIt != message.response.end()) {
            auto peers = parseCompactPeers(valuesIt->second);
            ctx.foundPeers.insert(ctx.foundPeers.end(),
                                  std::make_move_iterator(peers.begin()),
                                  std::make_move_iterator(peers.end()));
        }

        // 收敛判定：无未查询候选且全部响应已收齐
        bool noUnqueried = std::none_of(ctx.candidates.begin(), ctx.candidates.end(),
                                        [&ctx](const DhtNode& n) {
                                            return ctx.queriedEndpoints.count(
                                                       ctx.endpointKey(n)) == 0;
                                        });
        finalizeNow = noUnqueried && ctx.outstandingEndpoints.empty();
    }

    // 响应中发现新候选 → 继续下一轮迭代（Kademlia 迭代逼近）
    if (finalizeNow) {
        finalizeLookup(lookupId);
    } else {
        continueLookup(lookupId);
    }
}

void DhtClient::finalizeLookup(uint64_t lookupId) {
    LookupContext ctx;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lookups_.find(lookupId);
        if (it == lookups_.end()) {
            return;
        }
        ctx = std::move(it->second);
        lookups_.erase(it);

        // 清理该查找尚未收到响应的事务注册
        for (auto iter = lookupTransactions_.begin();
             iter != lookupTransactions_.end();) {
            if (iter->second.first == lookupId) {
                pendingRequests_.erase(iter->first);
                iter = lookupTransactions_.erase(iter);
            } else {
                ++iter;
            }
        }
    }

    // 候选按距离排序取最近的已响应节点去重上报
    if (ctx.nodeCallback) {
        std::sort(ctx.respondedNodes.begin(), ctx.respondedNodes.end(),
                  [&ctx](const DhtNode& a, const DhtNode& b) {
                      return a.closerThan(b, ctx.target);
                  });
        std::set<DhtNodeId> reported;
        size_t reportedCount = 0;
        for (const auto& node : ctx.respondedNodes) {
            if (reportedCount >= kLookupK) {
                break;
            }
            if (!reported.insert(node.id).second) {
                continue;
            }
            ++reportedCount;
            ctx.nodeCallback(node);
        }
    }

    // peers 查找：一次性上报全部发现的 peers（空列表同样上报，表示查找结束）
    if (ctx.peersCallback) {
        ctx.peersCallback(ctx.infoHash, ctx.foundPeers);
    }
}

void DhtClient::expireStaleLookups() {
    std::vector<uint64_t> expired;
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(lookupTimeoutSeconds_.load());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : lookups_) {
            if (now - entry.second.startTime > timeout) {
                expired.push_back(entry.first);
            }
        }
    }

    // 终结在锁外：回调（用户代码）不得在持锁状态下执行
    for (uint64_t lookupId : expired) {
        falcon::detail::log_warnf("DHT lookup {} timed out, finalizing with partial results",
                                  lookupId);
        finalizeLookup(lookupId);
    }
}

} // namespace protocols
} // namespace falcon
