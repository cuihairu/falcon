/**
 * @file pex_protocol_test.cpp
 * @brief BitTorrent PEX (Peer Exchange) 协议单元测试
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 覆盖：compact peer 编解码（IPv4/IPv6）、PexMessage/PexHandshake 编解码、
 * PexManager 的 peer 生命周期（去重/回调/上限）、PexExtensionHandler 的
 * 握手协商与消息收发门禁。
 */

#include "pex_protocol.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace falcon::protocols;

namespace {

const std::string kInfoHash(20, '\x11');

PexPeer makePeer(const std::string& ip, uint16_t port) {
    PexPeer peer;
    peer.ip = ip;
    peer.port = port;
    return peer;
}

} // namespace

//==============================================================================
// PexUtils：compact 编解码与 IPv6 转换
//==============================================================================

TEST(PexUtilsTest, CompactIPv4RoundTrip) {
    std::vector<PexPeer> peers = {
        makePeer("192.168.1.7", 6881),
        makePeer("10.0.0.255", 51413),
    };

    auto encoded = PexUtils::encodeCompactPeersIPv4(peers);
    ASSERT_EQ(encoded.size(), size_t{12});

    // 手工核对第一个 peer 的字节序（大端端口）
    EXPECT_EQ(encoded[0], 192);
    EXPECT_EQ(encoded[1], 168);
    EXPECT_EQ(encoded[2], 1);
    EXPECT_EQ(encoded[3], 7);
    EXPECT_EQ(encoded[4], (6881 >> 8) & 0xFF);
    EXPECT_EQ(encoded[5], 6881 & 0xFF);

    auto decoded = PexUtils::parseCompactPeersIPv4(encoded);
    ASSERT_EQ(decoded.size(), size_t{2});
    EXPECT_EQ(decoded[0].ip, "192.168.1.7");
    EXPECT_EQ(decoded[0].port, 6881);
    EXPECT_EQ(decoded[1].ip, "10.0.0.255");
    EXPECT_EQ(decoded[1].port, 51413);
}

TEST(PexUtilsTest, CompactIPv4IgnoresTrailingPartialPeer) {
    // 8 字节 = 1 完整 peer + 2 字节残片：残片被忽略
    std::vector<uint8_t> data = {10, 0, 0, 1, 0x1A, 0xE1, 0xFF, 0xFF};
    auto peers = PexUtils::parseCompactPeersIPv4(data);
    ASSERT_EQ(peers.size(), size_t{1});
    EXPECT_EQ(peers[0].port, 6881);
}

TEST(PexUtilsTest, CompactIPv6RoundTrip) {
    std::vector<PexPeer> peers = {makePeer("2001:db8::1", 6969)};

    auto encoded = PexUtils::encodeCompactPeersIPv6(peers);
    ASSERT_EQ(encoded.size(), size_t{18});

    auto decoded = PexUtils::parseCompactPeersIPv6(encoded);
    ASSERT_EQ(decoded.size(), size_t{1});
    EXPECT_EQ(decoded[0].ip, "2001:0db8:0000:0000:0000:0000:0000:0001");
    EXPECT_EQ(decoded[0].port, 6969);
}

TEST(PexUtilsTest, CompactIPv6IgnoresTrailingPartialPeer) {
    std::vector<uint8_t> data(20, 0x01);  // 18 完整 + 2 残片
    auto peers = PexUtils::parseCompactPeersIPv6(data);
    ASSERT_EQ(peers.size(), size_t{1});
}

TEST(PexUtilsTest, StringToIPv6FullForm) {
    auto bytes = PexUtils::stringToIPv6("2001:0db8:0000:0000:0000:0000:0000:0001");
    ASSERT_EQ(bytes.size(), size_t{16});
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x01);
    EXPECT_EQ(bytes[2], 0x0d);
    EXPECT_EQ(bytes[3], 0xb8);
    EXPECT_EQ(bytes[15], 0x01);
}

