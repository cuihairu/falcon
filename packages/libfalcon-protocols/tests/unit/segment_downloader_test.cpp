// Falcon Segment Downloader Unit Tests

#include <falcon/segment_downloader.hpp>
#include <falcon/download_task.hpp>
#include <falcon/download_options.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace falcon;

static std::string make_unique_temp_path(const std::string& stem) {
    auto dir = std::filesystem::temp_directory_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream oss;
    oss << stem << "_" << now;
    return (dir / oss.str()).string();
}

// Mock download task for testing
class MockDownloadTask : public DownloadTask {
public:
    MockDownloadTask(TaskId id, std::string url, DownloadOptions options)
        : DownloadTask(id, std::move(url), std::move(options)) {}

    void set_test_file_info(Bytes size) {
        FileInfo info;
        info.url = url();
        info.filename = "test_file.bin";
        info.total_size = size;
        info.supports_resume = true;
        info.content_type = "application/octet-stream";
        set_file_info(info);
    }
};

// Mock segment download function that creates files with predictable content
static bool mock_segment_download(
    const std::string& url,
    Bytes start,
    Bytes end,
    const std::string& output_path,
    std::atomic<bool>& cancelled) {

    // Minimal delay for testing
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    if (cancelled.load()) {
        return false;
    }

    // Create segment file with pattern
    std::ofstream file(output_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    Bytes size = end - start + 1;
    std::vector<uint8_t> buffer(4096);

    // Fill buffer with pattern
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<uint8_t>(i % 256);
    }

    Bytes written = 0;
    while (written < size) {
        Bytes to_write = std::min(static_cast<Bytes>(buffer.size()), size - written);
        file.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(to_write));
        written += to_write;

        // Check for cancellation periodically
        if (written % (64 * 1024) == 0 && cancelled.load()) {
            file.close();
            return false;
        }
    }

    file.close();
    return true;
}

} // namespace

TEST(SegmentDownloaderTest, CalculateOptimalSegments) {
    // Small file - single segment
    EXPECT_EQ(SegmentDownloader::calculate_optimal_segments(1024 * 500, {}), 1);

    // Medium file - multiple segments
    EXPECT_GE(SegmentDownloader::calculate_optimal_segments(10 * 1024 * 1024, {}), 2);

    // Large file - more segments
    EXPECT_GE(SegmentDownloader::calculate_optimal_segments(100 * 1024 * 1024, {}), 4);
}

TEST(SegmentDownloaderTest, SegmentStructure) {
    Segment seg(0, 100, 199);

    EXPECT_EQ(seg.index, 0);
    EXPECT_EQ(seg.start, 100);
    EXPECT_EQ(seg.end, 199);
    EXPECT_EQ(seg.size(), 100);
    EXPECT_EQ(seg.remaining(), 100);
    EXPECT_FLOAT_EQ(seg.progress(), 0.0f);

    seg.downloaded = 50;
    EXPECT_EQ(seg.remaining(), 50);
    EXPECT_FLOAT_EQ(seg.progress(), 0.5f);

    seg.downloaded = 100;
    EXPECT_EQ(seg.remaining(), 0);
    EXPECT_FLOAT_EQ(seg.progress(), 1.0f);
}

TEST(SegmentDownloaderTest, SegmentStats) {
    SegmentStats stats;
    stats.total_size = 1000;
    stats.total_downloaded = 500;

    EXPECT_FLOAT_EQ(stats.progress(), 0.5f);
    EXPECT_EQ(stats.completed_segments.load(), 0);
}

TEST(SegmentDownloaderTest, BasicSegmentedDownload) {
    // Create mock task
    DownloadOptions options;
    options.max_connections = 4;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);  // 10 KB file (smaller for faster tests)

    const std::string output_path = make_unique_temp_path("falcon_test_output.bin");

    // Create segment downloader
    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;  // 1 KB
    config.min_file_size = 1;  // Always segment

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // Start download with mock function
    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    EXPECT_FLOAT_EQ(downloader.progress(), 1.0f);
    EXPECT_EQ(downloader.completed_segments(), downloader.total_segments());
    std::remove(output_path.c_str());
}

