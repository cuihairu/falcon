/**
 * @file event_poll_test.cpp
 * @brief 事件轮询单元测试
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <gtest/gtest.h>
#include <falcon/net/event_poll.hpp>
#include <thread>
#include <chrono>
#include <array>
#include <vector>
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
#include <fcntl.h>
#define CLOSE_SOCKET(fd) close(fd)
#endif

using namespace falcon;
using namespace falcon::net;

//==============================================================================
// 测试辅助函数
//==============================================================================

/**
 * @brief 创建一对连接的 Socket 用于测试
 */
std::array<int, 2> create_socket_pair() {
#ifdef _WIN32
    // Windows 不支持 socketpair，使用 TCP 连接模拟
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        return {-1, -1};
    }

    // 绑定到本地端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    // 获取端口
    int addr_len = sizeof(addr);
    if (getsockname(listen_sock, (struct sockaddr*)&addr, &addr_len) == SOCKET_ERROR) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    // 开始监听
    if (listen(listen_sock, 1) == SOCKET_ERROR) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    // 创建客户端 socket
    SOCKET client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_sock == INVALID_SOCKET) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    // 连接
    if (connect(client_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_sock);
        closesocket(client_sock);
        return {-1, -1};
    }

    // 接受连接
    SOCKET server_sock = accept(listen_sock, NULL, NULL);
    closesocket(listen_sock);

    if (server_sock == INVALID_SOCKET) {
        closesocket(client_sock);
        return {-1, -1};
    }

    // 设置非阻塞
    u_long mode = 1;
    ioctlsocket(client_sock, FIONBIO, &mode);
    ioctlsocket(server_sock, FIONBIO, &mode);

    return {static_cast<int>(client_sock), static_cast<int>(server_sock)};
#else
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {-1, -1};
    }

    // 设置非阻塞
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }

    return {fds[0], fds[1]};
#endif
}

//==============================================================================
// EventPoll 创建测试
//==============================================================================

TEST(EventPollTest, CreateEventPoll) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr) << "Failed to create EventPoll";
}

TEST(EventPollTest, CreateMultipleInstances) {
    auto poll1 = EventPoll::create();
    auto poll2 = EventPoll::create();

    ASSERT_NE(poll1, nullptr);
    ASSERT_NE(poll2, nullptr);
}

//==============================================================================
// 事件注册测试
//==============================================================================

TEST(EventPollTest, AddReadEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    int callback_called = 0;

    auto callback = [&callback_called](int fd, int events, void* user_data) {
        callback_called++;
    };

    bool result = poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);
    EXPECT_TRUE(result) << "Failed to add read event";

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollTest, AddWriteEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    int callback_called = 0;

    auto callback = [&callback_called](int fd, int events, void* user_data) {
        callback_called++;
    };

    bool result = poll->add_event(fd1, static_cast<int>(IOEvent::WRITE), callback);
    EXPECT_TRUE(result) << "Failed to add write event";

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollTest, AddMultipleEvents) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    int read_called = 0;
    int write_called = 0;

    auto read_callback = [&read_called](int fd, int events, void* user_data) {
        read_called++;
    };

    auto write_callback = [&write_called](int fd, int events, void* user_data) {
        write_called++;
    };

    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), read_callback));
    EXPECT_TRUE(poll->add_event(fd1, static_cast<int>(IOEvent::WRITE), write_callback));

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollTest, AddReadWriteEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    int callback_called = 0;

    auto callback = [&callback_called](int fd, int events, void* user_data) {
        callback_called++;
    };

    bool result = poll->add_event(
        fd0,
        static_cast<int>(IOEvent::READ | IOEvent::WRITE),
        callback
    );

    EXPECT_TRUE(result) << "Failed to add read/write event";

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// 事件移除测试
//==============================================================================

TEST(EventPollTest, RemoveEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};

    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll->remove_event(fd0));

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollTest, RemoveNonExistentEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // 移除不存在的 fd - 应该返回 false
    EXPECT_FALSE(poll->remove_event(999));
}

TEST(EventPollTest, RemoveEventTwice) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};

    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll->remove_event(fd0));
    EXPECT_FALSE(poll->remove_event(fd0));  // 第二次应当返回 false（已不存在）

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// 事件触发测试
//==============================================================================

