/**
 * @file event_poll_poll_test.cpp
 * @brief PollEventPoll（poll 后端）与事件辅助函数单元测试
 * @author Falcon Team
 * @date 2026-09-05
 *
 * 直接构造 PollEventPoll 实例（通用 fallback 后端），
 * 使用 socketpair 验证注册/修改/移除/轮询/回调全流程，离线可运行。
 */

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
#include <fcntl.h>
#define CLOSE_SOCKET(fd) close(fd)
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/net/event_poll.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace falcon;
using namespace falcon::net;

namespace {

#ifdef _WIN32
void ensure_winsock_for_poll_test() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#endif

/// 创建一对已连接的非阻塞 Socket
std::array<int, 2> make_socket_pair_nb() {
#ifdef _WIN32
    ensure_winsock_for_poll_test();

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) return {-1, -1};

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listen_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_sock, 1) != 0) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    int len = sizeof(addr);
    if (getsockname(listen_sock, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    SOCKET client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_sock == INVALID_SOCKET) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    if (connect(client_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listen_sock);
        closesocket(client_sock);
        return {-1, -1};
    }

    SOCKET server_sock = accept(listen_sock, NULL, NULL);
    closesocket(listen_sock);
    if (server_sock == INVALID_SOCKET) {
        closesocket(client_sock);
        return {-1, -1};
    }

    u_long mode = 1;
    ioctlsocket(client_sock, FIONBIO, &mode);
    ioctlsocket(server_sock, FIONBIO, &mode);

    return {static_cast<int>(client_sock), static_cast<int>(server_sock)};
#else
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {-1, -1};
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }
    return {fds[0], fds[1]};
#endif
}

} // namespace

//==============================================================================
// 注册 / 修改 / 移除测试
//==============================================================================

TEST(PollEventPollTest, AddEventWithInvalidFdFails) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto callback = [](int, int, void*) {};
    EXPECT_FALSE(poll.add_event(-1, static_cast<int>(IOEvent::READ), callback));
    EXPECT_NE(poll.get_error()[0], '\0');
    EXPECT_EQ(poll.size(), 0U);
}

TEST(PollEventPollTest, AddEventWithZeroMaskFails) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int, int, void*) {};
    EXPECT_FALSE(poll.add_event(fd0, 0, callback));
    EXPECT_NE(poll.get_error()[0], '\0');

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(PollEventPollTest, AddModifyRemoveLifecycle) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int, int, void*) {};
    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::READ), callback));
    EXPECT_EQ(poll.size(), 1U);

    EXPECT_TRUE(poll.modify_event(fd0, static_cast<int>(IOEvent::WRITE)));
    EXPECT_EQ(poll.size(), 1U);

    EXPECT_TRUE(poll.remove_event(fd0));
    EXPECT_EQ(poll.size(), 0U);

    // 移除后可重新注册
    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::READ), callback));
    EXPECT_EQ(poll.size(), 1U);

    EXPECT_TRUE(poll.remove_event(fd0));

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(PollEventPollTest, ModifyNonExistentFdFails) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    EXPECT_FALSE(poll.modify_event(98765, static_cast<int>(IOEvent::WRITE)));
    EXPECT_NE(poll.get_error()[0], '\0');
}

TEST(PollEventPollTest, RemoveNonExistentFdFails) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    EXPECT_FALSE(poll.remove_event(98765));
}

TEST(PollEventPollTest, ExceedsMaxFdsLimit) {
    PollEventPoll poll_impl(/*max_fds=*/1);
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    auto [fd2, fd3] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);
    ASSERT_GE(fd3, 0);

    auto callback = [](int, int, void*) {};
    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::READ), callback));
    // 超过 max_fds=1：第二个 fd 应被拒绝
    EXPECT_FALSE(poll.add_event(fd2, static_cast<int>(IOEvent::READ), callback));
    EXPECT_NE(poll.get_error()[0], '\0');

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
    CLOSE_SOCKET(fd2);
    CLOSE_SOCKET(fd3);
}

//==============================================================================
// poll 轮询测试
//==============================================================================

TEST(PollEventPollTest, PollWithNoRegistrationsReturnsZero) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    EXPECT_EQ(poll.poll(10), 0);
}