TEST(PexUtilsTest, StringToIPv6DoubleColonExpansion) {
    // ::1 → 前面 7 个零段
    auto loopback = PexUtils::stringToIPv6("::1");
    for (size_t i = 0; i < 15; ++i) {
        EXPECT_EQ(loopback[i], 0) << "byte " << i;
    }
    EXPECT_EQ(loopback[15], 1);

    // 2001:db8::1 → 中段压缩
    auto mid = PexUtils::stringToIPv6("2001:db8::1");
    EXPECT_EQ(mid[0], 0x20);
    EXPECT_EQ(mid[3], 0xb8);
    EXPECT_EQ(mid[15], 1);

    // 未指定地址 ::
    auto unspecified = PexUtils::stringToIPv6("::");
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(unspecified[i], 0);
    }
}

TEST(PexUtilsTest, StringToIPv6MappedIPv4) {
    auto mapped = PexUtils::stringToIPv6("::ffff:192.168.1.1");
    // IPv4 映射前缀 ::ffff/96
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(mapped[i], 0) << "byte " << i;
    }
    EXPECT_EQ(mapped[10], 0xff);
    EXPECT_EQ(mapped[11], 0xff);
    EXPECT_EQ(mapped[12], 192);
    EXPECT_EQ(mapped[13], 168);
    EXPECT_EQ(mapped[14], 1);
    EXPECT_EQ(mapped[15], 1);
}

TEST(PexUtilsTest, IsValidPeerId) {
    EXPECT_TRUE(PexUtils::isValidPeerId(std::string(20, 'a')));
    EXPECT_FALSE(PexUtils::isValidPeerId(std::string(19, 'a')));
    EXPECT_FALSE(PexUtils::isValidPeerId(""));
}

//==============================================================================
// PexMessage / PexHandshake 编解码
//==============================================================================

TEST(PexMessageTest, EncodeAddUsesIPv4CompactFormat) {
    PexMessage msg;
    msg.type = PexMessageType::Add;
    msg.peers = {makePeer("10.1.2.3", 6881), makePeer("10.1.2.4", 6882)};

    auto data = msg.encode();
    ASSERT_EQ(data.size(), size_t{12});
    EXPECT_EQ(data[0], 10);
    EXPECT_EQ(data[3], 3);
    EXPECT_EQ(data[6], 10);
    EXPECT_EQ(data[9], 4);
}

TEST(PexMessageTest, EncodeAdd6UsesIPv6CompactFormat) {
    PexMessage msg;
    msg.type = PexMessageType::Add6;
    msg.peers = {makePeer("::1", 6969)};

    auto data = msg.encode();
    ASSERT_EQ(data.size(), size_t{18});
    EXPECT_EQ(data[15], 1);  // ::1 的最后一个字节
    EXPECT_EQ(data[16], (6969 >> 8) & 0xFF);
    EXPECT_EQ(data[17], 6969 & 0xFF);
}

TEST(PexMessageTest, DecodeParsesIPv4Peers) {
    std::vector<uint8_t> data = {192, 168, 0, 9, 0x1A, 0xE1};
    auto msg = PexMessage::decode(data);
    ASSERT_EQ(msg.peers.size(), size_t{1});
    EXPECT_EQ(msg.peers[0].ip, "192.168.0.9");
    EXPECT_EQ(msg.peers[0].port, 6881);
}

TEST(PexHandshakeTest, RoundTripPreservesAllFields) {
    PexHandshake hs;
    hs.supportFlags = 0x0F;
    hs.extensionIds.utPex = 2;
    hs.extensionIds.ltPex = 5;
    hs.metadataSize = "16384";
    hs.requestQueue = "250";

    auto decoded = PexHandshake::decode(hs.encode());
    EXPECT_EQ(decoded.extensionIds.utPex, 2);
    EXPECT_EQ(decoded.extensionIds.ltPex, 5);
    EXPECT_EQ(decoded.supportFlags, 0x0F);
    EXPECT_EQ(decoded.metadataSize, "16384");
    EXPECT_EQ(decoded.requestQueue, "250");
}