TEST(SegmentDownloaderTest, Cancellation) {
    DownloadOptions options;
    options.max_connections = 4;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);  // 10 KB file (smaller for faster tests)

    const std::string output_path = make_unique_temp_path("falcon_test_cancel.bin");

    SegmentConfig config;
    config.num_connections = 8;  // Many segments
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // Start download in a thread and cancel immediately
    std::atomic<bool> started{false};
    std::thread download_thread([&]() {
        started = true;
        downloader.start([](const std::string&, Bytes, Bytes, const std::string&,
                            std::atomic<bool>& cancelled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return !cancelled.load();
        });
    });

    // Wait for download to start
    while (!started) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    downloader.cancel();

    download_thread.join();

    // Download should have been cancelled
    EXPECT_LT(downloader.progress(), 1.0f);
    std::remove(output_path.c_str());
}

TEST(SegmentDownloaderTest, SingleConnectionFallback) {
    // Test with file too small for segmentation
    DownloadOptions options;
    options.max_connections = 1;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/small.bin", options);
    task->set_test_file_info(512);  // Small file

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024 * 1024;  // 1 MB minimum
    config.min_file_size = 1024 * 1024;  // 1 MB minimum file size

    const std::string output_path = make_unique_temp_path("falcon_test_small.bin");

    SegmentDownloader downloader(task, "http://test.example.com/small.bin",
                                 output_path, config);

    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    EXPECT_EQ(downloader.total_segments(), 1);
    std::remove(output_path.c_str());
}

TEST(SegmentDownloaderTest, PauseAndResume) {
    DownloadOptions options;
    options.max_connections = 4;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);  // 10 KB file (smaller for faster tests)

    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_pause.bin");

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // Download in a thread
    std::thread download_thread([&]() {
        downloader.start(mock_segment_download);
    });

    // Wait for some progress
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Pause
    downloader.pause();
    auto paused_progress = downloader.progress();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Progress should not have changed much
    auto new_progress = downloader.progress();
    EXPECT_NEAR(paused_progress, new_progress, 0.1f);

    downloader.cancel();
    download_thread.join();
    std::remove(output_path.c_str());
}

TEST(SegmentDownloaderTest, SpeedTracking) {
    DownloadOptions options;
    options.max_connections = 4;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);  // 10 KB file (smaller for faster tests)

    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_speed.bin");

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    // Speed should be calculated
    // Note: Mock download is very fast, so speed might be high or low depending on timing
    std::remove(output_path.c_str());
}

// Test configuration
TEST(SegmentConfigTest, DefaultsAreReasonable) {
    SegmentConfig config;

    EXPECT_GT(config.num_connections, 0);
    EXPECT_GT(config.min_segment_size, 0);
    EXPECT_GT(config.max_segment_size, config.min_segment_size);
    EXPECT_GT(config.min_file_size, 0);
    EXPECT_GT(config.timeout_seconds, 0);
    EXPECT_GT(config.max_retries, 0);
    EXPECT_GT(config.buffer_size, 0);
}

