/**
 * @file event_poll_kqueue.cpp
 * @brief kqueue 实现 (macOS/BSD)
 * @author Falcon Team
 * @date 2025-12-24
 */

// 仅在 macOS/BSD 上编译 kqueue 实现
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)

#include <falcon/net/event_poll.hpp>
#include <falcon/logger.hpp>

#include <sys/event.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>  // for fcntl, F_GETFL, F_SETFL, O_NONBLOCK

namespace falcon::net {

//==============================================================================
// KqueueEventPoll 实现
//==============================================================================

KqueueEventPoll::KqueueEventPoll(int max_events)
    : kqueue_fd_(-1)
    , max_events_(max_events)
{
    kqueue_fd_ = kqueue();
    if (kqueue_fd_ < 0) {
        set_error("kqueue() 失败: " + std::string(strerror(errno)));
        return;
    }

    FALCON_LOG_INFO("创建 kqueue 实例: fd=" << kqueue_fd_);
}

KqueueEventPoll::~KqueueEventPoll() {
    clear();

    if (kqueue_fd_ >= 0) {
        close(kqueue_fd_);
        FALCON_LOG_DEBUG("关闭 kqueue 实例: fd=" << kqueue_fd_);
    }
}

bool KqueueEventPoll::add_event(int fd, int events,
                                EventCallback callback,
                                void* user_data) {
    if (kqueue_fd_ < 0) {
        return set_error("kqueue 实例未创建");
    }

    // 设置为非阻塞模式
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return set_error("设置非阻塞模式失败: " + std::string(strerror(errno)));
    }

    // 创建 kevent 数组
    struct kevent kevents[2];
    int nevents = 0;

    // 注册读事件
    if (events & static_cast<int>(IOEvent::READ)) {
        EV_SET(&kevents[nevents], fd, EVFILT_READ, EV_ADD | EV_ENABLE,
               0, 0, nullptr);
        ++nevents;
    }

    // 注册写事件
    if (events & static_cast<int>(IOEvent::WRITE)) {
        EV_SET(&kevents[nevents], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE,
               0, 0, nullptr);
        ++nevents;
    }

    if (nevents > 0) {
        if (kevent(kqueue_fd_, kevents, nevents, nullptr, 0, nullptr) < 0) {
            return set_error("kevent(ADD) 失败: " + std::string(strerror(errno)));
        }
    }

    // 保存回调信息
    events_[fd] = EventEntry(fd, events, std::move(callback), user_data);

    FALCON_LOG_DEBUG("添加 kqueue 事件: fd=" << fd << ", events=" << events);
    return true;
}

