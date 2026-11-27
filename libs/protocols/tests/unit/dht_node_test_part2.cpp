/**
 * @file dht_node_test_part2.cpp
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

#include <falcon/detail/injection.hpp>

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

#if defined(FALCON_FAILURE_INJECTION)

// socket() 创建失败：start() 静默失败，isRunning() 不撒谎（批次 E 语义）
TEST(DhtNodeTest, StartSurvivesSocketCreationFailure) {
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::DhtSocketCreate);
    DhtClient client(0);
    client.clear_bootstrap_nodes();
    client.start();
    EXPECT_FALSE(client.isRunning());
    client.stop();  // 未运行状态收尾安全
}

#endif  // FALCON_FAILURE_INJECTION