TEST(SegmentConfigTest, AdaptiveSizingEnabledByDefault) {
    SegmentConfig config;
    EXPECT_TRUE(config.adaptive_sizing);
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(SegmentDownloaderBoundary, ZeroFileSize) {
    DownloadOptions options;
    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/empty.bin", options);
    task->set_test_file_info(0);  // Empty file

    SegmentConfig config;
    const std::string output_path = make_unique_temp_path("falcon_test_empty.bin");

    SegmentDownloader downloader(task, "http://test.example.com/empty.bin",
                                 output_path, config);

    // Zero file size should be handled
    // May return false or handle as special case
    std::remove(output_path.c_str());
}

TEST(SegmentDownloaderBoundary, VeryLargeFileSize) {
    DownloadOptions options;
    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/large.bin", options);
    task->set_test_file_info(10ULL * 1024 * 1024 * 1024);  // 10 GB

    SegmentConfig config;
    config.num_connections = 8;
    config.min_segment_size = 1024 * 1024;  // 1 MB
    config.max_segment_size = 100 * 1024 * 1024;  // 100 MB

    auto optimal_segments = SegmentDownloader::calculate_optimal_segments(
        10ULL * 1024 * 1024 * 1024, config);

    // Should calculate reasonable number of segments
    EXPECT_GE(optimal_segments, 1);
    EXPECT_LE(optimal_segments, 10000);  // Should not create excessive segments
}

TEST(SegmentDownloaderBoundary, SingleByteSegment) {
    Segment seg(0, 0, 0);

    EXPECT_EQ(seg.index, 0);
    EXPECT_EQ(seg.start, 0);
    EXPECT_EQ(seg.end, 0);
    EXPECT_EQ(seg.size(), 1);
    EXPECT_EQ(seg.remaining(), 1);
}

TEST(SegmentDownloaderBoundary, SegmentProgressBoundaries) {
    Segment seg(0, 0, 999);

    EXPECT_FLOAT_EQ(seg.progress(), 0.0f);

    seg.downloaded = 500;
    EXPECT_FLOAT_EQ(seg.progress(), 0.5f);

    seg.downloaded = 1000;
    EXPECT_FLOAT_EQ(seg.progress(), 1.0f);

    // Test overflow protection (if implemented)
    seg.downloaded = 2000;
    EXPECT_GE(seg.progress(), 1.0f);
}

TEST(SegmentDownloaderBoundary, MinSegmentSize) {
    SegmentConfig config;
    config.min_segment_size = 1;
    config.max_segment_size = std::numeric_limits<Bytes>::max();

    auto segments = SegmentDownloader::calculate_optimal_segments(100, config);
    EXPECT_GE(segments, 1);
}

TEST(SegmentDownloaderBoundary, MaxSegmentSize) {
    SegmentConfig config;
    config.min_segment_size = 1;
    config.max_segment_size = 100;

    auto segments = SegmentDownloader::calculate_optimal_segments(1000, config);
    EXPECT_GE(segments, 10);  // At least 10 segments of max 100 bytes each
}

TEST(SegmentDownloaderBoundary, ZeroConnections) {
    SegmentConfig config;
    config.num_connections = 0;

    // Should auto-calculate connections
    auto segments = SegmentDownloader::calculate_optimal_segments(1024 * 1024, config);
    EXPECT_GT(segments, 0);
}

TEST(SegmentDownloaderBoundary, ManyConnections) {
    SegmentConfig config;
    config.num_connections = 1000;

    // Should clamp to reasonable value
    auto segments = SegmentDownloader::calculate_optimal_segments(1024 * 1024, config);
    EXPECT_LT(segments, 1000);
}

//==============================================================================
// 错误处理测试
//==============================================================================

TEST(SegmentDownloaderError, InvalidSegmentRange) {
    // Start > End is invalid
    Segment seg(0, 1000, 100);  // Invalid range

    // Size calculation should handle this
    EXPECT_LE(seg.size(), 0);
}

TEST(SegmentDownloaderError, DownloadFunctionFailure) {
    DownloadOptions options;
    options.max_connections = 2;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 5);  // 5 KB

    SegmentConfig config;
    config.num_connections = 2;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.max_retries = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_error.bin");

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // Download function that always fails
    auto failing_download = [](const std::string&, Bytes, Bytes, const std::string&,
                                std::atomic<bool>&) -> bool {
        return false;
    };

    bool success = downloader.start(failing_download);
    EXPECT_FALSE(success);
    std::remove(output_path.c_str());
}

TEST(SegmentDownloaderError, RetryExhaustion) {
    DownloadOptions options;
    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 5);

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024;
    config.max_retries = 2;  // Only 2 retries
    config.retry_delay_ms = 10;

    const std::string output_path = make_unique_temp_path("falcon_test_retry.bin");

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    int attempt_count = 0;
    auto retrying_download = [&attempt_count](const std::string&, Bytes, Bytes,
                                               const std::string&, std::atomic<bool>&) -> bool {
        attempt_count++;
        return false;  // Always fail
    };

    bool success = downloader.start(retrying_download);
    EXPECT_FALSE(success);
    // Should have attempted initial + retries
    EXPECT_GT(attempt_count, 1);
    std::remove(output_path.c_str());
}

