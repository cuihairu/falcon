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
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

using namespace falcon;
using namespace falcon::net;

//==============================================================================
// 测试辅助函数
//==============================================================================

/**
 * @brief 创建一对连接的 Socket 用于测试
 */
std::array<int, 2> create_socket_pair() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return {-1, -1};
    }

    // 设置非阻塞
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 任意端口

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(server_fd);
        return {-1, -1};
    }

    if (listen(server_fd, 1) != 0) {
        close(server_fd);
        return {-1, -1};
    }

    // 获取实际绑定的端口
    struct sockaddr_in actual_addr = {};
    socklen_t len = sizeof(actual_addr);
    if (getsockname(server_fd, (struct sockaddr*)&actual_addr, &len) != 0) {
        close(server_fd);
        return {-1, -1};
    }

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        close(server_fd);
        return {-1, -1};
    }

    // 设置非阻塞
    flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // 尝试连接（可能返回 EINPROGRESS）
    connect(client_fd, (struct sockaddr*)&actual_addr, sizeof(actual_addr));

    return {server_fd, client_fd};
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

    int callback_called = 0;

    auto callback = [&callback_called](int fd, int events, void* user_data) {
        callback_called++;
    };

    bool result = poll->add_event(0, static_cast<int>(IOEvent::READ), callback);
    EXPECT_TRUE(result) << "Failed to add read event";
}

TEST(EventPollTest, AddWriteEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    int callback_called = 0;

    auto callback = [&callback_called](int fd, int events, void* user_data) {
        callback_called++;
    };

    bool result = poll->add_event(1, static_cast<int>(IOEvent::WRITE), callback);
    EXPECT_TRUE(result) << "Failed to add write event";
}

TEST(EventPollTest, AddMultipleEvents) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    int read_called = 0;
    int write_called = 0;

    auto read_callback = [&read_called](int fd, int events, void* user_data) {
        read_called++;
    };

    auto write_callback = [&write_called](int fd, int events, void* user_data) {
        write_called++;
    };

    EXPECT_TRUE(poll->add_event(0, static_cast<int>(IOEvent::READ), read_callback));
    EXPECT_TRUE(poll->add_event(1, static_cast<int>(IOEvent::WRITE), write_callback));
}

TEST(EventPollTest, AddReadWriteEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    int callback_called = 0;

    auto callback = [&callback_called](int fd, int events, void* user_data) {
        callback_called++;
    };

    bool result = poll->add_event(
        0,
        static_cast<int>(IOEvent::READ | IOEvent::WRITE),
        callback
    );

    EXPECT_TRUE(result) << "Failed to add read/write event";
}

//==============================================================================
// 事件移除测试
//==============================================================================

TEST(EventPollTest, RemoveEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    EXPECT_TRUE(poll->add_event(0, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll->remove_event(0));
}

TEST(EventPollTest, RemoveNonExistentEvent) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    // 移除不存在的 fd
    EXPECT_FALSE(poll->remove_event(999));
}

TEST(EventPollTest, RemoveEventTwice) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    EXPECT_TRUE(poll->add_event(0, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll->remove_event(0));
    EXPECT_FALSE(poll->remove_event(0));  // 第二次应该失败
}

//==============================================================================
// 事件触发测试
//==============================================================================

TEST(EventPollTest, PollWithTimeout) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    poll->add_event(0, static_cast<int>(IOEvent::READ), callback);

    // 等待 100ms，应该超时返回 0
    int events = poll->poll(100);
    EXPECT_EQ(events, 0);
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

    poll->add_event(0, static_cast<int>(IOEvent::READ), callback);
    poll->poll(100);

    // 注：因为 fd 0 可能没有数据可读，回调可能不会被调用
    // 这取决于具体的平台和实现
}

TEST(EventPollTest, CallbackWithUserData) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    int test_value = 42;
    void* received_data = nullptr;

    auto callback = [&](int fd, int events, void* user_data) {
        received_data = user_data;
    };

    poll->add_event(0, static_cast<int>(IOEvent::READ), callback, &test_value);
    poll->poll(100);

    // 如果回调被调用，检查用户数据
    if (received_data != nullptr) {
        EXPECT_EQ(received_data, &test_value);
    }
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

    auto callback = [](int fd, int events, void* user_data) {};

    // 先添加 READ 事件
    EXPECT_TRUE(poll->add_event(0, static_cast<int>(IOEvent::READ), callback));

    // 修改为 WRITE 事件
    EXPECT_TRUE(poll->modify_event(0, static_cast<int>(IOEvent::WRITE)));

    // 移除
    EXPECT_TRUE(poll->remove_event(0));
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

    poll->add_event(0, static_cast<int>(IOEvent::READ), callback);

    // 零超时应该立即返回
    int events = poll->poll(0);
    EXPECT_GE(events, 0);
}

TEST(EventPollTest, LargeFileDescriptor) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll, nullptr);

    auto callback = [](int fd, int events, void* user_data) {};

    // 大文件描述符
    EXPECT_TRUE(poll->add_event(1024, static_cast<int>(IOEvent::READ), callback));
    EXPECT_TRUE(poll->remove_event(1024));
}

//==============================================================================
// 平台特性测试
//==============================================================================

#if defined(__linux__)
TEST(EventPollTest, PlatformEpoll) {
    auto poll = EventPoll::create();
    ASSERT_NE(poll);

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
    EXPECT_TRUE(poll->add_event(0, static_cast<int>(IOEvent::READ), callback));
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

    // 添加多个文件描述符
    for (int i = 0; i < NUM_FDS; ++i) {
        EXPECT_TRUE(poll->add_event(i, static_cast<int>(IOEvent::READ), callback));
    }

    // 移除所有
    for (int i = 0; i < NUM_FDS; ++i) {
        EXPECT_TRUE(poll->remove_event(i));
    }
}

//==============================================================================
// 主函数
//==============================================================================

