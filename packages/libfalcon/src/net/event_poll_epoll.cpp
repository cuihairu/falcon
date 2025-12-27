/**
 * @file event_poll_epoll.cpp
 * @brief epoll 实现 (Linux)
 * @author Falcon Team
 * @date 2025-12-24
 */

// 仅在 Linux 上编译 epoll 实现
#ifdef __linux__

#include <falcon/net/event_poll.hpp>
#include <falcon/logger.hpp>

#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

namespace falcon::net {

//==============================================================================
// EPollEventPoll 实现
//==============================================================================

EPollEventPoll::EPollEventPoll(int max_events)
    : epoll_fd_(-1)
    , max_events_(max_events)
{
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        set_error("epoll_create1 失败: " + std::string(strerror(errno)));
        return;
    }

    FALCON_LOG_INFO("创建 epoll 实例: fd=" << epoll_fd_);
}

EPollEventPoll::~EPollEventPoll() {
    clear();

    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        FALCON_LOG_DEBUG("关闭 epoll 实例: fd=" << epoll_fd_);
    }
}

bool EPollEventPoll::add_event(int fd, int events,
                               EventCallback callback,
                               void* user_data) {
    if (epoll_fd_ < 0) {
        return set_error("epoll 实例未创建");
    }

    // 设置为非阻塞模式
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return set_error("设置非阻塞模式失败: " + std::string(strerror(errno)));
    }

    // 转换事件类型
    uint32_t epoll_events = 0;
    if (events & static_cast<int>(IOEvent::READ)) {
        epoll_events |= EPOLLIN;
    }
    if (events & static_cast<int>(IOEvent::WRITE)) {
        epoll_events |= EPOLLOUT;
    }
    // EPOLLERR 和 EPOLLHUP 默认启用

    struct epoll_event ev = {};
    ev.events = epoll_events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return set_error("epoll_ctl(ADD) 失败: " + std::string(strerror(errno)));
    }

    // 保存回调信息
    events_[fd] = EventEntry(fd, events, std::move(callback), user_data);

    FALCON_LOG_DEBUG("添加 epoll 事件: fd=" << fd << ", events=" << events);
    return true;
}

bool EPollEventPoll::modify_event(int fd, int events) {
    if (epoll_fd_ < 0) {
        return set_error("epoll 实例未创建");
    }

    auto it = events_.find(fd);
    if (it == events_.end()) {
        return set_error("文件描述符未注册: fd=" + std::to_string(fd));
    }

    // 转换事件类型
    uint32_t epoll_events = 0;
    if (events & static_cast<int>(IOEvent::READ)) {
        epoll_events |= EPOLLIN;
    }
    if (events & static_cast<int>(IOEvent::WRITE)) {
        epoll_events |= EPOLLOUT;
    }

    struct epoll_event ev = {};
    ev.events = epoll_events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        return set_error("epoll_ctl(MOD) 失败: " + std::string(strerror(errno)));
    }

    // 更新事件类型
    it->second.events = events;

    FALCON_LOG_DEBUG("修改 epoll 事件: fd=" << fd << ", events=" << events);
    return true;
}

bool EPollEventPoll::remove_event(int fd) {
    if (epoll_fd_ < 0) {
        return set_error("epoll 实例未创建");
    }

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        return set_error("epoll_ctl(DEL) 失败: " + std::string(strerror(errno)));
    }

    events_.erase(fd);

    FALCON_LOG_DEBUG("移除 epoll 事件: fd=" << fd);
    return true;
}

int EPollEventPoll::poll(int timeout_ms) {
    if (epoll_fd_ < 0) {
        set_error("epoll 实例未创建");
        return -1;
    }

    // 分配事件数组
    std::vector<struct epoll_event> epoll_events(static_cast<std::size_t>(max_events_));

    int nfds = epoll_wait(epoll_fd_, epoll_events.data(),
                         max_events_, timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) {
            return 0;  // 被信号中断，不算错误
        }
        set_error("epoll_wait 失败: " + std::string(strerror(errno)));
        return -1;
    }

    // 处理就绪事件
    for (int i = 0; i < nfds; ++i) {
        const auto& ev = epoll_events[static_cast<std::size_t>(i)];
        int fd = ev.data.fd;

        auto it = events_.find(fd);
        if (it == events_.end()) {
            FALCON_LOG_WARN("未知 fd 事件: fd=" << fd);
            continue;
        }

        // 转换事件类型
        int events = 0;
        if (ev.events & EPOLLIN) {
            events |= static_cast<int>(IOEvent::READ);
        }
        if (ev.events & EPOLLOUT) {
            events |= static_cast<int>(IOEvent::WRITE);
        }
        if (ev.events & (EPOLLERR | EPOLLHUP)) {
            events |= static_cast<int>(IOEvent::ERROR);
        }

        // 调用回调函数
        const auto& entry = it->second;
        if (entry.callback) {
            entry.callback(fd, events, entry.user_data);
        }
    }

    return nfds;
}

void EPollEventPoll::clear() {
    // 移除所有文件描述符
    for (const auto& pair : events_) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, pair.first, nullptr);
    }
    events_.clear();
}

bool EPollEventPoll::set_error(const std::string& msg) {
    error_msg_ = msg;
    FALCON_LOG_ERROR("EPollEventPoll: " << msg);
    return false;
}

} // namespace falcon::net

#endif // __linux__