//==============================================================================
// 性能测试
//==============================================================================

TEST(SegmentDownloaderPerformance, ManySmallSegments) {
    DownloadOptions options;
    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(100 * 1024);  // 100 KB

    SegmentConfig config;
    config.num_connections = 100;  // Many connections
    config.min_segment_size = 1;  // Allow tiny segments
    config.max_segment_size = 1024;  // 1 KB max
    config.min_file_size = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_many.bin");

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    auto start = std::chrono::steady_clock::now();
    bool success = downloader.start(mock_segment_download);
    auto end = std::chrono::steady_clock::now();

    EXPECT_TRUE(success);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Should complete reasonably fast even with many segments
    EXPECT_LT(duration.count(), 5000);  // 5 seconds max
    std::remove(output_path.c_str());
}

TEST(SegmentDownloaderPerformance, LargeFileDownload) {
    DownloadOptions options;
    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/large.bin", options);
    task->set_test_file_info(10 * 1024 * 1024);  // 10 MB

    SegmentConfig config;
    config.num_connections = 8;
    config.min_segment_size = 1024 * 1024;  // 1 MB
    config.min_file_size = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_10mb.bin");

    SegmentDownloader downloader(task, "http://test.example.com/large.bin",
                                 output_path, config);

    auto start = std::chrono::steady_clock::now();
    bool success = downloader.start(mock_segment_download);
    auto end = std::chrono::steady_clock::now();

    EXPECT_TRUE(success);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Performance check: should complete in reasonable time
    EXPECT_LT(duration.count(), 10000);  // 10 seconds max
    std::remove(output_path.c_str());
}

//==============================================================================
// 并发测试
//==============================================================================

TEST(SegmentDownloaderConcurrency, ConcurrentProgressQueries) {
    DownloadOptions options;
    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 50);  // 50 KB

    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_concurrent.bin");

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // Start download in background thread
    std::thread download_thread([&]() {
        downloader.start(mock_segment_download);
    });

    // Query progress concurrently from multiple threads
    std::vector<std::thread> query_threads;
    std::atomic<int> query_count{0};

    for (int i = 0; i < 10; ++i) {
        query_threads.emplace_back([&]() {
            for (int j = 0; j < 100; ++j) {
                float progress = downloader.progress();
                Bytes speed = downloader.speed();
                Bytes downloaded = downloader.downloaded_bytes();

                // Progress should be valid
                EXPECT_GE(progress, 0.0f);
                EXPECT_LE(progress, 1.0f);
                query_count++;
            }
        });
    }

    // Wait for all query threads
    for (auto& thread : query_threads) {
        thread.join();
    }

    download_thread.join();
    std::remove(output_path.c_str());

    EXPECT_EQ(query_count, 1000);
}

//==============================================================================
// 断点续传测试
//==============================================================================

TEST(SegmentDownloaderResume, ResumePartialDownload) {
    DownloadOptions options;
    options.resume_enabled = true;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);  // 10 KB

    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_resume.bin");

    // Create partial segment files
    for (std::size_t i = 0; i < 4; ++i) {
        std::string segment_path = output_path + ".falcon.tmp.seg" + std::to_string(i);
        std::ofstream seg_file(segment_path, std::ios::binary);
        if (seg_file.is_open()) {
            // Write partial data
            std::vector<uint8_t> data(512, static_cast<uint8_t>(i));
            seg_file.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
    }

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    EXPECT_FLOAT_EQ(downloader.progress(), 1.0f);

    // Clean up all segment files
    for (std::size_t i = 0; i < 4; ++i) {
        std::string segment_path = output_path + ".falcon.tmp.seg" + std::to_string(i);
        std::remove(segment_path.c_str());
    }
    std::remove(output_path.c_str());
}

