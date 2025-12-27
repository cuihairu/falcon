/**
 * @file event_poll.hpp
 * @brief I/O 多路复用接口 - aria2 风格
 * @author Falcon Team
 * @date 2025-12-24
 *
 * 设计参考: aria2/src/EventPoll.h
 * @see https://github.com/aria2/aria2/blob/master/src/EventPoll.h
 */

#pragma once

#include <falcon/types.hpp>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <cstring>

// 平台相关头文件
#ifdef _WIN32
    #include <winsock2.h>
    // Windows 上使用 WSAPOLLFD 替代 pollfd
    #ifndef pollfd
    #define pollfd WSAPOLLFD
    #endif
    // Windows 上使用 WSAPoll 替代 poll
    #ifndef poll
    #define poll WSAPoll
    #endif
    // Windows poll 常量映射到 WSAPoll 常量
    #ifndef POLLIN
    #define POLLIN  POLLRDNORM
    #endif
    #ifndef POLLOUT
    #define POLLOUT POLLWRNORM
    #endif
    #ifndef POLLERR
    #define POLLERR  (0x0001)
    #endif
    #ifndef POLLHUP
    #define POLLHUP  (0x0002)
    #endif
    #ifndef POLLNVAL
    #define POLLNVAL (0x0004)
    #endif
#else
    #ifdef __linux__
        #include <sys/epoll.h>
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        #include <sys/event.h>
    #endif
    #include <poll.h>      // for struct pollfd
    #include <sys/socket.h> // for socket functions
    #include <unistd.h>    // for close()
#endif

namespace falcon::net {

/**
 * @brief I/O 事件类型
 *
 * 可以使用按位或运算组合多个事件
 */
enum class IOEvent : int {
    NONE = 0,
    READ = 1,      // 可读事件
    WRITE = 2,     // 可写事件
    ERR = 4,       // 错误事件（原 ERROR，避免与 Windows 宏冲突）
    HANGUP = 8,    // 挂起事件（对端关闭连接）
    ALL = READ | WRITE | ERR | HANGUP
};

/**
 * @brief 事件标志位运算
 */
inline IOEvent operator|(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<int>(a) | static_cast<int>(b));
}

inline IOEvent operator&(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<int>(a) & static_cast<int>(b));
}

inline IOEvent& operator|=(IOEvent& a, IOEvent b) {
    return a = a | b;
}

inline bool has_event(IOEvent flags, IOEvent event) {
    return (static_cast<int>(flags) & static_cast<int>(event)) != 0;
}

/**
 * @brief 事件回调函数类型
 *
 * @param fd 文件描述符
 * @param events 发生的事件类型
 * @param user_data 用户自定义数据
 */
using EventCallback = std::function<void(int fd, int events, void* user_data)>;

/**
 * @brief 事件注册信息
 */
struct EventEntry {
    int fd;                         // 文件描述符
    int events;                     // 监听的事件类型
    EventCallback callback;         // 事件回调
    void* user_data = nullptr;      // 用户数据

    EventEntry() : fd(-1), events(0), callback(nullptr), user_data(nullptr) {}

    EventEntry(int f, int e, EventCallback cb, void* ud = nullptr)
        : fd(f), events(e), callback(std::move(cb)), user_data(ud) {}
};

/**
 * @brief Poll 结果
 */
struct PollResult {
    int fd;         // 文件描述符
    int events;     // 发生的事件
    void* user_data; // 用户数据

    PollResult(int f = -1, int e = 0, void* ud = nullptr)
        : fd(f), events(e), user_data(ud) {}
};

/**
 * @brief I/O 多路复用接口
 *
 * 跨平台封装：
 * - Linux: epoll (EPollEventPoll)
 * - macOS/BSD: kqueue (KqueueEventPoll)
 * - Windows: WSAPoll (PollEventPoll)
 *
 * 对应 aria2 的 EventPoll 类
 * @see https://github.com/aria2/aria2/blob/master/src/EventPoll.h
 */
class EventPoll {
public:
    virtual ~EventPoll() = default;

    /**
     * @brief 添加/注册文件描述符监听
     *
     * @param fd 文件描述符
     * @param events 事件类型（IOEvent 的组合）
     * @param callback 事件回调函数
     * @param user_data 用户自定义数据（传递给回调）
     * @return true 成功，false 失败
     */
    virtual bool add_event(int fd, int events,
                          EventCallback callback,
                          void* user_data = nullptr) = 0;

    /**
     * @brief 修改文件描述符的事件监听
     *
     * @param fd 文件描述符
     * @param events 新的事件类型
     * @return true 成功，false 失败
     */
    virtual bool modify_event(int fd, int events) = 0;