TEST(PexHandshakeTest, EmptyFieldsOmittedFromDict) {
    PexHandshake hs;  // 全默认：只编码 ut_pex
    hs.extensionIds.utPex = 1;

    auto decoded = PexHandshake::decode(hs.encode());
    EXPECT_EQ(decoded.extensionIds.utPex, 1);
    EXPECT_EQ(decoded.extensionIds.ltPex, 0);
    EXPECT_EQ(decoded.supportFlags, 0);
    EXPECT_TRUE(decoded.metadataSize.empty());
    EXPECT_TRUE(decoded.requestQueue.empty());
}

TEST(PexHandshakeTest, InvalidBencodeReturnsEmptyHandshake) {
    auto hs = PexHandshake::decode("not-bencode-at-all");
    EXPECT_EQ(hs.extensionIds.utPex, 0);
    EXPECT_EQ(hs.supportFlags, 0);
}

//==============================================================================
// PexManager：peer 生命周期
//==============================================================================

class PexManagerTest : public ::testing::Test {
protected:
    void SetUp() override { manager_ = std::make_unique<PexManager>(kInfoHash); }

    std::unique_ptr<PexManager> manager_;
};

TEST_F(PexManagerTest, AddAndRemoveConnectedPeers) {
    EXPECT_EQ(manager_->getInfoHash(), kInfoHash);
    EXPECT_EQ(manager_->getManagedPeerCount(), size_t{0});

    manager_->addConnectedPeer("10.0.0.1", 6881, std::string(20, 'a'));
    manager_->addConnectedPeer("10.0.0.2", 6882, std::string(20, 'b'));
    EXPECT_EQ(manager_->getManagedPeerCount(), size_t{2});

    // 同端点重复添加不增长
    manager_->addConnectedPeer("10.0.0.1", 6881, std::string(20, 'c'));
    EXPECT_EQ(manager_->getManagedPeerCount(), size_t{2});

    manager_->removePeer("10.0.0.1", 6881);
    EXPECT_EQ(manager_->getManagedPeerCount(), size_t{1});

    // 移除不存在的 peer 无副作用
    manager_->removePeer("10.0.0.1", 6881);
    EXPECT_EQ(manager_->getManagedPeerCount(), size_t{1});
}

TEST_F(PexManagerTest, ConnectedPeersNotReaddedFromPex) {
    manager_->addConnectedPeer("10.0.0.3", 6883, std::string(20, 'a'));

    PexMessage msg;
    msg.type = PexMessageType::Add;
    msg.peers = {makePeer("10.0.0.3", 6883)};

    int discovered = 0;
    manager_->setPeerDiscoveredCallback([&discovered](const PexPeer&) { discovered++; });
    manager_->handlePexMessage(msg);

    EXPECT_EQ(discovered, 0);
    EXPECT_EQ(manager_->getCandidatePeerCount(), size_t{0});
}

TEST_F(PexManagerTest, DiscoveredPeersEnterCandidateList) {
    PexMessage msg;
    msg.type = PexMessageType::Add;
    msg.peers = {makePeer("10.0.0.1", 100), makePeer("10.0.0.2", 200),
                 makePeer("10.0.0.1", 100)};  // 重复项

    int discovered = 0;
    manager_->setPeerDiscoveredCallback([&discovered](const PexPeer& peer) {
        discovered++;
        EXPECT_FALSE(peer.ip.empty());
    });
    manager_->handlePexMessage(msg);

    EXPECT_EQ(discovered, 2);
    EXPECT_EQ(manager_->getCandidatePeerCount(), size_t{2});

    auto candidates = manager_->getCandidatePeers(10);
    ASSERT_EQ(candidates.size(), size_t{2});

    // maxCount 截断
    EXPECT_EQ(manager_->getCandidatePeers(1).size(), size_t{1});
}

