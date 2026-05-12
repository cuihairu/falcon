/**
 * @file pex_protocol.cpp
 * @brief BitTorrent PEX (Peer Exchange) 协议实现
 * @author Falcon Team
 * @date 2026-05-07
 */

// 取消 Windows 可能定义的 ERROR 宏，避免与日志宏冲突
#ifdef ERROR
#undef ERROR
#endif

#include "pex_protocol.hpp"
#include "bencode.hpp"
#include <falcon/logger.hpp>

#include <sstream>
#include <algorithm>
#include <iomanip>

namespace falcon {
namespace protocols {

namespace PexUtils {

std::string ipv6ToString(const uint8_t* data) {
    std::ostringstream oss;
    for (int i = 0; i < 16; ++i) {
        if (i > 0 && i % 2 == 0) {
            oss << ":";
        }
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::vector<uint8_t> stringToIPv6(const std::string& ip) {
    std::vector<uint8_t> result(16, 0);

    // 处理 IPv4 映射的 IPv6 地址 (::ffff:x.x.x.x)
    if (ip.find("::ffff:") == 0) {
        std::string ipv4 = ip.substr(7);
        // 解析 IPv4 部分到最后 4 个字节
        std::istringstream iss(ipv4);
        std::string octet;
        int idx = 12;
        while (std::getline(iss, octet, '.') && idx < 16) {
            result[idx++] = static_cast<uint8_t>(std::stoi(octet));
        }
        // 设置 IPv4 映射前缀
        result[10] = 0xff;
        result[11] = 0xff;
        return result;
    }

    // 标准解析处理
    std::string addr = ip;
    size_t doubleColon = addr.find("::");

    // 处理 :: 压缩
    if (doubleColon != std::string::npos) {
        // 计算现有段数
        std::string before = addr.substr(0, doubleColon);
        std::string after = addr.substr(doubleColon + 2);

        int beforeCount = 0;
        if (!before.empty()) {
            size_t pos = 0;
            while ((pos = before.find(':', pos)) != std::string::npos) {
                beforeCount++;
                pos++;
            }
            if (!before.empty()) beforeCount++;
        }

        int afterCount = 0;
        if (!after.empty()) {
            size_t pos = 0;
            while ((pos = after.find(':', pos)) != std::string::npos) {
                afterCount++;
                pos++;
            }
            if (!after.empty()) afterCount++;
        }

        int missing = 8 - beforeCount - afterCount;
        std::string expansion;
        for (int i = 0; i < missing; ++i) {
            expansion += (i == 0 ? "" : ":") + std::string("0");
        }

        if (before.empty()) {
            addr = expansion + (after.empty() ? "" : ":" + after);
        } else if (after.empty()) {
            addr = before + ":" + expansion;
        } else {
            addr = before + ":" + expansion + ":" + after;
        }
    }

    // 解析完整的 IPv6 地址
    std::istringstream iss(addr);
    std::string segment;
    int idx = 0;
    while (std::getline(iss, segment, ':') && idx < 8) {
        if (segment.empty()) continue;
        uint16_t value = static_cast<uint16_t>(std::stoi(segment, nullptr, 16));
        result[idx * 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        result[idx * 2 + 1] = static_cast<uint8_t>(value & 0xFF);
        idx++;
    }

    return result;
}

bool isValidPeerId(const std::string& peerId) {
    return peerId.size() == 20;
}

std::vector<PexPeer> parseCompactPeersIPv4(const std::vector<uint8_t>& data) {
    std::vector<PexPeer> peers;

    for (size_t i = 0; i + 6 <= data.size(); i += 6) {
        PexPeer peer;

        // IP 地址 (4 字节)
        peer.ip = std::to_string(data[i]) + "." +
                  std::to_string(data[i + 1]) + "." +
                  std::to_string(data[i + 2]) + "." +
                  std::to_string(data[i + 3]);

        // 端口 (2 字节，大端序)
        peer.port = (static_cast<uint16_t>(data[i + 4]) << 8) |
                   static_cast<uint16_t>(data[i + 5]);

        peers.push_back(peer);
    }

    return peers;
}

std::vector<PexPeer> parseCompactPeersIPv6(const std::vector<uint8_t>& data) {
    std::vector<PexPeer> peers;

    for (size_t i = 0; i + 18 <= data.size(); i += 18) {
        PexPeer peer;
        peer.ip = ipv6ToString(data.data() + i);

        // 端口 (2 字节，大端序)
        peer.port = (static_cast<uint16_t>(data[i + 16]) << 8) |
                   static_cast<uint16_t>(data[i + 17]);

        peers.push_back(peer);
    }

    return peers;
}

std::vector<uint8_t> encodeCompactPeersIPv4(const std::vector<PexPeer>& peers) {
    std::vector<uint8_t> data;
    data.reserve(peers.size() * 6);

    for (const auto& peer : peers) {
        // 解析 IP 地址
        uint8_t ipBytes[4] = {0, 0, 0, 0};
        std::istringstream iss(peer.ip);
        std::string octet;
        for (int i = 0; i < 4 && std::getline(iss, octet, '.'); ++i) {
            ipBytes[i] = static_cast<uint8_t>(std::stoi(octet));
        }

        // 添加 IP 和端口
        data.insert(data.end(), ipBytes, ipBytes + 4);
        data.push_back(static_cast<uint8_t>(peer.port >> 8));
        data.push_back(static_cast<uint8_t>(peer.port & 0xFF));
    }

    return data;
}

std::vector<uint8_t> encodeCompactPeersIPv6(const std::vector<PexPeer>& peers) {
    std::vector<uint8_t> data;
    data.reserve(peers.size() * 18);

    for (const auto& peer : peers) {
        auto ipBytes = stringToIPv6(peer.ip);
        data.insert(data.end(), ipBytes.begin(), ipBytes.end());
        data.push_back(static_cast<uint8_t>(peer.port >> 8));
        data.push_back(static_cast<uint8_t>(peer.port & 0xFF));
    }

    return data;
}

} // namespace PexUtils

//==============================================================================
// PexMessage
//==============================================================================

std::vector<uint8_t> PexMessage::encode() const {
    std::vector<uint8_t> data;

    switch (type) {
        case PexMessageType::Add:
        case PexMessageType::Drop:
            data = PexUtils::encodeCompactPeersIPv4(peers);
            break;
        case PexMessageType::Add6:
        case PexMessageType::Drop6:
            data = PexUtils::encodeCompactPeersIPv6(peers);
            break;
    }

    return data;
}

PexMessage PexMessage::decode(const std::vector<uint8_t>& data) {
    PexMessage msg;
    // 根据数据大小判断类型（通常在外部确定类型）
    msg.peers = PexUtils::parseCompactPeersIPv4(data);
    return msg;
}

//==============================================================================
// PexHandshake
//==============================================================================

std::string PexHandshake::encode() const {
    using namespace falcon::protocols;

    BencodeValue dict;

    // 添加扩展 ID 映射
    if (extensionIds.utPex > 0) {
        dict["ut_pex"].setInt(extensionIds.utPex);
    }
    if (extensionIds.ltPex > 0) {
        dict["lt_pex"].setInt(extensionIds.ltPex);
    }

    // 添加其他支持标志
    if (supportFlags != 0) {
        dict["supportflags"].setInt(supportFlags);
    }

    // 添加元数据大小
    if (!metadataSize.empty()) {
        dict["metadata_size"].setString(metadataSize);
    }

    // 添加请求队列大小
    if (!requestQueue.empty()) {
        dict["reqq"].setString(requestQueue);
    }

    return dict.encode();
}

PexHandshake PexHandshake::decode(const std::string& data) {
    using namespace falcon::protocols;

    PexHandshake hs;
    try {
        BencodeValue dict = BencodeValue::decode(data);

        if (!dict.isDict()) {
            return hs;
        }

        // 解析扩展 ID
        if (dict.hasKey("ut_pex") && dict["ut_pex"].isInt()) {
            hs.extensionIds.utPex = static_cast<uint8_t>(dict["ut_pex"].asInt());
        }
        if (dict.hasKey("lt_pex") && dict["lt_pex"].isInt()) {
            hs.extensionIds.ltPex = static_cast<uint8_t>(dict["lt_pex"].asInt());
        }

        // 解析支持标志
        if (dict.hasKey("supportflags") && dict["supportflags"].isInt()) {
            hs.supportFlags = static_cast<int32_t>(dict["supportflags"].asInt());
        }

        // 解析元数据大小
        if (dict.hasKey("metadata_size") && dict["metadata_size"].isString()) {
            hs.metadataSize = dict["metadata_size"].asString();
        }

        // 解析请求队列大小
        if (dict.hasKey("reqq") && dict["reqq"].isString()) {
            hs.requestQueue = dict["reqq"].asString();
        }

    } catch (const BencodeException&) {
        // 解析失败，返回空握手
    }

    return hs;
}

//==============================================================================
// PexManager
//==============================================================================

PexManager::PexManager(const std::string& infoHash) : infoHash_(infoHash) {}

void PexManager::addConnectedPeer(const std::string& ip, uint16_t port, const std::string& peerId) {
    std::lock_guard<std::mutex> lock(connectedPeersMutex_);

    if (connectedPeers_.size() >= MAX_CONNECTED_PEERS) {
        return;
    }

    std::string key = ip + ":" + std::to_string(port);
    PexManagedPeer peer;
    peer.ip = ip;
    peer.port = port;
    peer.peerId = peerId;
    peer.state = PeerConnectionState::Connected;
    peer.lastSeen = std::chrono::steady_clock::now();
    peer.lastAdded = peer.lastSeen;

    connectedPeers_[key] = peer;

    // 从候选列表移除
    candidatePeers_.erase({ip, port});
}

void PexManager::removePeer(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(connectedPeersMutex_);

    std::string key = ip + ":" + std::to_string(port);
    auto it = connectedPeers_.find(key);
    if (it != connectedPeers_.end()) {
        // 添加到最近列表，避免短期内重新添加
        std::lock_guard<std::mutex> recentLock(recentPeersMutex_);
        recentPeers_.insert({ip, port});

        connectedPeers_.erase(it);
    }
}

void PexManager::handlePexMessage(const PexMessage& message) {
    for (const auto& peer : message.peers) {
        std::string key = peer.ip + ":" + std::to_string(peer.port);

        // 检查是否已连接
        {
            std::lock_guard<std::mutex> lock(connectedPeersMutex_);
            if (connectedPeers_.find(key) != connectedPeers_.end()) {
                continue;
            }
        }

        // 检查最近列表
        {
            std::lock_guard<std::mutex> lock(recentPeersMutex_);
            if (recentPeers_.find({peer.ip, peer.port}) != recentPeers_.end()) {
                continue;
            }
        }

        // 根据 PEX 消息类型处理
        switch (message.type) {
            case PexMessageType::Add:
            case PexMessageType::Add6:
                // 添加到候选列表
                {
                    std::lock_guard<std::mutex> lock(candidatePeersMutex_);
                    if (candidatePeers_.size() < MAX_CANDIDATE_PEERS) {
                        candidatePeers_.insert({peer.ip, peer.port});

                        // 触发回调
                        if (onPeerDiscovered_) {
                            onPeerDiscovered_(peer);
                        }
                    }
                }
                break;

            case PexMessageType::Drop:
            case PexMessageType::Drop6:
                // 从候选列表移除
                {
                    std::lock_guard<std::mutex> lock(candidatePeersMutex_);
                    candidatePeers_.erase({peer.ip, peer.port});

                    // 触发回调
                    if (onPeerDropped_) {
                        onPeerDropped_(peer.ip, peer.port);
                    }
                }
                break;
        }
    }
}

PexMessage PexManager::createPexMessage(PexMessageType type, size_t maxPeers) const {
    PexMessage msg;
    msg.type = type;

    std::lock_guard<std::mutex> lock(connectedPeersMutex_);

    // 收集要发送的 peers
    for (const auto& [key, peer] : connectedPeers_) {
        if (msg.peers.size() >= maxPeers) {
            break;
        }

        // 只发送活跃的 peers
        if (peer.state == PeerConnectionState::Active) {
            PexPeer pexPeer;
            pexPeer.ip = peer.ip;
            pexPeer.port = peer.port;
            pexPeer.isSeed = peer.isSeed;
            msg.peers.push_back(pexPeer);
        }
    }

    return msg;
}

void PexManager::setPeerDiscoveredCallback(PeerDiscoveredCallback callback) {
    onPeerDiscovered_ = std::move(callback);
}

void PexManager::setPeerDroppedCallback(PeerDroppedCallback callback) {
    onPeerDropped_ = std::move(callback);
}

size_t PexManager::getManagedPeerCount() const {
    std::lock_guard<std::mutex> lock(connectedPeersMutex_);
    return connectedPeers_.size();
}

size_t PexManager::getCandidatePeerCount() const {
    std::lock_guard<std::mutex> lock(candidatePeersMutex_);
    return candidatePeers_.size();
}

std::vector<PexPeer> PexManager::getCandidatePeers(size_t maxCount) const {
    std::lock_guard<std::mutex> lock(candidatePeersMutex_);

    std::vector<PexPeer> peers;
    for (const auto& peer : candidatePeers_) {
        if (peers.size() >= maxCount) {
            break;
        }
        peers.push_back({peer.first, peer.second});
    }
    return peers;
}

void PexManager::updatePeerState(const std::string& ip, uint16_t port,
                                 PeerConnectionState state, bool isSeed) {
    std::lock_guard<std::mutex> lock(connectedPeersMutex_);

    std::string key = ip + ":" + std::to_string(port);
    auto it = connectedPeers_.find(key);
    if (it != connectedPeers_.end()) {
        it->second.state = state;
        it->second.isSeed = isSeed;
        it->second.lastSeen = std::chrono::steady_clock::now();
    }
}

//==============================================================================
// PexExtensionHandler
//==============================================================================

PexExtensionHandler::PexExtensionHandler(const std::string& infoHash)
    : infoHash_(infoHash), manager_(infoHash) {}

bool PexExtensionHandler::handleExtensionHandshake(const std::string& data) {
    auto handshake = PexHandshake::decode(data);

    // 检查对方是否支持 PEX
    if (handshake.extensionIds.utPex > 0 || handshake.extensionIds.ltPex > 0) {
        extensionIds_ = handshake.extensionIds;
        return true;
    }

    return false;
}

void PexExtensionHandler::handlePexMessage(uint8_t extId, const std::vector<uint8_t>& data) {
    if (!enabled_.load()) {
        return;
    }

    // 确定消息类型
    PexMessageType type;
    if (extId == extensionIds_.utPex) {
        // uTorrent PEX: 根据扩展 ID 确定类型
        // 通常通过不同消息发送不同类型
        type = PexMessageType::Add;
    } else if (extId == extensionIds_.ltPex) {
        type = PexMessageType::Add;
    } else {
        return;
    }

    PexMessage msg;
    msg.type = type;
    msg.peers = PexUtils::parseCompactPeersIPv4(data);

    manager_.handlePexMessage(msg);
}

void PexExtensionHandler::sendPexMessage(PexMessageType type) {
    if (!enabled_.load() || !sendCallback_) {
        return;
    }

    auto msg = manager_.createPexMessage(type);
    auto data = msg.encode();

    // 发送 uTorrent PEX
    if (extensionIds_.utPex > 0) {
        sendCallback_(extensionIds_.utPex, data);
    }

    // 发送 libtorrent PEX
    if (extensionIds_.ltPex > 0) {
        sendCallback_(extensionIds_.ltPex, data);
    }
}

void PexExtensionHandler::setSendCallback(SendExtensionCallback callback) {
    sendCallback_ = std::move(callback);
}

void PexExtensionHandler::enable() {
    enabled_.store(true);
    falcon::detail::log_infof("PEX enabled for torrent: {}", infoHash_);
}

void PexExtensionHandler::disable() {
    enabled_.store(false);
    falcon::detail::log_infof("PEX disabled for torrent: {}", infoHash_);
}

} // namespace protocols
} // namespace falcon