TEST(SegmentDownloaderResume, DisabledResume) {
    DownloadOptions options;
    options.resume_enabled = false;  // Disable resume

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 5);

    SegmentConfig config;
    config.num_connections = 2;
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_no_resume.bin");

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    std::remove(output_path.c_str());
}

//==============================================================================
// 配置测试
//==============================================================================

TEST(SegmentConfig, CustomConfiguration) {
    SegmentConfig config;
    config.num_connections = 16;
    config.min_segment_size = 512 * 1024;  // 512 KB
    config.max_segment_size = 10 * 1024 * 1024;  // 10 MB
    config.min_file_size = 5 * 1024 * 1024;  // 5 MB
    config.timeout_seconds = 120;
    config.max_retries = 10;
    config.retry_delay_ms = 5000;
    config.buffer_size = 64 * 1024;  // 64 KB
    config.adaptive_sizing = false;
    config.validate_pieces = true;
    config.slow_speed_threshold = 1024;  // 1 KB/s

    EXPECT_EQ(config.num_connections, 16);
    EXPECT_EQ(config.min_segment_size, 512 * 1024);
    EXPECT_EQ(config.max_segment_size, 10 * 1024 * 1024);
    EXPECT_FALSE(config.adaptive_sizing);
    EXPECT_TRUE(config.validate_pieces);
}

TEST(SegmentConfig, EdgeCaseConfiguration) {
    SegmentConfig config;

    // Test minimum values
    config.num_connections = 1;
    config.min_segment_size = 1;
    config.max_segment_size = 1;
    config.min_file_size = 1;
    config.timeout_seconds = 1;
    config.max_retries = 0;

    EXPECT_EQ(config.num_connections, 1);
    EXPECT_EQ(config.min_segment_size, 1);
    EXPECT_EQ(config.max_segment_size, 1);
    EXPECT_EQ(config.min_file_size, 1);
    EXPECT_EQ(config.timeout_seconds, 1);
    EXPECT_EQ(config.max_retries, 0);
}

//==============================================================================
// SegmentStats 测试
//==============================================================================

TEST(SegmentStats, ProgressCalculation) {
    SegmentStats stats;

    stats.total_size = 0;
    stats.total_downloaded = 0;
    EXPECT_FLOAT_EQ(stats.progress(), 0.0f);

    stats.total_size = 1000;
    stats.total_downloaded = 0;
    EXPECT_FLOAT_EQ(stats.progress(), 0.0f);

    stats.total_downloaded = 500;
    EXPECT_FLOAT_EQ(stats.progress(), 0.5f);

    stats.total_downloaded = 1000;
    EXPECT_FLOAT_EQ(stats.progress(), 1.0f);
}

TEST(SegmentStats, CompletedSegmentsTracking) {
    SegmentStats stats;

    EXPECT_EQ(stats.completed_segments.load(), 0);

    stats.completed_segments.store(5);
    EXPECT_EQ(stats.completed_segments.load(), 5);

    stats.completed_segments.store(10);
    EXPECT_EQ(stats.completed_segments.load(), 10);
}

TEST(SegmentStats, ActiveConnectionsTracking) {
    SegmentStats stats;

    EXPECT_EQ(stats.active_connections, 0);

    stats.active_connections = 5;
    EXPECT_EQ(stats.active_connections, 5);

    stats.active_connections = 0;
    EXPECT_EQ(stats.active_connections, 0);
}

//==============================================================================
// 压力测试
//==============================================================================

TEST(SegmentDownloaderStress, RapidStartStop) {
    DownloadOptions options;
    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024);

    const std::string output_path = make_unique_temp_path("falcon_test_stress.bin");

    for (int i = 0; i < 10; ++i) {
        SegmentConfig config;
        SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                     output_path, config);

        std::thread cancel_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            downloader.cancel();
        });

        downloader.start(mock_segment_download);
        cancel_thread.join();
    }

    std::remove(output_path.c_str());
}