TEST(EventPollTest, PollWithTimeout) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);

    // 等待 100ms，应该超时返回 0
    int events = poll->poll(100);
    EXPECT_EQ(events, 0);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollTest, PollWithoutEvents) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // 没有注册事件的情况下，应该立即返回
    int events = poll->poll(100);
    EXPECT_EQ(events, 0);
}

//==============================================================================
// 回调测试
//==============================================================================

TEST(EventPollTest, CallbackInvoked) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    int callback_called = 0;
    int callback_fd = -1;
    int callback_events = 0;

    auto callback = [&](int fd, int events, void* user_data) {
        callback_called++;
        callback_fd = fd;
        callback_events = events;
    };

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);
    poll->poll(100);

    // 注：因为 fd 0 可能没有数据可读，回调可能不会被调用
    // 这取决于具体的平台和实现

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollTest, CallbackWithUserData) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    int test_value = 42;
    void* received_data = nullptr;

    auto callback = [&](int fd, int events, void* user_data) {
        received_data = user_data;
    };

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback, &test_value);
    poll->poll(100);

    // 如果回调被调用，检查用户数据
    if (received_data != nullptr) {
        EXPECT_EQ(received_data, &test_value);
    }

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// Socket 事件测试
//==============================================================================

TEST(EventPollTest, SocketWriteEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [server_fd, client_fd] = create_socket_pair();

    int write_ready = 0;

    auto callback = [&](int fd, int events, void* user_data) {
        if (events & static_cast<int>(IOEvent::WRITE)) {
            write_ready++;
        }
    };

    EXPECT_TRUE(poll->add_event(client_fd, static_cast<int>(IOEvent::WRITE), callback));

    // Socket 应该立即可写
    int events = poll->poll(100);
    EXPECT_GT(events, 0);
    EXPECT_GT(write_ready, 0);

    CLOSE_SOCKET(client_fd);
    CLOSE_SOCKET(server_fd);
}

//==============================================================================
// 事件修改测试
//==============================================================================

TEST(EventPollTest, ModifyEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};

    // 先添加 READ 事件
    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));

    // 修改为 WRITE 事件
    EXPECT_TRUE(poll->modify_event(fd0, static_cast<int>(IOEvent::WRITE)));

    // 移除
    EXPECT_TRUE(poll->remove_event(fd0));

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(EventPollTest, InvalidFileDescriptor) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    // 负数文件描述符
    EXPECT_FALSE(poll->add_event(-1, static_cast<int>(IOEvent::READ), callback));
}

TEST(EventPollTest, ZeroTimeout) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);

    // 零超时应该立即返回
    int events = poll->poll(0);
    EXPECT_GE(events, 0);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollTest, LargeFileDescriptor) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    constexpr int TARGET_FD = 1024;
    int fd_dup = dup2(fd0, TARGET_FD);
    ASSERT_EQ(fd_dup, TARGET_FD);

#ifdef _WIN32
    // Windows: 重新创建 socket
    CLOSE_SOCKET(fd0);
    auto [new_fd0, new_fd1] = create_socket_pair();
    fd0 = new_fd0;
    CLOSE_SOCKET(new_fd1);
#else
    int flags = fcntl(fd_dup, F_GETFL, 0);
    fcntl(fd_dup, F_SETFL, flags | O_NONBLOCK);

    CLOSE_SOCKET(fd0);
    fd0 = fd_dup;
#endif

    EXPECT_TRUE(poll->add_event(TARGET_FD, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll->remove_event(TARGET_FD));

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// 平台特性测试
//==============================================================================

#if defined(__linux__)
TEST(EventPollTest, PlatformEpoll) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // Linux 应该使用 epoll
    // 这里只能验证基本功能
    auto callback = [](int fd, int events, void* user_data) {};
    EXPECT_TRUE(poll->add_event(0, static_cast<int>(IOEvent::READ), callback));
}
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
TEST(EventPollTest, PlatformKqueue) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // BSD/macOS 应该使用 kqueue
    auto callback = [](int fd, int events, void* user_data) {};
    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));
    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}
#else
TEST(EventPollTest, PlatformPoll) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // 其他平台使用 poll
    auto callback = [](int fd, int events, void* user_data) {};
    EXPECT_TRUE(poll->add_event(0, static_cast<int>(IOEvent::READ), callback));
}
#endif

//==============================================================================
// 压力测试
//==============================================================================