TEST_F(PexManagerTest, DropRemovesCandidateAndFiresCallback) {
    PexMessage add;
    add.type = PexMessageType::Add;
    add.peers = {makePeer("10.0.0.5", 555)};
    manager_->handlePexMessage(add);
    EXPECT_EQ(manager_->getCandidatePeerCount(), size_t{1});

    int dropped = 0;
    manager_->setPeerDroppedCallback(
        [&dropped](const std::string& ip, uint16_t port) {
            dropped++;
            EXPECT_EQ(ip, "10.0.0.5");
            EXPECT_EQ(port, 555);
        });

    PexMessage drop;
    drop.type = PexMessageType::Drop;
    drop.peers = {makePeer("10.0.0.5", 555)};
    manager_->handlePexMessage(drop);

    EXPECT_EQ(dropped, 1);
    EXPECT_EQ(manager_->getCandidatePeerCount(), size_t{0});
}

TEST_F(PexManagerTest, RemovedPeerNotReaddedFromRecentList) {
    manager_->addConnectedPeer("10.0.0.9", 999, std::string(20, 'a'));
    manager_->removePeer("10.0.0.9", 999);

    PexMessage msg;
    msg.type = PexMessageType::Add;
    msg.peers = {makePeer("10.0.0.9", 999)};
    manager_->handlePexMessage(msg);

    // 刚移除的 peer 进 recent 列表，PEX 重发不再成为候选
    EXPECT_EQ(manager_->getCandidatePeerCount(), size_t{0});
}

TEST_F(PexManagerTest, CreatePexMessageOnlyIncludesActivePeers) {
    manager_->addConnectedPeer("10.0.0.1", 111, std::string(20, 'a'));
    manager_->addConnectedPeer("10.0.0.2", 222, std::string(20, 'b'));
    manager_->addConnectedPeer("10.0.0.3", 333, std::string(20, 'c'));

    // 只有 Active 状态的 peer 进入交换消息
    manager_->updatePeerState("10.0.0.1", 111, PeerConnectionState::Active, true);
    manager_->updatePeerState("10.0.0.2", 222, PeerConnectionState::Active);

    auto msg = manager_->createPexMessage(PexMessageType::Add, 50);
    ASSERT_EQ(msg.peers.size(), size_t{2});
    EXPECT_EQ(msg.peers[0].ip, "10.0.0.1");
    EXPECT_TRUE(msg.peers[0].isSeed);
    EXPECT_FALSE(msg.peers[1].isSeed);

    // maxPeers 截断
    auto capped = manager_->createPexMessage(PexMessageType::Add, 1);
    EXPECT_EQ(capped.peers.size(), size_t{1});
}

TEST_F(PexManagerTest, UpdatePeerStateIgnoresUnknownPeer) {
    manager_->updatePeerState("10.0.0.404", 1, PeerConnectionState::Active);
    EXPECT_EQ(manager_->getManagedPeerCount(), size_t{0});
}

TEST_F(PexManagerTest, ConnectedPeerCap) {
    for (size_t i = 0; i < 105; ++i) {
        manager_->addConnectedPeer("10.1.0." + std::to_string(i % 256),
                                   static_cast<uint16_t>(i),
                                   std::string(20, 'x'));
    }
    EXPECT_EQ(manager_->getManagedPeerCount(), size_t{100});
}

TEST_F(PexManagerTest, CandidateCap) {
    PexMessage msg;
    msg.type = PexMessageType::Add;
    for (size_t i = 0; i < 305; ++i) {
        msg.peers.push_back(makePeer("10.2." + std::to_string(i / 256) + "." +
                                         std::to_string(i % 256),
                                     static_cast<uint16_t>(i + 1)));
    }
    manager_->handlePexMessage(msg);
    EXPECT_EQ(manager_->getCandidatePeerCount(), size_t{300});
}

//==============================================================================
// PexExtensionHandler：握手协商与收发门禁
//==============================================================================

class PexExtensionHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler_ = std::make_unique<PexExtensionHandler>(kInfoHash);
        handler_->setSendCallback([this](uint8_t extId, const std::vector<uint8_t>& data) {
            sent_.push_back({extId, data});
        });
    }

    struct Sent {
        uint8_t extId;
        std::vector<uint8_t> data;
    };

    std::unique_ptr<PexExtensionHandler> handler_;
    std::vector<Sent> sent_;
};

