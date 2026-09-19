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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <set>
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

    // 最近一个数据报发送者的端口（供测试反查 DhtClient 的随机端口）
    uint16_t last_sender_port() const {
        std::lock_guard<std::mutex> lock(receivedMutex_);
        return last_sender_port_;
    }

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
                last_sender_port_ = ntohs(sender.sin_port);
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
    uint16_t last_sender_port_ = 0;
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

//==============================================================================
// DhtUtils / DhtNode 边界
//==============================================================================

// 大写 hex 同样解码（hexVal 的大写分支）
TEST(DhtUtilsTest, NodeIdFromStringUppercaseHex) {
    DhtNodeId id{};
    for (size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    std::string lower = DhtUtils::nodeIdToString(id);
    std::string upper = lower;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    EXPECT_EQ(DhtUtils::nodeIdFromString(upper), id);
}

// 非 hex 形态回退：40 长度含非 hex 字符、非 40 长度（短/超长）一律按
// 原始字节截断/补零
TEST(DhtUtilsTest, NodeIdFromStringNonHexFallsBackToRawBytes) {
    std::string raw(40, 'z');  // 'z' 非 hex → hex 解码整体失败 → 字节回退
    DhtNodeId id = DhtUtils::nodeIdFromString(raw);
    EXPECT_EQ(id[0], static_cast<uint8_t>('z'));
    EXPECT_EQ(id[19], static_cast<uint8_t>('z'));

    // 短于 20 字节：原始字节 + 尾部补零
    DhtNodeId shortId = DhtUtils::nodeIdFromString("abc");
    EXPECT_EQ(shortId[0], static_cast<uint8_t>('a'));
    EXPECT_EQ(shortId[1], static_cast<uint8_t>('b'));
    EXPECT_EQ(shortId[2], static_cast<uint8_t>('c'));
    EXPECT_EQ(shortId[3], uint8_t{0});

    // 超长输入：截断到 20 字节
    DhtNodeId longId = DhtUtils::nodeIdFromString(std::string(50, 'w'));
    EXPECT_EQ(longId[0], static_cast<uint8_t>('w'));
    EXPECT_EQ(longId[19], static_cast<uint8_t>('w'));
}

// DhtNode::distanceTo 与 DhtUtils::xorDistance 一致
TEST(DhtNodeTest, DistanceToMatchesXor) {
    DhtNode a;
    a.id = DhtUtils::nodeIdFromString(std::string(40, 'a'));
    DhtNodeId b = DhtUtils::nodeIdFromString(std::string(40, 'b'));
    EXPECT_EQ(a.distanceTo(b), DhtUtils::xorDistance(a.id, b));
}

//==============================================================================
// DhtBucket / DhtRoutingTable
//==============================================================================

namespace {

// 构造桶测试节点：id 全部填充 tag，lastSeen 可回拨
DhtNode makeBucketNode(uint8_t tag, int ageMinutes = 0) {
    DhtNode n;
    n.ip = "10.0.0.1";
    n.port = static_cast<uint16_t>(1000 + tag);
    n.id.fill(tag);
    n.lastSeen = std::chrono::steady_clock::now() - std::chrono::minutes(ageMinutes);
    return n;
}

} // namespace

// 桶满（K=8）时替换 15 分钟不活跃的最旧节点
TEST(DhtBucketTest, FullBucketReplacesStaleNode) {
    DhtBucket bucket;
    bucket.addNode(makeBucketNode(0, 16));  // 最旧：16 分钟未活跃
    for (uint8_t i = 1; i < 8; ++i) {
        EXPECT_TRUE(bucket.addNode(makeBucketNode(i)));
    }
    EXPECT_EQ(bucket.getNodes().size(), size_t{8});

    DhtNode newcomer = makeBucketNode(0x30);
    EXPECT_TRUE(bucket.addNode(newcomer));
    auto nodes = bucket.getNodes();
    EXPECT_EQ(nodes.size(), size_t{8});
    EXPECT_TRUE(std::any_of(nodes.begin(), nodes.end(),
                            [&](const DhtNode& n) { return n.id == newcomer.id; }));
    // 被替换掉的正是 16 分钟前的最旧节点（tag 0）
    EXPECT_FALSE(std::any_of(nodes.begin(), nodes.end(),
                             [](const DhtNode& n) { return n.id[0] == 0; }));
}

// 桶满且全员活跃：新节点进替换缓存，桶内容不变
TEST(DhtBucketTest, FullBucketWithActiveNodesRejectsNewcomer) {
    DhtBucket bucket;
    for (uint8_t i = 0; i < 8; ++i) {
        bucket.addNode(makeBucketNode(i));
    }
    DhtNode newcomer = makeBucketNode(0x40);
    EXPECT_FALSE(bucket.addNode(newcomer));
    auto nodes = bucket.getNodes();
    EXPECT_EQ(nodes.size(), size_t{8});
    EXPECT_FALSE(std::any_of(nodes.begin(), nodes.end(),
                             [&](const DhtNode& n) { return n.id == newcomer.id; }));
}

// removeNode / getNodes / getActiveNodeCount（inactive 节点不计活跃）
TEST(DhtBucketTest, RemoveNodesAndGetActiveCount) {
    DhtBucket bucket;
    for (uint8_t i = 0; i < 5; ++i) {
        bucket.addNode(makeBucketNode(i));
    }
    DhtNode idle = makeBucketNode(9);
    idle.active = false;
    bucket.addNode(idle);
    EXPECT_EQ(bucket.getNodes().size(), size_t{6});
    EXPECT_EQ(bucket.getActiveNodeCount(), size_t{5});

    bucket.removeNode(idle.id);
    EXPECT_EQ(bucket.getNodes().size(), size_t{5});
    bucket.removeNode(makeBucketNode(2).id);
    EXPECT_EQ(bucket.getNodes().size(), size_t{4});
    // 移除不存在的节点：无操作
    bucket.removeNode(makeBucketNode(0x77).id);
    EXPECT_EQ(bucket.getNodes().size(), size_t{4});
}

// findClosestNodes：按 XOR 距离排序并截断到 count
TEST(DhtBucketTest, FindClosestNodesSortsAndTruncates) {
    DhtNodeId target{};
    target[0] = 0x80;
    auto nodeAtDistance = [&](uint8_t mask, uint8_t tag) {
        DhtNode n = makeBucketNode(tag);
        n.id = target;
        n.id[0] = static_cast<uint8_t>(n.id[0] ^ mask);
        return n;
    };

    DhtBucket bucket;
    bucket.addNode(nodeAtDistance(0x80, 1));  // 距离 0x80：最远
    bucket.addNode(nodeAtDistance(0x01, 2));  // 距离 0x01：最近
    bucket.addNode(nodeAtDistance(0x40, 3));  // 距离 0x40：中间

    auto closest = bucket.findClosestNodes(target, 2);
    ASSERT_EQ(closest.size(), size_t{2});
    EXPECT_EQ(closest[0].id[0], uint8_t{0x81});  // 0x80 ^ 0x01
    EXPECT_EQ(closest[1].id[0], uint8_t{0xC0});  // 0x80 ^ 0x40

    auto all = bucket.findClosestNodes(target, 10);  // count 超过节点数：原样返回
    EXPECT_EQ(all.size(), size_t{3});
}

// 路由表跨桶聚合：getAllNodes / getTotalNodeCount / findClosestNodes
TEST(DhtRoutingTableTest, CollectSortCountAcrossBuckets) {
    DhtRoutingTable table;
    DhtNodeId target{};
    target[0] = 0x80;
    auto nodeWithFirstByte = [&](uint8_t firstByte, uint8_t tag) {
        DhtNode n = makeBucketNode(tag);
        n.id.fill(0);
        n.id[0] = firstByte;
        return n;
    };

    // 不同前导零的 id 落入不同桶
    table.addNode(nodeWithFirstByte(0x80, 1));  // 0 个前导零
    table.addNode(nodeWithFirstByte(0x40, 2));  // 1 个前导零
    table.addNode(nodeWithFirstByte(0x01, 3));  // 7 个前导零
    table.addNode(nodeWithFirstByte(0x00, 4));  // 全零 id → 桶上限 159
    EXPECT_EQ(table.getTotalNodeCount(), size_t{4});
    EXPECT_EQ(table.getAllNodes().size(), size_t{4});

    auto closest = table.findClosestNodes(target, 2);
    ASSERT_EQ(closest.size(), size_t{2});
    EXPECT_EQ(closest[0].id[0], uint8_t{0x80});  // 距离 0
    EXPECT_EQ(closest[1].id[0], uint8_t{0x00});  // 距离 0x80（全零 id 节点）

    auto all = table.findClosestNodes(target, 100);
    EXPECT_EQ(all.size(), size_t{4});
}

// 全零 id：前导零计数钳位到桶上限（159），不越界
TEST(DhtRoutingTableTest, AllZeroIdFallsIntoLastBucket) {
    DhtRoutingTable table;
    table.addNode(makeBucketNode(0));
    EXPECT_EQ(table.getTotalNodeCount(), size_t{1});
    EXPECT_EQ(table.getAllNodes().size(), size_t{1});
}

//==============================================================================
// DhtMessage Error 分支与防御
//==============================================================================

// Error 消息 encode/decode 往返（含空错误表回落 "Unknown error"）
TEST(DhtMessageTest, ErrorEncodeDecodeRoundTrip) {
    DhtMessage err;
    err.type = DhtMessageType::Error;
    err.transactionId = "er";
    err.nodeId = DhtUtils::nodeIdFromString(kTestInfoHash);
    err.response["error"] = "method not allowed";

    DhtMessage back = DhtMessage::decode(err.encode());
    EXPECT_EQ(back.type, DhtMessageType::Error);
    EXPECT_EQ(back.transactionId, "er");
    ASSERT_FALSE(back.response.empty());
    EXPECT_EQ(back.response.at("error"), "method not allowed");

    DhtMessage bare;
    bare.type = DhtMessageType::Error;  // response 为空 → "Unknown error"
    DhtMessage bareBack = DhtMessage::decode(bare.encode());
    EXPECT_EQ(bareBack.type, DhtMessageType::Error);
    EXPECT_EQ(bareBack.response.at("error"), "Unknown error");
}

// 非 dict 的合法 bencode：返回默认消息；垃圾输入不抛异常
TEST(DhtMessageTest, DecodeNonDictAndGarbageReturnDefaultMessage) {
    DhtMessage msg = DhtMessage::decode("i42e");
    EXPECT_TRUE(msg.transactionId.empty());
    EXPECT_TRUE(msg.response.empty());

    DhtMessage garbage = DhtMessage::decode("not-bencode-at-all");
    EXPECT_TRUE(garbage.transactionId.empty());
}

//==============================================================================
// DhtClient 查找边界
//==============================================================================

// 未 start 的客户端：socket 未建立，发送必败 → 查找立即终结（不悬挂）
TEST_F(DhtClientTest, NotStartedClientFailsFastOnLookup) {
    auto client = std::make_unique<DhtClient>(0);
    client->clear_bootstrap_nodes();
    client->addBootstrapNode("127.0.0.1", 6881);
    // 有意不调用 start()

    std::promise<PeerList> pp;
    auto pf = pp.get_future();
    client->findPeers(kTestInfoHash,
                      [&pp](const std::string&, const PeerList& peers) {
                          pp.set_value(peers);
                      });
    ASSERT_EQ(pf.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(pf.get().empty());

    // findNode 同样快速终结（活体证据：后续 findPeers 仍能到达回调）
    client->findNode(infoHashId_, [](const DhtNode&) {});
    std::promise<PeerList> pp2;
    auto pf2 = pp2.get_future();
    client->findPeers(kTestInfoHash,
                      [&pp2](const std::string&, const PeerList& peers) {
                          pp2.set_value(peers);
                      });
    ASSERT_EQ(pf2.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    client->stop();
}

// 单轮并发查询受 α=3 上限：第 4 近的候选等前一轮响应后才被查询
TEST_F(DhtClientTest, AlphaCapsConcurrentQueriesPerRound) {
    MockDhtNode nodes[4];
    for (auto& n : nodes) {
        ASSERT_TRUE(n.valid());
    }
    DhtNodeId ids[4];
    std::string allNodesBlob;
    for (int i = 0; i < 4; ++i) {
        ids[i] = idNearTarget(infoHashId_, 19, static_cast<uint8_t>(1 << i));
        allNodesBlob += encodeCompactNode(ids[i], "127.0.0.1", nodes[i].port());
    }
    // 引导节点响应带出全部 4 个节点（含自身端点，已被查询会跳过）
    DhtNodeId id0 = ids[0];
    nodes[0].responder = [id0, allNodesBlob](const DhtMessage&) {
        return makeResponse(id0, allNodesBlob, "");
    };
    for (int i = 1; i < 4; ++i) {
        MockDhtNode& n = nodes[i];
        DhtNodeId id = ids[i];
        n.responder = [id](const DhtMessage&) { return makeResponse(id, "", ""); };
    }

    auto client = std::make_unique<DhtClient>(0);
    client->clear_bootstrap_nodes();
    client->addBootstrapNode("127.0.0.1", nodes[0].port());
    client->start();

    // 第一阶段热身查找：响应把 4 个节点以真实 id 写入路由表——
    // bootstrap 候选的 id 全零，距离排序退化，必须经路由表获得确定的距离序
    std::promise<PeerList> warmPromise;
    auto warmFuture = warmPromise.get_future();
    client->findPeers(kTestInfoHash,
                      [&warmPromise](const std::string&, const PeerList& peers) {
                          warmPromise.set_value(peers);
                      });
    ASSERT_EQ(warmFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    int baseline[4];
    for (int i = 0; i < 4; ++i) {
        baseline[i] = nodes[i].request_count.load();
        ASSERT_EQ(baseline[i], 1);  // 热身阶段每个节点恰好被查询一次
    }

    // 第二阶段：门闩扣住最近 3 个节点的响应，冻结第一轮
    std::atomic<bool> gate{false};
    for (int i = 0; i < 3; ++i) {
        MockDhtNode& n = nodes[i];
        DhtNodeId id = ids[i];
        n.responder = [&gate, id](const DhtMessage&) {
            while (!gate.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return makeResponse(id, "", "");
        };
    }
    DhtNodeId farId = ids[3];
    nodes[3].responder = [farId](const DhtMessage&) {
        return makeResponse(farId, "", "");
    };

    std::promise<PeerList> promise;
    auto future = promise.get_future();
    client->findPeers(kTestInfoHash,
                      [&promise](const std::string&, const PeerList& peers) {
                          promise.set_value(peers);
                      });

    // 第一轮只查询最近 α=3 个候选（来自路由表的确定性距离序）
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((nodes[0].request_count.load() - baseline[0]) +
                   (nodes[1].request_count.load() - baseline[1]) +
                   (nodes[2].request_count.load() - baseline[2]) <
               3 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ((nodes[0].request_count.load() - baseline[0]) +
                  (nodes[1].request_count.load() - baseline[1]) +
                  (nodes[2].request_count.load() - baseline[2]),
              3);
    EXPECT_EQ(nodes[3].request_count.load() - baseline[3], 0);  // 被 α 上限挡住

    gate.store(true);  // 放行 → 响应驱动下一轮 → 第 4 近的候选被查询
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(future.get().empty());
    EXPECT_EQ(nodes[3].request_count.load() - baseline[3], 1);

    client->stop();
}

// 响应携带超量节点：候选吸收在 kMaxCandidates=64 处截断，查找仍正常终结
TEST_F(DhtClientTest, CandidateAbsorptionCappedAtMaxCandidates) {
    MockDhtNode nodeA;
    ASSERT_TRUE(nodeA.valid());
    DhtNodeId idA = idNearTarget(infoHashId_, 0, 0xFF);
    std::string nodesBlob;
    for (int i = 0; i < 70; ++i) {  // 70 > 64：截断分支必然命中
        DhtNodeId nid{};
        nid[19] = static_cast<uint8_t>(i + 1);
        nodesBlob += encodeCompactNode(nid, "10.1.0.1", static_cast<uint16_t>(2000 + i));
    }
    nodeA.responder = [idA, nodesBlob](const DhtMessage&) {
        return makeResponse(idA, nodesBlob, "");
    };

    auto client = makeClient(nodeA);
    client->set_lookup_timeout(std::chrono::seconds(1));

    std::promise<PeerList> promise;
    auto future = promise.get_future();
    client->findPeers(kTestInfoHash,
                      [&promise](const std::string&, const PeerList& peers) {
                          promise.set_value(peers);
                      });

    // 吸收截断后剩余候选不可达 → 超时终结（而非悬挂或崩溃）
    ASSERT_EQ(future.wait_for(std::chrono::seconds(8)), std::future_status::ready);
    EXPECT_TRUE(future.get().empty());

    client->stop();
}

// 收敛上报按距离取前 k=8 个已响应节点（第 9 近的被截断）
TEST_F(DhtClientTest, NodeReportingCappedAtK) {
    constexpr int kNodeCount = 9;  // kLookupK = 8
    MockDhtNode nodes[kNodeCount];
    for (auto& n : nodes) {
        ASSERT_TRUE(n.valid());
    }
    DhtNodeId ids[kNodeCount];
    std::string allNodesBlob;
    for (int i = 0; i < kNodeCount; ++i) {
        ids[i] = idNearTarget(infoHashId_, 19, static_cast<uint8_t>(0x10 + i));
        allNodesBlob += encodeCompactNode(ids[i], "127.0.0.1", nodes[i].port());
    }
    for (int i = 0; i < kNodeCount; ++i) {
        MockDhtNode& n = nodes[i];
        DhtNodeId id = ids[i];
        n.responder = [id](const DhtMessage&) { return makeResponse(id, "", ""); };
    }
    // bootstrap 响应带出全部 9 个节点（自身端点已查询，会被跳过）
    DhtNodeId id0 = ids[0];
    nodes[0].responder = [id0, allNodesBlob](const DhtMessage&) {
        return makeResponse(id0, allNodesBlob, "");
    };

    auto client = std::make_unique<DhtClient>(0);
    client->clear_bootstrap_nodes();
    client->addBootstrapNode("127.0.0.1", nodes[0].port());
    client->start();

    std::mutex resultMutex;
    std::vector<DhtNodeId> reported;
    std::promise<void> donePromise;
    auto done = donePromise.get_future();
    client->findNode(infoHashId_, [&](const DhtNode& node) {
        std::lock_guard<std::mutex> lock(resultMutex);
        reported.push_back(node.id);
        if (reported.size() == 8) {
            donePromise.set_value();
        }
    });

    ASSERT_EQ(done.wait_for(std::chrono::seconds(8)), std::future_status::ready);
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        EXPECT_EQ(reported.size(), size_t{8});
        // 全部去重且为最近的 8 个（最远的 ids[8] 被截断）
        std::set<DhtNodeId> unique(reported.begin(), reported.end());
        EXPECT_EQ(unique.size(), size_t{8});
        EXPECT_EQ(unique.count(ids[8]), size_t{0});
    }

    client->stop();
}

// 两个端点不同但声称同一节点 id 的响应者：只上报一次
TEST_F(DhtClientTest, DuplicateResponderIdsReportedOnce) {
    MockDhtNode nodeA;
    MockDhtNode nodeB1;
    MockDhtNode nodeB2;
    ASSERT_TRUE(nodeA.valid());
    ASSERT_TRUE(nodeB1.valid());
    ASSERT_TRUE(nodeB2.valid());

    DhtNodeId idA = idNearTarget(infoHashId_, 0, 0xFF);
    DhtNodeId sharedId = idNearTarget(infoHashId_, 19, 0x01);
    std::string nodesB = encodeCompactNode(sharedId, "127.0.0.1", nodeB1.port()) +
                         encodeCompactNode(sharedId, "127.0.0.1", nodeB2.port());
    nodeA.responder = [idA, nodesB](const DhtMessage&) {
        return makeResponse(idA, nodesB, "");
    };
    nodeB1.responder = [sharedId](const DhtMessage&) {
        return makeResponse(sharedId, "", "");
    };
    nodeB2.responder = [sharedId](const DhtMessage&) {
        return makeResponse(sharedId, "", "");
    };

    auto client = makeClient(nodeA);

    std::mutex resultMutex;
    std::vector<DhtNodeId> reported;
    auto reportedCount = [&]() {
        std::lock_guard<std::mutex> lock(resultMutex);
        return reported.size();
    };
    client->findNode(infoHashId_, [&](const DhtNode& node) {
        std::lock_guard<std::mutex> lock(resultMutex);
        reported.push_back(node.id);
    });

    // 引导 A、B1、B2 三个响应者都被查询并收敛终结
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((nodeA.request_count.load() < 1 || nodeB1.request_count.load() < 1 ||
            nodeB2.request_count.load() < 1 || reportedCount() < 2) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 终结余量
    EXPECT_EQ(nodeA.request_count.load(), 1);
    EXPECT_EQ(nodeB1.request_count.load(), 1);
    EXPECT_EQ(nodeB2.request_count.load(), 1);

    std::lock_guard<std::mutex> lock(resultMutex);
    ASSERT_EQ(reported.size(), size_t{2});  // idA + sharedId（去重后）
    EXPECT_EQ(std::set<DhtNodeId>(reported.begin(), reported.end()).size(), size_t{2});
    EXPECT_EQ(std::count(reported.begin(), reported.end(), sharedId), 1);  // 只报一次

    client->stop();
}

TEST_F(DhtClientTest, HardRecvErrorFromDeadBootstrapIsIgnored) {
    // 制造一个无监听的 UDP 死端口
    int dead = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(dead, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    ASSERT_EQ(bind(dead, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    getsockname(dead, reinterpret_cast<sockaddr*>(&addr), &len);
    const uint16_t dead_port = ntohs(addr.sin_port);
    closeSocket(dead);

    auto client = std::make_unique<DhtClient>(0);
    client->clear_bootstrap_nodes();
    client->addBootstrapNode("127.0.0.1", dead_port);
    client->start();
    ASSERT_TRUE(client->isRunning());

    // 空路由表 → 查找向死端口候选发查询;随后 recvfrom 读到 ICMP 错误
    client->findPeers(kTestInfoHash, [](const std::string&, const PeerList&) {});

    // 给 receiveLoop 足够轮次经历「硬错误 → 100ms 休眠 → 继续」;
    // 回调不等待(查找超时 30s),stop 的 join 即线程存活铁证
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    client->stop();
    EXPECT_FALSE(client->isRunning());
}
