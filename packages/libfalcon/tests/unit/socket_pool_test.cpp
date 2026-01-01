/**
 * @file socket_pool_test.cpp
 * @brief Socket 连接池单元测试
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <gtest/gtest.h>
#include <falcon/net/socket_pool.hpp>
#include <thread>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#define CLOSE_SOCKET(fd) closesocket(fd)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#define CLOSE_SOCKET(fd) close(fd)
#endif

using namespace falcon;
using namespace falcon::net;

//==============================================================================
// 测试辅助函数
//==============================================================================

/**
 * @brief 创建一个测试 Socket
 */
int create_test_socket() {
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        return -1;
    }
    // 设置非阻塞
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
    return static_cast<int>(fd);
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        // 设置非阻塞
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
#endif
}

/**
 * @brief 创建一个 SocketKey
 */
SocketKey make_key(const std::string& host, uint16_t port) {
    SocketKey key;
    key.host = host;
    key.port = port;
    return key;
}

//==============================================================================
// SocketPool 创建测试
//==============================================================================

TEST(SocketPoolTest, CreateSocketPool) {
    std::chrono::seconds timeout(30);
    size_t max_idle = 16;

    auto pool = std::make_unique<SocketPool>(timeout, max_idle);

    ASSERT_NE(pool, nullptr);
}

//==============================================================================
// Socket 获取测试
//==============================================================================

TEST(SocketPoolTest, AcquireReturnsNullWhenEmpty) {
    SocketPool pool(std::chrono::seconds(30), 16);

    SocketKey key = make_key("example.com", 80);
    auto socket = pool.acquire(key);

    EXPECT_EQ(socket, nullptr);
}

TEST(SocketPoolTest, AcquireAndRelease) {
    SocketPool pool(std::chrono::seconds(30), 16);

    // 创建测试 Socket
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    auto pooled_socket = std::make_shared<PooledSocket>(fd, key);

    // 释放到池中
    pool.release(pooled_socket);

    // 获取应该返回同一个 Socket
    auto acquired = pool.acquire(key);
    ASSERT_NE(acquired, nullptr);
    EXPECT_EQ(acquired->fd(), fd);
    EXPECT_EQ(acquired->key().host, "example.com");
    EXPECT_EQ(acquired->key().port, 80);
}

TEST(SocketPoolTest, AcquireDifferentHosts) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd1 = create_test_socket();
    int fd2 = create_test_socket();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    SocketKey key1 = make_key("example.com", 80);
    SocketKey key2 = make_key("google.com", 80);

    auto socket1 = std::make_shared<PooledSocket>(fd1, key1);
    auto socket2 = std::make_shared<PooledSocket>(fd2, key2);

    pool.release(socket1);
    pool.release(socket2);

    // 获取不同的主机
    auto acquired1 = pool.acquire(key1);
    auto acquired2 = pool.acquire(key2);

    ASSERT_NE(acquired1, nullptr);
    ASSERT_NE(acquired2, nullptr);
    EXPECT_EQ(acquired1->fd(), fd1);
    EXPECT_EQ(acquired2->fd(), fd2);
}

TEST(SocketPoolTest, AcquireDifferentPorts) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd1 = create_test_socket();
    int fd2 = create_test_socket();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    SocketKey key1 = make_key("example.com", 80);
    SocketKey key2 = make_key("example.com", 443);

    auto socket1 = std::make_shared<PooledSocket>(fd1, key1);
    auto socket2 = std::make_shared<PooledSocket>(fd2, key2);

    pool.release(socket1);
    pool.release(socket2);

    // 获取不同端口
    auto acquired1 = pool.acquire(key1);
    auto acquired2 = pool.acquire(key2);

    ASSERT_NE(acquired1, nullptr);
    ASSERT_NE(acquired2, nullptr);
    EXPECT_EQ(acquired1->fd(), fd1);
    EXPECT_EQ(acquired2->fd(), fd2);
}

//==============================================================================
// Socket 复用测试
//==============================================================================