TEST_F(PexExtensionHandlerTest, HandshakeNegotiatesPexSupport) {
    // 对方声明 ut_pex=2
    EXPECT_TRUE(handler_->handleExtensionHandshake("d6:ut_pexi2ee"));
    EXPECT_FALSE(handler_->isEnabled());

    handler_->enable();
    EXPECT_TRUE(handler_->isEnabled());

    handler_->sendPexMessage(PexMessageType::Add);
    ASSERT_EQ(sent_.size(), size_t{1});
    EXPECT_EQ(sent_[0].extId, 2);  // 用协商出的扩展 ID 发送
}

TEST_F(PexExtensionHandlerTest, HandshakeWithoutPexSupportRejected) {
    EXPECT_FALSE(handler_->handleExtensionHandshake("d4:blahi1ee"));

    handler_->enable();
    handler_->sendPexMessage(PexMessageType::Add);
    EXPECT_TRUE(sent_.empty());  // 未协商出扩展 ID，不发送
}

TEST_F(PexExtensionHandlerTest, InvalidHandshakeRejected) {
    EXPECT_FALSE(handler_->handleExtensionHandshake("\xFF\xFE garbage"));
}

TEST_F(PexExtensionHandlerTest, SendGatedOnEnabledAndCallback) {
    // 未 enable：即使有回调也不发送
    EXPECT_TRUE(handler_->handleExtensionHandshake("d6:ut_pexi1ee"));
    handler_->sendPexMessage(PexMessageType::Add);
    EXPECT_TRUE(sent_.empty());

    // enable 后发送
    handler_->enable();
    handler_->sendPexMessage(PexMessageType::Add);
    EXPECT_EQ(sent_.size(), size_t{1});
}

TEST_F(PexExtensionHandlerTest, HandlePexMessageGatesAndDispatches) {
    EXPECT_TRUE(handler_->handleExtensionHandshake("d6:ut_pexi3ee"));

    auto peers = PexUtils::encodeCompactPeersIPv4({makePeer("10.0.0.7", 777)});

    // 未 enable：消息被忽略
    handler_->handlePexMessage(3, peers);
    EXPECT_EQ(handler_->getManager().getCandidatePeerCount(), size_t{0});

    handler_->enable();
    // 未知扩展 ID：忽略
    handler_->handlePexMessage(99, peers);
    EXPECT_EQ(handler_->getManager().getCandidatePeerCount(), size_t{0});

    // 协商的扩展 ID：peers 进入候选
    handler_->handlePexMessage(3, peers);
    EXPECT_EQ(handler_->getManager().getCandidatePeerCount(), size_t{1});
}

TEST_F(PexExtensionHandlerTest, DisableStopsReceiving) {
    EXPECT_TRUE(handler_->handleExtensionHandshake("d6:ut_pexi1ee"));
    handler_->enable();
    handler_->disable();
    EXPECT_FALSE(handler_->isEnabled());

    auto peers = PexUtils::encodeCompactPeersIPv4({makePeer("10.0.0.8", 888)});
    handler_->handlePexMessage(1, peers);
    EXPECT_EQ(handler_->getManager().getCandidatePeerCount(), size_t{0});
}

TEST_F(PexExtensionHandlerTest, SendsViaBothExtensionIds) {
    PexHandshake hs;
    hs.extensionIds.utPex = 2;
    hs.extensionIds.ltPex = 5;
    ASSERT_TRUE(handler_->handleExtensionHandshake(hs.encode()));

    handler_->getManager().addConnectedPeer("10.0.0.6", 666, std::string(20, 'p'));
    handler_->getManager().updatePeerState("10.0.0.6", 666, PeerConnectionState::Active);

    handler_->enable();
    handler_->sendPexMessage(PexMessageType::Add);

    // ut 与 lt 两个扩展 ID 各发一次，载荷相同
    ASSERT_EQ(sent_.size(), size_t{2});
    EXPECT_EQ(sent_[0].extId, 2);
    EXPECT_EQ(sent_[1].extId, 5);
    EXPECT_EQ(sent_[0].data, sent_[1].data);
    EXPECT_EQ(sent_[0].data.size(), size_t{6});  // 1 个 IPv4 peer
}