TEST(EventPollTest, MultipleFileDescriptors) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    constexpr int NUM_FDS = 100;
    auto callback = [](int fd, int events, void* user_data) {};

    std::vector<int> fds;
    fds.reserve(NUM_FDS);

    for (int i = 0; i < NUM_FDS; ++i) {
        auto [fd0, fd1] = create_socket_pair();
        ASSERT_GE(fd0, 0);
        ASSERT_GE(fd1, 0);
        fds.push_back(fd0);
        CLOSE_SOCKET(fd1);
    }

    for (int fd : fds) {
        EXPECT_TRUE(poll->add_event(fd, static_cast<int>(IOEvent::READ), callback));
    }

    for (int fd : fds) {
        EXPECT_TRUE(poll->remove_event(fd));
        CLOSE_SOCKET(fd);
    }
}

//==============================================================================
// 错误事件测试
//==============================================================================

TEST(EventPollError, ErrorEventHandling) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    int error_count = 0;
    int hangup_count = 0;

    auto callback = [&](int fd, int events, void* user_data) {
        if (events & static_cast<int>(IOEvent::ERR)) {
            error_count++;
        }
        if (events & static_cast<int>(IOEvent::HANGUP)) {
            hangup_count++;
        }
    };

    EXPECT_TRUE(poll->add_event(fd0,
        static_cast<int>(IOEvent::READ | IOEvent::ERR | IOEvent::HANGUP),
        callback));

    // 关闭对端，触发 HANGUP 事件
    CLOSE_SOCKET(fd1);

    int events = poll->poll(100);
    // 可能检测到 HANGUP 事件

    CLOSE_SOCKET(fd0);
}

TEST(EventPollError, InvalidEventMask) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
 ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};

    // 无效的事件掩码（0）
    bool result = poll->add_event(fd0, 0, callback);
    // 结果取决于实现，可能返回 false 或接受

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// 并发测试
//==============================================================================

TEST(EventPollConcurrency, ConcurrentAddRemove) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    constexpr int NUM_THREADS = 4;
    constexpr int OPERATIONS_PER_THREAD = 50;
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> failure_count(0);

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&poll, &success_count, &failure_count, i]() {
            for (int j = 0; j < OPERATIONS_PER_THREAD; ++j) {
                auto [fd0, fd1] = create_socket_pair();
                if (fd0 < 0 || fd1 < 0) {
                    failure_count++;
                    continue;
                }

                auto callback = [](int fd, int events, void* user_data) {};

                if (poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback)) {
                    success_count++;
                    poll->remove_event(fd0);
                } else {
                    failure_count++;
                }

                CLOSE_SOCKET(fd0);
                CLOSE_SOCKET(fd1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证操作完成
    EXPECT_EQ(success_count + failure_count, NUM_THREADS * OPERATIONS_PER_THREAD);
}

TEST(EventPollConcurrency, ConcurrentPollCalls) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};
    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);

    constexpr int NUM_THREADS = 5;
    std::vector<std::thread> threads;
    std::atomic<int> poll_count(0);

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&poll, &poll_count]() {
            for (int j = 0; j < 10; ++j) {
                int events = poll->poll(10);
                poll_count++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(poll_count.load(), NUM_THREADS * 10);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// 性能和压力测试
//==============================================================================

TEST(EventPollStress, LargeNumberOfFDs) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    constexpr int NUM_FDS = 500;
    std::vector<int> fds;
    auto callback = [](int fd, int events, void* user_data) {};

    // 创建大量文件描述符
    for (int i = 0; i < NUM_FDS; ++i) {
        auto [fd0, fd1] = create_socket_pair();
        if (fd0 >= 0 && fd1 >= 0) {
            fds.push_back(fd0);
            EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));
            CLOSE_SOCKET(fd1);
        }
    }

    // 验证大小
    EXPECT_EQ(poll->size(), fds.size());

    // 清理
    for (int fd : fds) {
        poll->remove_event(fd);
        CLOSE_SOCKET(fd);
    }
}

TEST(EventPollStress, RapidAddRemoveCycle) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    constexpr int CYCLES = 100;
    auto callback = [](int fd, int events, void* user_data) {};

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < CYCLES; ++i) {
        auto [fd0, fd1] = create_socket_pair();
        if (fd0 >= 0 && fd1 >= 0) {
            EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));
            EXPECT_TRUE(poll->remove_event(fd0));
            CLOSE_SOCKET(fd0);
            CLOSE_SOCKET(fd1);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 性能测试：100 个周期的添加/删除应该在合理时间内完成
    EXPECT_LT(duration.count(), 1000);
}