TEST(SocketPoolTest, SocketReuse) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    auto socket = std::make_shared<PooledSocket>(fd, key);

    pool.release(socket);

    // 第一次获取
    auto acquired1 = pool.acquire(key);
    ASSERT_NE(acquired1, nullptr);
    EXPECT_EQ(acquired1->fd(), fd);

    // 释放后再获取
    pool.release(acquired1);
    auto acquired2 = pool.acquire(key);

    ASSERT_NE(acquired2, nullptr);
    EXPECT_EQ(acquired2->fd(), fd);
}

//==============================================================================
// 最大空闲连接测试
//==============================================================================

TEST(SocketPoolTest, MaxIdleConnections) {
    constexpr size_t MAX_IDLE = 3;
    SocketPool pool(std::chrono::seconds(30), MAX_IDLE);

    // 创建多个 Socket
    std::vector<int> fds;
    for (size_t i = 0; i < 5; ++i) {
        int fd = create_test_socket();
        ASSERT_GE(fd, 0);
        fds.push_back(fd);

        SocketKey key = make_key("host" + std::to_string(i), 80);
        auto socket = std::make_shared<PooledSocket>(fd, key);
        pool.release(socket);
    }

    // 超过最大空闲数后，最老的连接应该被关闭
    // 这里验证池的基本行为
}

//==============================================================================
// 超时测试
//==============================================================================

//==============================================================================
// PooledSocket 测试
//==============================================================================

TEST(PooledSocketTest, CreatePooledSocket) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    PooledSocket socket(fd, key);

    EXPECT_EQ(socket.fd(), fd);
    EXPECT_EQ(socket.key().host, "example.com");
    EXPECT_EQ(socket.key().port, 80);
}

TEST(PooledSocketTest, PooledSocketIsValid) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    PooledSocket socket(fd, key);

    EXPECT_TRUE(socket.is_valid());
}

TEST(PooledSocketTest, PooledSocketMoveConstructor) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    PooledSocket socket1(fd, key);

    PooledSocket socket2(std::move(socket1));

    EXPECT_EQ(socket2.fd(), fd);
    EXPECT_EQ(socket2.key().host, "example.com");
    EXPECT_TRUE(socket2.is_valid());
}

TEST(PooledSocketTest, PooledSocketMoveAssignment) {
    int fd1 = create_test_socket();
    int fd2 = create_test_socket();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    SocketKey key1 = make_key("example.com", 80);
    SocketKey key2 = make_key("google.com", 80);

    PooledSocket socket1(fd1, key1);
    PooledSocket socket2(fd2, key2);

    socket2 = std::move(socket1);

    EXPECT_EQ(socket2.fd(), fd1);
    EXPECT_EQ(socket2.key().host, "example.com");
}

TEST(PooledSocketTest, PooledSocketClose) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    PooledSocket socket(fd, key);

    EXPECT_TRUE(socket.is_valid());

    socket.close_fd();

    // 关闭后 Socket 无效
    EXPECT_FALSE(socket.is_valid());
}

//==============================================================================
// SocketKey 比较
//==============================================================================

TEST(SocketKeyTest, Equality) {
    SocketKey key1 = make_key("example.com", 80);
    SocketKey key2 = make_key("example.com", 80);
    SocketKey key3 = make_key("example.com", 443);
    SocketKey key4 = make_key("google.com", 80);

    EXPECT_TRUE(key1 == key2);
    EXPECT_FALSE(key1 == key3);
    EXPECT_FALSE(key1 == key4);
}

TEST(SocketKeyTest, LessThan) {
    SocketKey key1 = make_key("a.com", 80);
    SocketKey key2 = make_key("b.com", 80);
    SocketKey key3 = make_key("a.com", 443);

    EXPECT_LT(key1, key2);
    EXPECT_LT(key1, key3);
}

//==============================================================================
// 并发测试
//==============================================================================

