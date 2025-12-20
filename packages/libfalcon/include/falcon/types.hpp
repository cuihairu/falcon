#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace falcon {

/// Task ID type
using TaskId = std::uint64_t;

/// Invalid task ID constant
constexpr TaskId INVALID_TASK_ID = 0;

/// Bytes type for file sizes
using Bytes = std::uint64_t;

/// Speed type (bytes per second)
using BytesPerSecond = std::uint64_t;

/// Time point type
using TimePoint = std::chrono::steady_clock::time_point;

/// Duration type
using Duration = std::chrono::steady_clock::duration;

/// File information structure
struct FileInfo {
    std::string url;
    std::string filename;
    std::string content_type;
    Bytes total_size = 0;
    bool supports_resume = false;
    TimePoint last_modified{};
};

/// Progress information
struct ProgressInfo {
    TaskId task_id = INVALID_TASK_ID;
    Bytes downloaded_bytes = 0;
    Bytes total_bytes = 0;
    BytesPerSecond speed = 0;
    float progress = 0.0f;  // 0.0 ~ 1.0
    Duration elapsed{};
    Duration estimated_remaining{};
};

}  // namespace falcon
