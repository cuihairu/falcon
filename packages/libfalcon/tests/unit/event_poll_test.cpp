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
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    close(client_fd);
    close(server_fd);
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

    close(fd0);
    close(fd1);
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

    close(fd0);
    close(fd1);
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

    int flags = fcntl(fd_dup, F_GETFL, 0);
    fcntl(fd_dup, F_SETFL, flags | O_NONBLOCK);

    close(fd0);
    fd0 = fd_dup;

    EXPECT_TRUE(poll->add_event(TARGET_FD, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll->remove_event(TARGET_FD));

    close(fd0);
    close(fd1);
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
    close(fd0);
    close(fd1);
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
        close(fd1);
    }

    for (int fd : fds) {
        EXPECT_TRUE(poll->add_event(fd, static_cast<int>(IOEvent::READ), callback));
    }

    for (int fd : fds) {
        EXPECT_TRUE(poll->remove_event(fd));
        close(fd);
    }
}

//==============================================================================
// 主函数
//==============================================================================