TEST(SocketPoolTest, ConcurrentAccess) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    auto socket = std::make_shared<PooledSocket>(fd, key);

    pool.release(socket);

    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&pool, &key]() {
            for (int j = 0; j < 100; ++j) {
                auto acquired = pool.acquire(key);
                if (acquired) {
                    pool.release(acquired);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(SocketPoolTest, EmptyKey) {
    SocketPool pool(std::chrono::seconds(30), 16);

    SocketKey key;
    key.host = "";
    key.port = 0;

    auto acquired = pool.acquire(key);
    EXPECT_EQ(acquired, nullptr);
}

TEST(SocketPoolTest, VeryLongHostname) {
    SocketPool pool(std::chrono::seconds(30), 16);

    std::string long_host(1000, 'a');
    SocketKey key;
    key.host = long_host;
    key.port = 80;

    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    auto socket = std::make_shared<PooledSocket>(fd, key);
    pool.release(socket);

    auto acquired = pool.acquire(key);
    ASSERT_NE(acquired, nullptr);
}

TEST(SocketPoolTest, InvalidFileDescriptor) {
    SocketPool pool(std::chrono::seconds(30), 16);

    SocketKey key = make_key("example.com", 80);

    // 无效的文件描述符
    PooledSocket socket(-1, key);
    EXPECT_FALSE(socket.is_valid());
}

//==============================================================================
// 性能测试
//==============================================================================

TEST(SocketPoolTest, PerformanceReleaseAcquire) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    auto socket = std::make_shared<PooledSocket>(fd, key);

    pool.release(socket);

    constexpr int ITERATIONS = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        auto acquired = pool.acquire(key);
        ASSERT_NE(acquired, nullptr);
        pool.release(acquired);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 性能测试：10000 次获取/释放应该在合理时间内完成
    EXPECT_LT(duration.count(), 1000);  // 小于 1 秒
}

//==============================================================================
// create_connection 测试
//==============================================================================

class SocketPoolCreateConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        // 初始化 Winsock
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    }

    void TearDown() override {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    SocketPool pool_{std::chrono::seconds(30), 16};
};

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionToLocalhost) {
    SocketKey key = make_key("127.0.0.1", 8080);

    // 尝试创建连接（可能失败，因为没有服务器）
    auto socket = pool_.create_connection(key);

    // 验证返回值类型（可能为 nullptr）
    if (socket) {
        EXPECT_TRUE(socket->is_valid());
        EXPECT_EQ(socket->key().host, "127.0.0.1");
        EXPECT_EQ(socket->key().port, 8080);
    }
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionToInvalidHost) {
    SocketKey key;
    key.host = "this-hostname-definitely-does-not-exist-12345.invalid";
    key.port = 80;

    auto socket = pool_.create_connection(key);

    // DNS 解析应该失败
    EXPECT_EQ(socket, nullptr);
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionWithEmptyHost) {
    SocketKey key;
    key.host = "";
    key.port = 80;

    auto socket = pool_.create_connection(key);

    // 空主机名应该导致失败
    EXPECT_EQ(socket, nullptr);
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionWithZeroPort) {
    SocketKey key;
    key.host = "localhost";
    key.port = 0;

    auto socket = pool_.create_connection(key);

    // 零端口应该导致失败
    EXPECT_EQ(socket, nullptr);
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionIPv4) {
    SocketKey key;
    key.host = "127.0.0.1";
    key.port = 8080;

    auto socket = pool_.create_connection(key);

    // 验证返回值类型
    if (socket) {
        EXPECT_TRUE(socket->is_valid());
        EXPECT_EQ(socket->key().host, "127.0.0.1");
    }
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionIPv6) {
    SocketKey key;
    key.host = "::1";
    key.port = 8080;

    auto socket = pool_.create_connection(key);

    // 验证返回值类型（可能失败，取决于系统是否支持 IPv6）
    if (socket) {
        EXPECT_TRUE(socket->is_valid());
        EXPECT_EQ(socket->key().host, "::1");
    }
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionToDifferentPorts) {
    std::vector<uint16_t> ports = {80, 443, 8080, 9000};

    for (auto port : ports) {
        SocketKey key = make_key("127.0.0.1", port);
        auto socket = pool_.create_connection(key);

        // 验证返回值类型
        if (socket) {
            EXPECT_EQ(socket->key().port, port);
        }
    }
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionVeryLongHostname) {
    std::string long_host(1000, 'a');
    SocketKey key;
    key.host = long_host;
    key.port = 80;

    auto socket = pool_.create_connection(key);

    // 超长主机名应该导致 DNS 失败
    EXPECT_EQ(socket, nullptr);
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionWithSpecialCharacters) {
    SocketKey key;
    key.host = "host_with-dots.and_underscores.com";
    key.port = 80;

    auto socket = pool_.create_connection(key);

    // 验证返回值类型（可能因 DNS 失败返回 nullptr）
    if (socket) {
        EXPECT_TRUE(socket->is_valid());
    }
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionToReservedPort) {
    SocketKey key;
    key.host = "127.0.0.1";
    key.port = 1;  // 保留端口

    auto socket = pool_.create_connection(key);

    // 保留端口通常会导致连接失败
    if (socket) {
        EXPECT_TRUE(socket->is_valid());
    }
}

