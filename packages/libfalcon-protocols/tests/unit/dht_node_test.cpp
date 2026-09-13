/**
 * @file dht_node_test.cpp
 * @brief DHT 客户端 Kademlia 迭代查找测试
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 通过本地 UDP mock DHT 节点构成两跳网络，端到端验证：
 * - get_peers/find_node 查找的迭代逼近（响应中发现新节点后继续查询）
 * - peers/节点回调在查找收敛时上报
 * - 超时终结（部分结果上报，不悬挂）
 * - 收敛后不再重复查询
 */

#include "dht_node.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

using namespace falcon;
using namespace falcon::protocols;

namespace {

void closeSocket(int fd) {
    if (fd < 0) {
        return;
    }
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

// 可编程响应的本地 mock DHT 节点：记录收到的查询，按注册的构造器回复
class MockDhtNode {
public:
    MockDhtNode() {
        socket_ = static_cast<int>(
            ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (socket_ < 0) {
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        // INADDR_ANY 与 DhtClient 的绑定一致（INADDR_LOOPBACK 在部分
        // 沙盒环境的字节序处理有差异，ANY 最稳）
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;
        if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            closeSocket(socket_);
            socket_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        getsockname(socket_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        // 接收超时 100ms：保证 stop() 的 join 快速返回
#ifdef _WIN32
        DWORD timeoutMs = 100;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        thread_ = std::thread([this] { loop(); });
    }

    ~MockDhtNode() { stop(); }

    MockDhtNode(const MockDhtNode&) = delete;
    MockDhtNode& operator=(const MockDhtNode&) = delete;

    void stop() {
        if (socket_ < 0) {
            return;
        }
        stopped_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        closeSocket(socket_);
        socket_ = -1;
    }

    bool valid() const { return socket_ >= 0; }
    uint16_t port() const { return port_; }

    // 收到查询时构造响应（transactionId 由 mock 自动回填）
    std::function<DhtMessage(const DhtMessage&)> responder;
    std::atomic<int> request_count{0};

    std::vector<DhtMessage> received_queries() const {
        std::lock_guard<std::mutex> lock(receivedMutex_);
        return received_;
    }

private:
    void loop() {
        uint8_t buffer[4096];
        while (!stopped_.load()) {
            sockaddr_in sender{};
            socklen_t senderLen = sizeof(sender);
#ifdef _WIN32
            int n = recvfrom(socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                             reinterpret_cast<sockaddr*>(&sender), &senderLen);
#else
            ssize_t n = recvfrom(socket_, buffer, sizeof(buffer), 0,
                                 reinterpret_cast<sockaddr*>(&sender), &senderLen);
#endif
            if (n <= 0) {
                continue;  // 超时
            }

            DhtMessage msg = DhtMessage::decode(std::string(buffer, buffer + n));
            if (msg.type != DhtMessageType::Query) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(receivedMutex_);
                received_.push_back(msg);
            }
            request_count.fetch_add(1);

            if (responder) {
                DhtMessage resp = responder(msg);
                resp.transactionId = msg.transactionId;
                std::string data = resp.encode();
                sendto(socket_, data.data(), static_cast<int>(data.size()), 0,
                       reinterpret_cast<sockaddr*>(&sender), senderLen);
            }
        }
    }

    int socket_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
    mutable std::mutex receivedMutex_;
    std::vector<DhtMessage> received_;
};

// BEP-005 compact node info：20 id + 4 ip + 2 port（大端序）
std::string encodeCompactNode(const DhtNodeId& id, const std::string& ip, uint16_t port) {
    std::string s(reinterpret_cast<const char*>(id.data()), id.size());
    unsigned int a = 0, b = 0, c = 0, d = 0;
    std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
    s.push_back(static_cast<char>(a));
    s.push_back(static_cast<char>(b));
    s.push_back(static_cast<char>(c));
    s.push_back(static_cast<char>(d));
    s.push_back(static_cast<char>((port >> 8) & 0xFF));
    s.push_back(static_cast<char>(port & 0xFF));
    return s;
}

// BEP-005 compact peer info：4 ip + 2 port
std::string encodeCompactPeer(const std::string& ip, uint16_t port) {
    std::string s;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
    s.push_back(static_cast<char>(a));
    s.push_back(static_cast<char>(b));
    s.push_back(static_cast<char>(c));
    s.push_back(static_cast<char>(d));
    s.push_back(static_cast<char>((port >> 8) & 0xFF));
    s.push_back(static_cast<char>(port & 0xFF));
    return s;
}

DhtMessage makeResponse(const DhtNodeId& responderId, const std::string& nodes,
                        const std::string& values) {
    DhtMessage resp;
    resp.type = DhtMessageType::Response;
    resp.nodeId = responderId;
    if (!nodes.empty()) {
        resp.response["nodes"] = nodes;
    }
    if (!values.empty()) {
        resp.response["values"] = values;
    }
    return resp;
}

// 与 target 指定字节异或，构造特定距离的节点 ID
DhtNodeId idNearTarget(const DhtNodeId& target, size_t xorByteIndex, uint8_t mask) {
    DhtNodeId id = target;
    id[xorByteIndex] = static_cast<uint8_t>(id[xorByteIndex] ^ mask);
    return id;
}

const std::string kTestInfoHash(40, 'a');  // 40 hex 字符 → 20 字节

using PeerList = std::vector<std::pair<std::string, uint16_t>>;

class DhtClientTest : public ::testing::Test {
protected:
    void SetUp() override { infoHashId_ = DhtUtils::nodeIdFromString(kTestInfoHash); }

    // 启动一个绑定随机端口的 DhtClient，bootstrap 只指向 given 节点
    std::unique_ptr<DhtClient> makeClient(const MockDhtNode& bootstrap) {
        auto client = std::make_unique<DhtClient>(0);
        // 清掉构造时预置的公网引导节点，保证查找只经过本地 mock
        client->clear_bootstrap_nodes();
        client->addBootstrapNode("127.0.0.1", bootstrap.port());
        client->start();
        return client;
    }

    DhtNodeId infoHashId_;
};

} // namespace

//==============================================================================
// DhtUtils 行为
//==============================================================================

TEST(DhtUtilsTest, NodeIdStringRoundTrip) {
    DhtNodeId id{};
    for (size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<uint8_t>(i * 13 + 1);
    }
    std::string hex = DhtUtils::nodeIdToString(id);
    EXPECT_EQ(hex.size(), size_t{40});
    DhtNodeId back = DhtUtils::nodeIdFromString(hex);
    EXPECT_EQ(back, id);
}

TEST(DhtUtilsTest, XorDistanceAndOrdering) {
    DhtNodeId target{};
    target[0] = 0x80;
    DhtNodeId a = target;  // 距离 0
    DhtNodeId b = target;
    b[0] = static_cast<uint8_t>(b[0] ^ 0x80);  // 距离最大

    DhtNodeId d0 = DhtUtils::xorDistance(a, target);
    EXPECT_EQ(d0, (DhtNodeId{}));

    // a 比 b 更接近 target
    EXPECT_TRUE(DhtUtils::distanceLessThan(a, b, target));
    EXPECT_FALSE(DhtUtils::distanceLessThan(b, a, target));
}

//==============================================================================
// 迭代查找端到端
//==============================================================================

// 两跳迭代：bootstrap 响应中发现新节点 → 继续查询 → 收敛上报 peers
TEST_F(DhtClientTest, FindPeersIteratesThroughTwoHops) {
    MockDhtNode nodeA;
    MockDhtNode nodeB;
    ASSERT_TRUE(nodeA.valid());
    ASSERT_TRUE(nodeB.valid());

    // A 远离 target，B 靠近 target（A 的响应中发现 B）
    DhtNodeId idA = idNearTarget(infoHashId_, 0, 0xFF);
    DhtNodeId idB = idNearTarget(infoHashId_, 19, 0x01);

    std::string nodesAB = encodeCompactNode(idA, "127.0.0.1", nodeA.port()) +
                          encodeCompactNode(idB, "127.0.0.1", nodeB.port());

    nodeA.responder = [idA, &nodeA, &nodeB, nodesAB](const DhtMessage&) {
        return makeResponse(idA, nodesAB, "");
    };
    nodeB.responder = [idB, &nodeA, &nodeB, nodesAB](const DhtMessage&) {
        // 命中：返回两个 peers
        std::string values = encodeCompactPeer("10.0.0.1", 6881) +
                             encodeCompactPeer("10.0.0.2", 6882);
        return makeResponse(idB, nodesAB, values);
    };

    auto client = makeClient(nodeA);

    std::promise<PeerList> promise;
    auto future = promise.get_future();
    client->findPeers(kTestInfoHash,
                      [&promise](const std::string&, const PeerList& peers) {
                          promise.set_value(peers);
                      });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    PeerList peers = future.get();

    // 迭代必须到达 B 并带回 peers
    ASSERT_EQ(peers.size(), size_t{2});
    EXPECT_EQ(peers[0], std::make_pair(std::string("10.0.0.1"), uint16_t{6881}));
    EXPECT_EQ(peers[1], std::make_pair(std::string("10.0.0.2"), uint16_t{6882}));
    EXPECT_EQ(nodeB.request_count.load(), 1);

    // bootstrap 只被查询一轮（响应中的真实 ID 不导致同一端点重复查询）
    EXPECT_EQ(nodeA.request_count.load(), 1);

    // 查询必须是 get_peers 且携带 20 字节原始 info_hash（BEP-005）
    auto queriesA = nodeA.received_queries();
    ASSERT_FALSE(queriesA.empty());
    EXPECT_EQ(queriesA[0].queryType, DhtQueryType::GetPeers);
    DhtNodeId expectedId = DhtUtils::nodeIdFromString(kTestInfoHash);
    std::string expectedRaw(reinterpret_cast<const char*>(expectedId.data()),
                            expectedId.size());
    EXPECT_EQ(queriesA[0].arguments.at("info_hash"), expectedRaw);

    client->stop();
}

// find_node：收敛后按距离序逐个上报已响应节点，且不产生多余查询
TEST_F(DhtClientTest, FindNodeReportsRespondersAndConverges) {
    MockDhtNode nodeA;
    MockDhtNode nodeB;
    ASSERT_TRUE(nodeA.valid());
    ASSERT_TRUE(nodeB.valid());

    DhtNodeId idA = idNearTarget(infoHashId_, 0, 0xFF);
    DhtNodeId idB = idNearTarget(infoHashId_, 19, 0x01);

    std::string nodesAB = encodeCompactNode(idA, "127.0.0.1", nodeA.port()) +
                          encodeCompactNode(idB, "127.0.0.1", nodeB.port());

    nodeA.responder = [idA, nodesAB](const DhtMessage&) {
        return makeResponse(idA, nodesAB, "");
    };
    nodeB.responder = [idB, nodesAB](const DhtMessage&) {
        return makeResponse(idB, nodesAB, "");
    };

    auto client = makeClient(nodeA);

    std::mutex resultMutex;
    std::vector<DhtNode> reported;
    std::promise<void> donePromise;
    auto done = donePromise.get_future();
    DhtNodeId target = infoHashId_;
    client->findNode(target, [&](const DhtNode& node) {
        std::lock_guard<std::mutex> lock(resultMutex);
        reported.push_back(node);
        if (reported.size() == 2) {
            donePromise.set_value();
        }
    });

    // 收敛：两个节点各被查询一次后终结，逐个上报（B 先于 A，按距离序）
    ASSERT_EQ(done.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(nodeA.request_count.load(), 1);
    EXPECT_EQ(nodeB.request_count.load(), 1);

    std::lock_guard<std::mutex> lock(resultMutex);
    ASSERT_EQ(reported.size(), size_t{2});
    EXPECT_EQ(reported[0].id, idB);  // 距离更近的先上报
    EXPECT_EQ(reported[1].id, idA);

    client->stop();
}

// 单节点无 nodes/values：收敛终结，空结果回调
TEST_F(DhtClientTest, FindPeersEmptyNetworkReturnsEmptyResult) {
    MockDhtNode nodeA;
    ASSERT_TRUE(nodeA.valid());
    DhtNodeId idA = idNearTarget(infoHashId_, 0, 0xFF);
    nodeA.responder = [idA](const DhtMessage&) { return makeResponse(idA, "", ""); };

    auto client = makeClient(nodeA);

    std::promise<PeerList> promise;
    auto future = promise.get_future();
    client->findPeers(kTestInfoHash,
                      [&promise](const std::string&, const PeerList& peers) {
                          promise.set_value(peers);
                      });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(future.get().empty());
    EXPECT_EQ(nodeA.request_count.load(), 1);

    client->stop();
}

// 响应者沉默：超时终结查找并回调（不悬挂），耗时远小于默认 30s 超时
TEST_F(DhtClientTest, LookupTimeoutFinalizesWithoutHanging) {
    MockDhtNode nodeA;  // 无 responder：永不响应
    ASSERT_TRUE(nodeA.valid());

    auto client = makeClient(nodeA);
    client->set_lookup_timeout(std::chrono::seconds(1));

    auto start = std::chrono::steady_clock::now();
    std::promise<PeerList> promise;
    auto future = promise.get_future();
    client->findPeers(kTestInfoHash,
                      [&promise](const std::string&, const PeerList& peers) {
                          promise.set_value(peers);
                      });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(8)), std::future_status::ready);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(future.get().empty());
    // 超时生效：远小于默认 30s；超时检查周期为 1s，不设过严下界
    EXPECT_LT(elapsed, std::chrono::seconds(10));

    client->stop();
}

// 并发查找互不干扰：两个 info_hash 同时查找，各自收敛上报
TEST_F(DhtClientTest, ConcurrentLookupsAreIndependent) {
    MockDhtNode nodeA;
    ASSERT_TRUE(nodeA.valid());
    DhtNodeId idA = idNearTarget(infoHashId_, 0, 0xFF);
    nodeA.responder = [idA](const DhtMessage& q) {
        std::string values;
        if (q.arguments.count("info_hash") > 0) {
            values = encodeCompactPeer("10.0.0.9", 6889);
        }
        return makeResponse(idA, "", values);
    };

    auto client = makeClient(nodeA);

    std::promise<PeerList> p1, p2;
    auto f1 = p1.get_future();
    auto f2 = p2.get_future();
    const std::string secondHash(40, 'b');
    client->findPeers(kTestInfoHash, [&p1](const std::string&, const PeerList& peers) {
        p1.set_value(peers);
    });
    client->findPeers(secondHash, [&p2](const std::string&, const PeerList& peers) {
        p2.set_value(peers);
    });

    ASSERT_EQ(f1.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(f2.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(f1.get().size(), size_t{1});
    EXPECT_EQ(f2.get().size(), size_t{1});
    // 两个查找各触发一次查询
    EXPECT_EQ(nodeA.request_count.load(), 2);

    client->stop();
}

// 无效引导端点（端口 0）：查找立即收敛，不悬挂、不发送
TEST_F(DhtClientTest, InvalidBootstrapEndpointTerminatesImmediately) {
    auto client = std::make_unique<DhtClient>(0);
    client->clear_bootstrap_nodes();
    client->addBootstrapNode("127.0.0.1", 0);
    client->start();

    std::promise<PeerList> promise;
    auto future = promise.get_future();
    client->findPeers(kTestInfoHash,
                      [&promise](const std::string&, const PeerList& peers) {
                          promise.set_value(peers);
                      });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(future.get().empty());

    client->stop();
}

// 默认预置的公网域名引导节点（非数字 IP）发送失败：查找快速终结而非
// 悬挂到超时——发送失败按已终结处理
TEST_F(DhtClientTest, UnresolvableBootstrapFailsFastWithoutHanging) {
    auto client = std::make_unique<DhtClient>(0);  // 保留默认公网域名引导节点
    client->start();

    auto start = std::chrono::steady_clock::now();
    std::promise<PeerList> promise;
    auto future = promise.get_future();
    client->findPeers(kTestInfoHash,
                      [&promise](const std::string&, const PeerList& peers) {
                          promise.set_value(peers);
                      });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    // 远小于默认 30s 查找超时
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));

    client->stop();
}