    /**
     * @brief 移除文件描述符监听
     *
     * @param fd 文件描述符
     * @return true 成功，false 失败
     */
    virtual bool remove_event(int fd) = 0;

    /**
     * @brief 等待事件（阻塞）
     *
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return 发生的事件数量，0 表示超时，-1 表示错误
     */
    virtual int poll(int timeout_ms = -1) = 0;

    /**
     * @brief 获取最后一次错误信息
     */
    virtual const char* get_error() const = 0;

    /**
     * @brief 获取当前监听的文件描述符数量
     */
    virtual std::size_t size() const = 0;

    /**
     * @brief 清空所有监听的文件描述符
     */
    virtual void clear() = 0;

    /**
     * @brief 创建平台特定的 EventPoll 实现
     *
     * 根据编译平台自动选择最优实现：
     * - Linux: EPollEventPoll
     * - macOS/BSD: KqueueEventPoll
     * - 其他: PollEventPoll（fallback）
     *
     * @return EventPoll 智能指针
     */
    static std::unique_ptr<EventPoll> create();

protected:
    EventPoll() = default;
};

/**
 * @brief epoll 实现 (Linux)
 *
 * 高性能事件通知机制，支持大量文件描述符
 * 仅在 Linux 系统上可用
 */
class EPollEventPoll : public EventPoll {
public:
    explicit EPollEventPoll(int max_events = 1024);
    ~EPollEventPoll() override;

    bool add_event(int fd, int events,
                  EventCallback callback,
                  void* user_data = nullptr) override;

    bool modify_event(int fd, int events) override;
    bool remove_event(int fd) override;

    int poll(int timeout_ms = -1) override;

    const char* get_error() const override { return error_msg_.c_str(); }
    std::size_t size() const override { return events_.size(); }
    void clear() override;

private:
    int epoll_fd_;
    int max_events_;
    std::string error_msg_;

    // 事件回调映射表
    std::map<int, EventEntry> events_;

    bool set_error(const std::string& msg);
};

#ifdef __APPLE__
/**
 * @brief kqueue 实现 (macOS/BSD)
 *
 * macOS 和 BSD 系统的高性能事件通知机制
 */
class KqueueEventPoll : public EventPoll {
public:
    explicit KqueueEventPoll(int max_events = 1024);
    ~KqueueEventPoll() override;

    bool add_event(int fd, int events,
                  EventCallback callback,
                  void* user_data = nullptr) override;

    bool modify_event(int fd, int events) override;
    bool remove_event(int fd) override;

    int poll(int timeout_ms = -1) override;

    const char* get_error() const override { return error_msg_.c_str(); }
    std::size_t size() const override { return events_.size(); }
    void clear() override;

private:
    int kqueue_fd_;
    int max_events_;
    std::string error_msg_;

    // 事件回调映射表
    std::map<int, EventEntry> events_;

    bool set_error(const std::string& msg);

    // 转换 IOEvent 到 kevent filter
    short to_filter(int events) const;
    short to_flags(int events) const;
};
#endif // __APPLE__

/**
 * @brief poll 实现 (通用 fallback)
 *
 * 使用标准 poll() 系统调用，性能较低但可移植性好
 */
class PollEventPoll : public EventPoll {
public:
    explicit PollEventPoll(int max_fds = 1024);
    ~PollEventPoll() override = default;

    bool add_event(int fd, int events,
                  EventCallback callback,
                  void* user_data = nullptr) override;

    bool modify_event(int fd, int events) override;
    bool remove_event(int fd) override;

    int poll(int timeout_ms = -1) override;

    const char* get_error() const override { return error_msg_.c_str(); }
    std::size_t size() const override { return events_.size(); }
    void clear() override;

private:
    int max_fds_;
    std::string error_msg_;

    // 事件回调映射表
    std::map<int, EventEntry> events_;

    // pollfd 数组（动态分配）
    std::vector<struct pollfd> poll_fds_;

    bool set_error(const std::string& msg);
    void rebuild_poll_fds();

    // 转换 IOEvent 到 poll 事件
    short to_poll_events(int events) const;
};

/**
 * @brief 辅助函数：将 IOEvent 转换为字符串（用于调试）
 */
inline const char* event_to_string(IOEvent event) {
    switch (event) {
        case IOEvent::READ:  return "READ";
        case IOEvent::WRITE: return "WRITE";
        case IOEvent::ERR:   return "ERROR";
        case IOEvent::HANGUP: return "HANGUP";
        default: return "UNKNOWN";
    }
}

/**
 * @brief 辅助函数：将事件标志转换为字符串
 */
std::string events_to_string(int events);

} // namespace falcon::net
