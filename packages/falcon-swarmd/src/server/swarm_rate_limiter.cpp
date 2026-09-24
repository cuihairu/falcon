// ============================================================================
// SwarmRateLimiter 实现（见 swarm_rate_limiter.hpp 头注释）
// ============================================================================

#include "swarm_rate_limiter.hpp"

namespace falcon::swarm {

SwarmRateLimiter::SwarmRateLimiter(std::size_t max_events,
                                   std::chrono::milliseconds window)
    : max_events_(max_events), window_(window) {}

bool SwarmRateLimiter::allow(const std::string& key, Clock::time_point now) {
    if (max_events_ == 0) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::deque<Clock::time_point>& win = windows_[key];

    const Clock::time_point cutoff = now - window_;
    while (!win.empty() && win.front() <= cutoff) {
        win.pop_front();
    }

    if (win.size() >= max_events_) {
        return false;
    }
    win.push_back(now);
    return true;
}

}  // namespace falcon::swarm