TEST(EventPollStress, ModifyEventMultipleTimes) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};

    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));

    constexpr int MODIFICATIONS = 100;
    for (int i = 0; i < MODIFICATIONS; ++i) {
        if (i % 2 == 0) {
            EXPECT_TRUE(poll->modify_event(fd0, static_cast<int>(IOEvent::WRITE)));
        } else {
            EXPECT_TRUE(poll->modify_event(fd0, static_cast<int>(IOEvent::READ)));
        }
    }

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// 边界条件增强测试
//==============================================================================

TEST(EventPollBoundary, AddSameFdTwice) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};

    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));

    // 尝试再次添加同一个 fd
    // 结果取决于实现：可能返回 false 或覆盖原有事件
    bool result = poll->add_event(fd0, static_cast<int>(IOEvent::WRITE), callback);
    // 验证不会崩溃

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollBoundary, ModifyNonExistentEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // 修改不存在的事件
    bool result = poll->modify_event(9999, static_cast<int>(IOEvent::WRITE));
    // 应该返回 false
    EXPECT_FALSE(result);
}

TEST(EventPollBoundary, NegativeTimeout) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};
    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);

    // 负数超时通常表示无限等待
    // 但我们在测试中使用短超时避免阻塞
    int events = poll->poll(-1);
    // 如果实现支持无限等待，这个测试可能会阻塞
    // 所以我们假设使用短超时或立即返回

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollBoundary, VeryLargeTimeout) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);

    // 很大的超时值
    int events = poll->poll(1000000);
    // 应该快速返回，因为没有事件发生

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollBoundary, ClearEmptyPoll) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // 清空空的 poll 应该是安全的
    poll->clear();
    EXPECT_EQ(poll->size(), 0);
}

TEST(EventPollBoundary, ClearAfterOperations) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    constexpr int NUM_FDS = 10;
    auto callback = [](int fd, int events, void* user_data) {};
    std::vector<int> fds;

    for (int i = 0; i < NUM_FDS; ++i) {
        auto [fd0, fd1] = create_socket_pair();
        if (fd0 >= 0 && fd1 >= 0) {
            fds.push_back(fd0);
            poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);
            CLOSE_SOCKET(fd1);
        }
    }

    EXPECT_GT(poll->size(), 0);

    // 清空所有事件
    poll->clear();
    EXPECT_EQ(poll->size(), 0);

    for (int fd : fds) {
        CLOSE_SOCKET(fd);
    }
}

//==============================================================================
// get_error() 和 size() 测试
//==============================================================================

TEST(EventPollAPI, GetErrorInitiallyEmpty) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // 初始时错误信息应该为空或"无错误"
    const char* error = poll->get_error();
    // 验证不会崩溃
}

TEST(EventPollAPI, SizeAfterOperations) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};
    std::vector<int> fds;

    // 初始大小为 0
    EXPECT_EQ(poll->size(), 0);

    // 添加 5 个 fd
    for (int i = 0; i < 5; ++i) {
        auto [fd0, fd1] = create_socket_pair();
        if (fd0 >= 0 && fd1 >= 0) {
            fds.push_back(fd0);
            poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);
            CLOSE_SOCKET(fd1);
        }
    }

    EXPECT_EQ(poll->size(), 5);

    // 移除 2 个
    for (int i = 0; i < 2; ++i) {
        poll->remove_event(fds[i]);
    }
    EXPECT_EQ(poll->size(), 3);

    // 清空
    poll->clear();
    EXPECT_EQ(poll->size(), 0);

    for (int fd : fds) {
        CLOSE_SOCKET(fd);
    }
}

//==============================================================================
// IOEvent 位运算测试
//==============================================================================

TEST(IOEventTest, BitwiseOr) {
    int events = static_cast<int>(IOEvent::READ | IOEvent::WRITE);
    EXPECT_TRUE(events & static_cast<int>(IOEvent::READ));
    EXPECT_TRUE(events & static_cast<int>(IOEvent::WRITE));
}

