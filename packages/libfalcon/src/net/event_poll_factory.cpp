/**
 * @file event_poll_factory.cpp
 * @brief EventPoll 工厂方法与辅助函数
 * @author Falcon Team
 * @date 2025-12-25
 */

#include <falcon/net/event_poll.hpp>
#include <falcon/logger.hpp>

namespace falcon::net {

std::unique_ptr<EventPoll> EventPoll::create() {
#if defined(__linux__)
    FALCON_LOG_INFO("使用 EPollEventPoll (Linux)");
    return std::make_unique<EPollEventPoll>();
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    FALCON_LOG_INFO("使用 KqueueEventPoll (macOS/BSD)");
    return std::make_unique<KqueueEventPoll>();
#else
    FALCON_LOG_INFO("使用 PollEventPoll (通用)");
    return std::make_unique<PollEventPoll>();
#endif
}

std::string events_to_string(int events) {
    std::string result;
    if (events & static_cast<int>(IOEvent::READ)) {
        result += "READ|";
    }
    if (events & static_cast<int>(IOEvent::WRITE)) {
        result += "WRITE|";
    }
    if (events & static_cast<int>(IOEvent::ERR)) {
        result += "ERROR|";
    }
    if (events & static_cast<int>(IOEvent::HANGUP)) {
        result += "HANGUP|";
    }
    if (!result.empty()) {
        result.pop_back();  // remove trailing '|'
    }
    return result;
}

} // namespace falcon::net

