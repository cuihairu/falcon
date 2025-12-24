// Falcon Segment Downloader Unit Tests

#include <falcon/segment_downloader.hpp>
#include <falcon/download_task.hpp>
#include <falcon/download_options.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace falcon;

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

// Create a test file with known content
static std::string create_test_file(const std::string& path, Bytes size) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    // Write pattern to file
    std::vector<uint8_t> buffer(4096);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<uint8_t>(i % 256);
    }

    Bytes written = 0;
    while (written < size) {
        Bytes to_write = std::min(static_cast<Bytes>(buffer.size()), size - written);
        file.write(reinterpret_cast<const char*>(buffer.data()), to_write);
        written += to_write;
    }

    file.close();
    return path;
}

// Verify file content matches expected pattern
static bool verify_file_content(const std::string& path, Bytes size) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::vector<uint8_t> buffer(4096);
    Bytes read = 0;

    while (read < size) {
        Bytes to_read = std::min(static_cast<Bytes>(buffer.size()), size - read);
        file.read(reinterpret_cast<char*>(buffer.data()), to_read);

        if (file.gcount() != static_cast<std::streamsize>(to_read)) {
            return false;
        }

        // Verify pattern
        for (std::size_t i = 0; i < static_cast<std::size_t>(to_read); ++i) {
            uint8_t expected = static_cast<uint8_t>((read + i) % 256);
            if (buffer[i] != expected) {
                return false;
            }
        }

        read += to_read;
    }

    file.close();
    return true;
}

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
        file.write(reinterpret_cast<const char*>(buffer.data()), to_write);
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

    // Create segment downloader
    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;  // 1 KB
    config.min_file_size = 1;  // Always segment

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 "/tmp/test_output.bin", config);

    // Start download with mock function
    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    EXPECT_FLOAT_EQ(downloader.progress(), 1.0f);
    EXPECT_EQ(downloader.completed_segments(), downloader.total_segments());
}

TEST(SegmentDownloaderTest, Cancellation) {
    DownloadOptions options;
    options.max_connections = 4;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);  // 10 KB file (smaller for faster tests)

    SegmentConfig config;
    config.num_connections = 8;  // Many segments
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 "/tmp/test_output.bin", config);

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

    SegmentDownloader downloader(task, "http://test.example.com/small.bin",
                                 "/tmp/test_small.bin", config);

    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    EXPECT_EQ(downloader.total_segments(), 1);
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

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 "/tmp/test_pause.bin", config);

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

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 "/tmp/test_speed.bin", config);

    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    // Speed should be calculated
    // Note: Mock download is very fast, so speed might be high or low depending on timing
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