bool KqueueEventPoll::modify_event(int fd, int events) {
    if (kqueue_fd_ < 0) {
        return set_error("kqueue 实例未创建");
    }

    auto it = events_.find(fd);
    if (it == events_.end()) {
        return set_error("文件描述符未注册: fd=" + std::to_string(fd));
    }

    int old_events = it->second.events;

    // 创建 kevent 数组
    struct kevent kevents[4];
    int nevents = 0;

    // 处理读事件变更
    bool old_read = old_events & static_cast<int>(IOEvent::READ);
    bool new_read = events & static_cast<int>(IOEvent::READ);

    if (old_read && !new_read) {
        // 禁用读事件
        EV_SET(&kevents[nevents], fd, EVFILT_READ, EV_DISABLE, 0, 0, nullptr);
        ++nevents;
    } else if (!old_read && new_read) {
        // 启用读事件
        EV_SET(&kevents[nevents], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ++nevents;
    }

    // 处理写事件变更
    bool old_write = old_events & static_cast<int>(IOEvent::WRITE);
    bool new_write = events & static_cast<int>(IOEvent::WRITE);

    if (old_write && !new_write) {
        // 禁用写事件
        EV_SET(&kevents[nevents], fd, EVFILT_WRITE, EV_DISABLE, 0, 0, nullptr);
        ++nevents;
    } else if (!old_write && new_write) {
        // 启用写事件
        EV_SET(&kevents[nevents], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ++nevents;
    }

    if (nevents > 0) {
        if (kevent(kqueue_fd_, kevents, nevents, nullptr, 0, nullptr) < 0) {
            return set_error("kevent(MOD) 失败: " + std::string(strerror(errno)));
        }
    }

    // 更新事件类型
    it->second.events = events;

    FALCON_LOG_DEBUG("修改 kqueue 事件: fd=" << fd << ", events=" << events);
    return true;
}

bool KqueueEventPoll::remove_event(int fd) {
    if (kqueue_fd_ < 0) {
        return set_error("kqueue 实例未创建");
    }

    auto it = events_.find(fd);
    if (it == events_.end()) {
        return false;  // fd 不存在，返回 false（与 epoll 行为一致）
    }

    int events = it->second.events;

    // 创建 kevent 数组删除事件
    struct kevent kevents[2];
    int nevents = 0;

    if (events & static_cast<int>(IOEvent::READ)) {
        EV_SET(&kevents[nevents], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        ++nevents;
    }

    if (events & static_cast<int>(IOEvent::WRITE)) {
        EV_SET(&kevents[nevents], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        ++nevents;
    }

    if (nevents > 0) {
        // 忽略 ENOENT 错误（事件可能已经自动删除）
        if (kevent(kqueue_fd_, kevents, nevents, nullptr, 0, nullptr) < 0) {
            if (errno != ENOENT) {
                return set_error("kevent(DEL) 失败: " + std::string(strerror(errno)));
            }
        }
    }

    events_.erase(fd);

    FALCON_LOG_DEBUG("移除 kqueue 事件: fd=" << fd);
    return true;
}

int KqueueEventPoll::poll(int timeout_ms) {
    if (kqueue_fd_ < 0) {
        set_error("kqueue 实例未创建");
        return -1;
    }

    // 分配事件数组
    std::vector<struct kevent> kevents(max_events_);

    // 计算超时
    struct timespec timeout = {};
    struct timespec* timeout_ptr = nullptr;
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (timeout_ms % 1000) * 1000000;
        timeout_ptr = &timeout;
    }

    int nevents = kevent(kqueue_fd_, nullptr, 0, kevents.data(),
                         max_events_, timeout_ptr);
    if (nevents < 0) {
        if (errno == EINTR) {
            return 0;  // 被信号中断
        }
        set_error("kevent (wait) 失败: " + std::string(strerror(errno)));
        return -1;
    }

    // 处理就绪事件
    for (int i = 0; i < nevents; ++i) {
        const auto& ev = kevents[i];
        int fd = static_cast<int>(ev.ident);

        auto it = events_.find(fd);
        if (it == events_.end()) {
            FALCON_LOG_WARN("未知 fd 事件: fd=" << fd);
            continue;
        }

        // 转换事件类型
        int events = 0;
        if (ev.filter == EVFILT_READ) {
            events |= static_cast<int>(IOEvent::READ);
        }
        if (ev.filter == EVFILT_WRITE) {
            events |= static_cast<int>(IOEvent::WRITE);
        }
        if (ev.flags & (EV_ERROR | EV_EOF)) {
            events |= static_cast<int>(IOEvent::ERR);
        }

        // 调用回调函数
        const auto& entry = it->second;
        if (entry.callback) {
            entry.callback(fd, events, entry.user_data);
        }
    }

    return nevents;
}

void KqueueEventPoll::clear() {
    // 移除所有文件描述符（避免在遍历 map 时修改它）
    std::vector<int> fds;
    fds.reserve(events_.size());
    for (const auto& pair : events_) {
        fds.push_back(pair.first);
    }
    for (int fd : fds) {
        remove_event(fd);
    }
}

bool KqueueEventPoll::set_error(const std::string& msg) {
    error_msg_ = msg;
    FALCON_LOG_ERROR("KqueueEventPoll: " << msg);
    return false;
}

} // namespace falcon::net

#endif // __APPLE__ || __FreeBSD__ || __NetBSD__ || __OpenBSD__