TEST(PollEventPollTest, PollTimeoutWithNoReadyFd) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    auto callback = [](int, int, void*) {};
    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::READ), callback));

    // 对端不写数据：注册 READ 事件应超时返回 0
    EXPECT_EQ(poll.poll(30), 0);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(PollEventPollTest, PollWriteReadyInvokesCallback) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    std::atomic<int> callback_count{0};
    std::atomic<int> received_events{0};
    std::atomic<int> received_fd{-1};

    auto callback = [&](int fd, int events, void*) {
        received_fd = fd;
        received_events = events;
        callback_count++;
    };

    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::WRITE), callback));

    // socketpair 空闲时始终可写
    EXPECT_GE(poll.poll(100), 1);
    EXPECT_GE(callback_count.load(), 1);
    EXPECT_EQ(received_fd.load(), fd0);
    EXPECT_TRUE(received_events.load() & static_cast<int>(IOEvent::WRITE));

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(PollEventPollTest, PollReadReadyInvokesCallback) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    std::atomic<int> callback_count{0};
    std::atomic<int> received_events{0};

    auto callback = [&](int, int events, void*) {
        received_events = events;
        callback_count++;
    };

    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::READ), callback));

    const char message[] = "poll-me";
#ifdef _WIN32
    ASSERT_GT(send(fd1, message, static_cast<int>(sizeof(message)), 0), 0);
#else
    ASSERT_GT(write(fd1, message, sizeof(message)), 0);
#endif

    EXPECT_GE(poll.poll(100), 1);
    EXPECT_GE(callback_count.load(), 1);
    EXPECT_TRUE(received_events.load() & static_cast<int>(IOEvent::READ));

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(PollEventPollTest, PollDetectsPeerHangupAsError) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    std::atomic<int> received_events{0};
    std::atomic<int> callback_count{0};

    auto callback = [&](int, int events, void*) {
        received_events = events;
        callback_count++;
    };

    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::READ), callback));

    // 关闭对端触发 HUP 事件
    CLOSE_SOCKET(fd1);

    EXPECT_GE(poll.poll(200), 1);
    EXPECT_GE(callback_count.load(), 1);
    EXPECT_TRUE(received_events.load() & static_cast<int>(IOEvent::ERR));

    CLOSE_SOCKET(fd0);
}

TEST(PollEventPollTest, PollDeliversUserDataToCallback) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    int user_value = 4242;
    void* received = nullptr;

    auto callback = [&](int, int, void* user_data) {
        received = user_data;
    };

    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::WRITE), callback,
                               &user_value));

    EXPECT_GE(poll.poll(100), 1);
    EXPECT_EQ(received, &user_value);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

TEST(PollEventPollTest, PollMultipleReadyFds) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    auto [fd2, fd3] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);
    ASSERT_GE(fd3, 0);

    std::atomic<int> callback_count{0};
    auto callback = [&](int, int, void*) { callback_count++; };

    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::WRITE), callback));
    EXPECT_TRUE(poll.add_event(fd2, static_cast<int>(IOEvent::WRITE), callback));
    EXPECT_EQ(poll.size(), 2U);

    // 两个 fd 均可写，单次 poll 应报告 2 个就绪事件
    EXPECT_GE(poll.poll(100), 2);
    EXPECT_GE(callback_count.load(), 2);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
    CLOSE_SOCKET(fd2);
    CLOSE_SOCKET(fd3);
}

TEST(PollEventPollTest, RemovedFdNoLongerPolled) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);

    std::atomic<int> callback_count{0};
    auto callback = [&](int, int, void*) { callback_count++; };

    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::WRITE), callback));
    EXPECT_TRUE(poll.remove_event(fd0));

    // 移除后不再监听 → 超时返回 0
    EXPECT_EQ(poll.poll(20), 0);
    EXPECT_EQ(callback_count.load(), 0);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
}

//==============================================================================
// clear / size / 并发测试
//==============================================================================

TEST(PollEventPollTest, ClearRemovesAllEvents) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [fd0, fd1] = make_socket_pair_nb();
    auto [fd2, fd3] = make_socket_pair_nb();
    ASSERT_GE(fd0, 0);
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);
    ASSERT_GE(fd3, 0);

    auto callback = [](int, int, void*) {};
    EXPECT_TRUE(poll.add_event(fd0, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll.add_event(fd2, static_cast<int>(IOEvent::READ), callback));
    EXPECT_EQ(poll.size(), 2U);

    poll.clear();
    EXPECT_EQ(poll.size(), 0U);
    EXPECT_EQ(poll.poll(10), 0);

    CLOSE_SOCKET(fd0);
    CLOSE_SOCKET(fd1);
    CLOSE_SOCKET(fd2);
    CLOSE_SOCKET(fd3);
}