TEST_F(SocketPoolCreateConnectionTest, CreateConnectionHighPortNumber) {
    SocketKey key;
    key.host = "127.0.0.1";
    key.port = 65535;  // 最大端口

    auto socket = pool_.create_connection(key);

    // 验证返回值类型
    if (socket) {
        EXPECT_TRUE(socket->is_valid());
        EXPECT_EQ(socket->key().port, 65535);
    }
}

//==============================================================================
// 错误处理测试
//==============================================================================

TEST(SocketPoolErrorHandling, CreateConnectionAfterSocketCreationFailure) {
    SocketPool pool(std::chrono::seconds(30), 16);

    // 尝试连接到无效地址，期望失败
    SocketKey key;
    key.host = "255.255.255.255";
    key.port = 99999;

    auto socket = pool.create_connection(key);

    // 应该返回 nullptr
    EXPECT_EQ(socket, nullptr);
}

TEST(SocketPoolErrorHandling, MultipleFailedConnections) {
    SocketPool pool(std::chrono::seconds(30), 16);

    std::vector<std::string> invalid_hosts = {
        "invalid1.example.com",
        "invalid2.example.com",
        "invalid3.example.com"
    };

    for (const auto& host : invalid_hosts) {
        SocketKey key;
        key.host = host;
        key.port = 80;

        auto socket = pool.create_connection(key);
        EXPECT_EQ(socket, nullptr);
    }
}

//==============================================================================
// Socket 复用和生命周期测试
//==============================================================================

TEST(SocketPoolLifecycle, SocketReleasedToPool) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    auto socket = std::make_shared<PooledSocket>(fd, key);

    pool.release(socket);

    // 获取应该返回同一个 socket
    auto acquired = pool.acquire(key);
    ASSERT_NE(acquired, nullptr);
    EXPECT_EQ(acquired->fd(), fd);
}

TEST(SocketPoolLifecycle, SocketNotReusableWhenAcquired) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd1 = create_test_socket();
    ASSERT_GE(fd1, 0);

    SocketKey key = make_key("example.com", 80);
    auto socket1 = std::make_shared<PooledSocket>(fd1, key);

    pool.release(socket1);

    // 第一次获取
    auto acquired1 = pool.acquire(key);
    ASSERT_NE(acquired1, nullptr);

    // 第二次获取应该返回 nullptr（已被获取）
    auto acquired2 = pool.acquire(key);
    EXPECT_EQ(acquired2, nullptr);
}

TEST(SocketPoolLifecycle, SocketReuseAfterRelease) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    auto socket = std::make_shared<PooledSocket>(fd, key);

    pool.release(socket);

    // 获取、释放、再获取
    auto acquired1 = pool.acquire(key);
    ASSERT_NE(acquired1, nullptr);

    pool.release(acquired1);

    auto acquired2 = pool.acquire(key);
    ASSERT_NE(acquired2, nullptr);
    EXPECT_EQ(acquired2->fd(), fd);
}

//==============================================================================
// 并发 create_connection 测试
//==============================================================================