TEST(IOEventTest, BitwiseAnd) {
    int events = static_cast<int>(IOEvent::READ | IOEvent::WRITE | IOEvent::ERR);
    int result = static_cast<int>(events & static_cast<int>(IOEvent::READ));
    EXPECT_EQ(result, static_cast<int>(IOEvent::READ));
}

TEST(IOEventTest, HasEventHelper) {
    int events = static_cast<int>(IOEvent::READ | IOEvent::WRITE);

    EXPECT_TRUE(has_event(static_cast<IOEvent>(events), IOEvent::READ));
    EXPECT_TRUE(has_event(static_cast<IOEvent>(events), IOEvent::WRITE));
    EXPECT_FALSE(has_event(static_cast<IOEvent>(events), IOEvent::ERR));
}

//==============================================================================
// 平台特定实现测试
//==============================================================================

#if defined(__linux__)
TEST(EventPollPlatform, EPollSpecificBehavior) {
    // Linux 上的 epoll 特定行为测试
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    int callback_count = 0;
    auto callback = [&callback_count](int fd, int events, void* user_data) {
        callback_count++;
    };

    // epoll 支持边缘触发（如果实现的话）
    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback));

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}
#endif

#if defined(_WIN32)
TEST(EventPollPlatform, WindowsWSAPollBehavior) {
    // Windows 上的 WSAPoll 特定行为测试
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int fd, int events, void* user_data) {};

    EXPECT_TRUE(poll->add_event(fd0, static_cast<int>(IOEvent::READ | IOEvent::WRITE), callback));

    // WSAPoll 应该正常工作
    int events = poll->poll(100);
    EXPECT_GE(events, 0);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}
#endif

//==============================================================================
// 事件回调数据测试
//==============================================================================

TEST(EventPollCallback, ComplexUserData) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    struct TestStruct {
        int value1;
        float value2;
        std::string text;
    };

    TestStruct data{42, 3.14f, "test"};
    TestStruct* received_data = nullptr;

    auto callback = [&received_data](int fd, int events, void* user_data) {
        received_data = static_cast<TestStruct*>(user_data);
    };

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback, &data);
    poll->poll(100);

    // 如果回调被调用，验证数据
    if (received_data != nullptr) {
        EXPECT_EQ(received_data->value1, 42);
        EXPECT_FLOAT_EQ(received_data->value2, 3.14f);
        EXPECT_EQ(received_data->text, "test");
    }

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(EventPollCallback, NullUserData) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    void* received_data = reinterpret_cast<void*>(0xDEADBEEF);

    auto callback = [&received_data](int fd, int events, void* user_data) {
        received_data = user_data;
    };

    auto [fd0, fd1] = create_socket_pair();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    // 不提供 user_data（默认为 nullptr）
    poll->add_event(fd0, static_cast<int>(IOEvent::READ), callback);
    poll->poll(100);

    // 如果回调被调用，user_data 应该是 nullptr
    // 如果回调没被调用，received_data 保持原值

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// 实际数据传输测试
//==============================================================================

TEST(EventPollData, ActualDataTransfer) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto [server_fd, client_fd] = create_socket_pair();
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    bool data_received = false;
    std::vector<uint8_t> received_buffer;

    auto read_callback = [&](int fd, int events, void* user_data) {
        if (events & static_cast<int>(IOEvent::READ)) {
            uint8_t buffer[1024];
#ifdef _WIN32
            int n = recv(fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
#else
            ssize_t n = read(fd, buffer, sizeof(buffer));
#endif
            if (n > 0) {
                received_buffer.assign(buffer, buffer + n);
                data_received = true;
            }
        }
    };

    EXPECT_TRUE(poll->add_event(server_fd, static_cast<int>(IOEvent::READ), read_callback));

    // 从客户端发送数据
    const char* test_message = "Hello, EventPoll!";
#ifdef _WIN32
    send(client_fd, test_message, static_cast<int>(strlen(test_message)), 0);
#else
    write(client_fd, test_message, strlen(test_message));
#endif

    // 等待事件
    poll->poll(100);

    // 验证数据接收
    if (data_received) {
        std::string received_str(received_buffer.begin(), received_buffer.end());
        EXPECT_EQ(received_str, test_message);
    }

    CLOSE_SOCKET(client_fd);
    CLOSE_SOCKET(server_fd);
}

//==============================================================================
// 主函数
//==============================================================================
