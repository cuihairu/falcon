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
// 主函数
//==============================================================================