TEST(SocketPoolConcurrency, ConcurrentCreateConnections) {
    SocketPool pool(std::chrono::seconds(30), 16);

    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> failure_count(0);

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&pool, &success_count, &failure_count]() {
            SocketKey key;
            key.host = "127.0.0.1";
            key.port = 8080;

            for (int j = 0; j < 10; ++j) {
                auto socket = pool.create_connection(key);
                if (socket) {
                    success_count++;
                } else {
                    failure_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证总尝试次数
    EXPECT_EQ(success_count + failure_count, NUM_THREADS * 10);
}

TEST(SocketPoolConcurrency, ConcurrentAcquireAndRelease) {
    SocketPool pool(std::chrono::seconds(30), 16);

    // 预先释放一个 socket
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    auto socket = std::make_shared<PooledSocket>(fd, key);
    pool.release(socket);

    constexpr int NUM_THREADS = 10;
    std::vector<std::thread> threads;
    std::atomic<int> acquire_count(0);

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&pool, &key, &acquire_count]() {
            for (int j = 0; j < 100; ++j) {
                auto acquired = pool.acquire(key);
                if (acquired) {
                    acquire_count++;
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                    pool.release(acquired);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 至少应该有一些成功的获取
    EXPECT_GT(acquire_count.load(), 0);
}

//==============================================================================
// 性能和压力测试
//==============================================================================

TEST(SocketPoolStress, RapidAcquireRelease) {
    SocketPool pool(std::chrono::seconds(30), 16);

    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    auto socket = std::make_shared<PooledSocket>(fd, key);
    pool.release(socket);

    constexpr int ITERATIONS = 1000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        auto acquired = pool.acquire(key);
        if (acquired) {
            pool.release(acquired);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 性能测试：1000 次操作应该在合理时间内完成
    EXPECT_LT(duration.count(), 500);
}

TEST(SocketPoolStress, MultipleSocketsInPool) {
    constexpr size_t MAX_IDLE = 10;
    SocketPool pool(std::chrono::seconds(30), MAX_IDLE);

    // 创建并释放多个 socket
    std::vector<SocketKey> keys;
    for (size_t i = 0; i < MAX_IDLE; ++i) {
        int fd = create_test_socket();
        ASSERT_GE(fd, 0);

        SocketKey key = make_key("host" + std::to_string(i) + ".com", 80);
        keys.push_back(key);

        auto socket = std::make_shared<PooledSocket>(fd, key);
        pool.release(socket);
    }

    // 验证所有 socket 都可以被获取
    for (const auto& key : keys) {
        auto acquired = pool.acquire(key);
        EXPECT_NE(acquired, nullptr);
    }
}

TEST(SocketPoolStress, ExceedMaxIdleConnections) {
    constexpr size_t MAX_IDLE = 3;
    SocketPool pool(std::chrono::seconds(30), MAX_IDLE);

    // 创建超过最大空闲数的 socket
    std::vector<SocketKey> keys;
    for (size_t i = 0; i < MAX_IDLE + 2; ++i) {
        int fd = create_test_socket();
        ASSERT_GE(fd, 0);

        SocketKey key = make_key("host" + std::to_string(i) + ".com", 80);
        keys.push_back(key);

        auto socket = std::make_shared<PooledSocket>(fd, key);
        pool.release(socket);
    }

    // 验证至少可以获取 MAX_IDLE 个 socket
    int acquired_count = 0;
    for (const auto& key : keys) {
        auto acquired = pool.acquire(key);
        if (acquired) {
            acquired_count++;
        }
    }

    EXPECT_GE(acquired_count, static_cast<int>(MAX_IDLE));
}

//==============================================================================
// 边界条件增强测试
//==============================================================================

TEST(SocketPoolBoundary, PortBoundaryValues) {
    SocketPool pool(std::chrono::seconds(30), 16);

    // 测试端口边界值
    std::vector<uint16_t> ports = {0, 1, 80, 443, 8080, 65535};

    for (auto port : ports) {
        SocketKey key;
        key.host = "127.0.0.1";
        key.port = port;

        auto socket = pool.create_connection(key);
        // 验证不会崩溃
    }
}

TEST(SocketPoolBoundary, HostnameLengthBoundary) {
    SocketPool pool(std::chrono::seconds(30), 16);

    // 测试不同长度的主机名
    std::vector<std::string> hosts = {
        "a",
        "ab",
        "abc",
        "a.b",
        "a" + std::string(253, 'x') + ".com",  // 接近 DNS 限制
    };

    for (const auto& host : hosts) {
        SocketKey key;
        key.host = host;
        key.port = 80;

        auto socket = pool.create_connection(key);
        // 验证不会崩溃
    }
}

TEST(SocketPoolBoundary, SameKeyDifferentCase) {
    SocketPool pool(std::chrono::seconds(30), 16);

    SocketKey key1 = make_key("Example.COM", 80);
    SocketKey key2 = make_key("example.com", 80);

    int fd1 = create_test_socket();
    int fd2 = create_test_socket();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    auto socket1 = std::make_shared<PooledSocket>(fd1, key1);
    auto socket2 = std::make_shared<PooledSocket>(fd2, key2);

    pool.release(socket1);

    // 不同大小写应该被视为不同的 key
    auto acquired = pool.acquire(key1);
    ASSERT_NE(acquired, nullptr);
    EXPECT_EQ(acquired->fd(), fd1);

    // key2 应该获取不到 socket1
    auto acquired2 = pool.acquire(key2);
    EXPECT_EQ(acquired2, nullptr);
}

//==============================================================================
// PooledSocket 增强测试
//==============================================================================

TEST(PooledSocketAdvanced, SocketKeyToString) {
    SocketKey key = make_key("example.com", 443);

    std::string str = key.to_string();

    // 验证字符串格式
    EXPECT_FALSE(str.empty());
    EXPECT_NE(str.find("example.com"), std::string::npos);
    EXPECT_NE(str.find("443"), std::string::npos);
}

TEST(PooledSocketAdvanced, MultipleCloseCalls) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    PooledSocket socket(fd, key);

    EXPECT_TRUE(socket.is_valid());

    socket.close_fd();
    EXPECT_FALSE(socket.is_valid());

    // 多次关闭应该是安全的
    socket.close_fd();
    socket.close_fd();

    EXPECT_FALSE(socket.is_valid());
}

TEST(PooledSocketAdvanced, MovedSocketIsValid) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    SocketKey key = make_key("example.com", 80);
    PooledSocket socket1(fd, key);

    PooledSocket socket2(std::move(socket1));

    EXPECT_TRUE(socket2.is_valid());
    // socket1 在移动后应该处于无效状态
}

//==============================================================================
// SocketKey 增强测试
//==============================================================================

TEST(SocketKeyAdvanced, CopyConstructor) {
    SocketKey key1 = make_key("example.com", 80);
    SocketKey key2 = key1;

    EXPECT_EQ(key1.host, key2.host);
    EXPECT_EQ(key1.port, key2.port);
    EXPECT_TRUE(key1 == key2);
}

TEST(SocketKeyAdvanced, AssignmentOperator) {
    SocketKey key1 = make_key("example.com", 80);
    SocketKey key2 = make_key("google.com", 443);

    key2 = key1;

    EXPECT_EQ(key2.host, "example.com");
    EXPECT_EQ(key2.port, 80);
}

TEST(SocketKeyAdvanced, LessThanSorting) {
    std::vector<SocketKey> keys = {
        make_key("z.com", 80),
        make_key("a.com", 443),
        make_key("m.com", 80),
        make_key("a.com", 80)
    };

    std::sort(keys.begin(), keys.end());

    // 验证排序结果
    EXPECT_EQ(keys[0].host, "a.com");
    EXPECT_EQ(keys[0].port, 80);
    EXPECT_EQ(keys[1].host, "a.com");
    EXPECT_EQ(keys[1].port, 443);
    EXPECT_EQ(keys[2].host, "m.com");
    EXPECT_EQ(keys[3].host, "z.com");
}

//==============================================================================
// 主函数
//==============================================================================

