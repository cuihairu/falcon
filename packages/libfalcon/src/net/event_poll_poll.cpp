/**
 * @file event_poll_poll.cpp
 * @brief poll 实现 (通用 fallback)
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <falcon/net/event_poll.hpp>
#include <falcon/logger.hpp>

#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>  // for fcntl, F_GETFL, F_SETFL, O_NONBLOCK

namespace falcon::net {

//==============================================================================
// PollEventPoll 实现
//==============================================================================

PollEventPoll::PollEventPoll(int max_fds)
    : max_fds_(max_fds)
{
    poll_fds_.reserve(max_fds_);
    FALCON_LOG_INFO("创建 PollEventPoll: max_fds=" << max_fds_);
}

bool PollEventPoll::add_event(int fd, int events,
                              EventCallback callback,
                              void* user_data) {
    if (static_cast<int>(events_.size()) >= max_fds_) {
        return set_error("超过最大文件描述符数量");
    }

    // 设置为非阻塞模式
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return set_error("设置非阻塞模式失败: " + std::string(strerror(errno)));
    }

    // 保存回调信息
    events_[fd] = EventEntry(fd, events, std::move(callback), user_data);

    // 重建 poll_fds 数组
    rebuild_poll_fds();

    FALCON_LOG_DEBUG("添加 poll 事件: fd=" << fd << ", events=" << events);
    return true;
}

bool PollEventPoll::modify_event(int fd, int events) {
    auto it = events_.find(fd);
    if (it == events_.end()) {
        return set_error("文件描述符未注册: fd=" + std::to_string(fd));
    }

    // 更新事件类型
    it->second.events = events;

    // 重建 poll_fds 数组
    rebuild_poll_fds();

    FALCON_LOG_DEBUG("修改 poll 事件: fd=" << fd << ", events=" << events);
    return true;
}

bool PollEventPoll::remove_event(int fd) {
    auto it = events_.find(fd);
    if (it == events_.end()) {
        return true;  // 已经不存在
    }

    events_.erase(fd);

    // 重建 poll_fds 数组
    rebuild_poll_fds();

    FALCON_LOG_DEBUG("移除 poll 事件: fd=" << fd);
    return true;
}

int PollEventPoll::poll(int timeout_ms) {
    if (poll_fds_.empty()) {
        return 0;  // 没有待监听的文件描述符
    }

    int nfds = ::poll(poll_fds_.data(), poll_fds_.size(), timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) {
            return 0;  // 被信号中断
        }
        set_error("poll() 失败: " + std::string(strerror(errno)));
        return -1;
    }

    if (nfds == 0) {
        return 0;  // 超时
    }

    // 处理就绪事件
    for (const auto& pfd : poll_fds_) {
        if (pfd.revents == 0) {
            continue;  // 无事件
        }

        int fd = pfd.fd;
        auto it = events_.find(fd);
        if (it == events_.end()) {
            FALCON_LOG_WARN("未知 fd 事件: fd=" << fd);
            continue;
        }

        // 转换事件类型
        int events = 0;
        if (pfd.revents & POLLIN) {
            events |= static_cast<int>(IOEvent::READ);
        }
        if (pfd.revents & POLLOUT) {
            events |= static_cast<int>(IOEvent::WRITE);
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
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

void PollEventPoll::clear() {
    events_.clear();
    poll_fds_.clear();
}

bool PollEventPoll::set_error(const std::string& msg) {
    error_msg_ = msg;
    FALCON_LOG_ERROR("PollEventPoll: " << msg);
    return false;
}

void PollEventPoll::rebuild_poll_fds() {
    poll_fds_.clear();

    for (const auto& pair : events_) {
        const auto& entry = pair.second;
        struct pollfd pfd = {};
        pfd.fd = entry.fd;
        pfd.events = to_poll_events(entry.events);
        pfd.revents = 0;
        poll_fds_.push_back(pfd);
    }
}

short PollEventPoll::to_poll_events(int events) const {
    short poll_events = 0;

    if (events & static_cast<int>(IOEvent::READ)) {
        poll_events |= POLLIN;
    }
    if (events & static_cast<int>(IOEvent::WRITE)) {
        poll_events |= POLLOUT;
    }

    return poll_events;
}

} // namespace falcon::net