TEST(PollEventPollTest, ConcurrentAddRemoveIsSafe) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    constexpr int kThreads = 3;
    constexpr int kCycles = 20;

    std::vector<std::thread> threads;
    std::atomic<int> add_ok{0};
    std::atomic<int> add_fail{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&poll, &add_ok, &add_fail]() {
            for (int i = 0; i < kCycles; ++i) {
                auto pair = make_socket_pair_nb();
                if (pair[0] < 0 || pair[1] < 0) {
                    continue;
                }
                auto callback = [](int, int, void*) {};
                if (poll.add_event(pair[0], static_cast<int>(IOEvent::READ),
                                   callback)) {
                    add_ok++;
                    poll.remove_event(pair[0]);
                } else {
                    add_fail++;
                }
                CLOSE_SOCKET(pair[0]);
                CLOSE_SOCKET(pair[1]);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(add_ok.load() + add_fail.load(), kThreads * kCycles);
    EXPECT_EQ(poll.size(), 0U);
}

//==============================================================================
// events_to_string 辅助函数测试（event_poll_factory.cpp）
//==============================================================================

TEST(EventsToStringTest, EmptyMask) {
    EXPECT_EQ(events_to_string(0), "");
}

TEST(EventsToStringTest, SingleMasks) {
    EXPECT_EQ(events_to_string(static_cast<int>(IOEvent::READ)), "READ");
    EXPECT_EQ(events_to_string(static_cast<int>(IOEvent::WRITE)), "WRITE");
    EXPECT_EQ(events_to_string(static_cast<int>(IOEvent::ERR)), "ERROR");
    EXPECT_EQ(events_to_string(static_cast<int>(IOEvent::HANGUP)), "HANGUP");
}

TEST(EventsToStringTest, CombinedMasks) {
    EXPECT_EQ(events_to_string(
                  static_cast<int>(IOEvent::READ | IOEvent::WRITE)),
              "READ|WRITE");
    EXPECT_EQ(events_to_string(
                  static_cast<int>(IOEvent::READ | IOEvent::ERR | IOEvent::HANGUP)),
              "READ|ERROR|HANGUP");
    EXPECT_EQ(events_to_string(static_cast<int>(IOEvent::ALL)),
              "READ|WRITE|ERROR|HANGUP");
}

TEST(EventsToStringTest, UnknownBitsIgnored) {
    // 未定义的位不产生输出
    EXPECT_EQ(events_to_string(static_cast<int>(IOEvent::READ) | 0x100), "READ");
}

//==============================================================================
// 批次 S：已关闭正整数 fd 的注册探测 / 无事件 fd 的跳过
//==============================================================================

// POSIX 分支 add_event 以 fcntl(F_GETFL) 探测 fd 有效性：
// 已关闭的正整数 fd 返回 EBADF 而非 EINVAL，命中 fcntl 失败分支
#ifndef _WIN32
TEST(PollEventPollTest, AddEventWithClosedPositiveFdFails) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    const int fd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(fd, 0);
    ::close(fd);

    auto callback = [](int, int, void*) {};
    EXPECT_FALSE(poll.add_event(fd, static_cast<int>(IOEvent::READ), callback));
    EXPECT_NE(poll.get_error()[0], '\0');
    EXPECT_EQ(poll.size(), 0U);
}
#endif

// 两个已注册 fd 中只有一个就绪：poll 返回 1，静默 fd 的
// revents==0 被跳过，不触发回调
TEST(PollEventPollTest, PollSkipsIdleFdWhileOtherReady) {
    PollEventPoll poll_impl;
    EventPoll& poll = poll_impl;

    auto [a0, a1] = make_socket_pair_nb();
    auto [b0, b1] = make_socket_pair_nb();
    ASSERT_GE(a0, 0);
    ASSERT_GE(a1, 0);
    ASSERT_GE(b0, 0);
    ASSERT_GE(b1, 0);

    std::atomic<int> callback_count{0};
    auto callback = [&](int, int, void*) { callback_count++; };

    EXPECT_TRUE(poll.add_event(a0, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll.add_event(b0, static_cast<int>(IOEvent::READ), callback));

    // 只向 a1 写：a0 就绪，b0 保持静默
    const char message[] = "wake";
#ifdef _WIN32
    ASSERT_GT(send(a1, message, static_cast<int>(sizeof(message)), 0), 0);
#else
    ASSERT_GT(write(a1, message, sizeof(message)), 0);
#endif

    EXPECT_EQ(poll.poll(200), 1);
    EXPECT_EQ(callback_count.load(), 1);

    CLOSE_SOCKET(a0);
    CLOSE_SOCKET(a1);
    CLOSE_SOCKET(b0);
    CLOSE_SOCKET(b1);
}
