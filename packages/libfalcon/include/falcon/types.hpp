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

/// Command ID type
using CommandId = std::uint64_t;

/// Invalid command ID constant
constexpr CommandId INVALID_COMMAND_ID = 0;

/// Segment ID type (for segmented downloads)
using SegmentId = std::uint32_t;

/// Bytes type for file sizes
using Bytes = std::uint64_t;

/// Speed type (bytes per second)
using Speed = std::uint64_t;

/// Bytes per second type (alias for Speed)
using BytesPerSecond = Speed;

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
    Speed speed = 0;
    float progress = 0.0f;  // 0.0 ~ 1.0
    Duration elapsed{};
    Duration estimated_remaining{};
};

}  // namespace falcon
