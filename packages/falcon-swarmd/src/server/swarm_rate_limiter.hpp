#pragma once

// ============================================================================
// SwarmRateLimiter：per-key 滑动窗口限频器（设计文档 §9.1 限频配额防线）
//
// 命中判定纯内存零 I/O；`now` 显式入参——单测以虚拟时钟推进窗口，
// 零 sleep 零真实时间依赖。server 侧实例化两个：register-per-IP 与
// query-per-IP；命中后 HTTP 层回 429 + JSON-RPC -32002。
//
// 语义：
//   - 滑动窗口 [now - window, now]：过期时间戳先淘汰再计数；
//   - 窗口内事件数达 max_events 后同 key 拒绝（返回 false）；
//   - max_events == 0 = 不限频（恒放行，配置关闭限频的形态）；
//   - allow 返回 true 时该事件已计入窗口（先判后记，原子的判定+记账）。
// ============================================================================

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace falcon::swarm {

class SwarmRateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    // max_events：窗口内允许的最大事件数（0 = 不限）；window：窗口宽度
    SwarmRateLimiter(std::size_t max_events, std::chrono::milliseconds window);

    SwarmRateLimiter(const SwarmRateLimiter&) = delete;
    SwarmRateLimiter& operator=(const SwarmRateLimiter&) = delete;

    // 判定 + 记账；now 由调用方提供（server 用 steady_clock::now()，
    // 单测用推进的虚拟时刻）
    bool allow(const std::string& key, Clock::time_point now);

    std::size_t max_events() const { return max_events_; }
    std::chrono::milliseconds window() const { return window_; }

private:
    std::size_t max_events_;
    std::chrono::milliseconds window_;
    std::mutex mutex_;
    std::map<std::string, std::deque<Clock::time_point>> windows_;
};

}  // namespace falcon::swarm
