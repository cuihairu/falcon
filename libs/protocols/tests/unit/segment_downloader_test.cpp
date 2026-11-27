
// Falcon Segment Downloader Unit Tests

#include <falcon/protocols/segment_downloader.hpp>
#include <falcon/download_task.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/download_options.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
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
    std::atomic<bool>& cancelled,
    std::atomic<Bytes>& /*live_progress*/) {

    // Minimal delay for testing
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    if (cancelled.load()) {
        return false;
    }

    // Create segment file with pattern（app 追加：忠实模拟 206 服务器
    // 语义——既有前缀保留，只补 Range 剩余部分，段文件恰好多出
    // end - start + 1 字节）
    std::ofstream file(output_path, std::ios::binary | std::ios::app);
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

// 完成合并后的段文件清理必须连同遗留的 .resume 控制文件一起收走——
// 断点控制文件残留在成品旁，后续同名下载的恢复检测会误认断点
TEST(SegmentDownloaderTest, ResumeControlFileCleanedOnCompletion) {
    DownloadOptions options;
    options.max_connections = 2;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);

    const std::string output_path = make_unique_temp_path("falcon_test_resume_clean.bin");

    // 预置段 0 的遗留 .resume 文件（旧版本/异常中断的产物）
    const std::string resume_path = output_path + ".falcon.tmp.seg0.resume";
    {
        std::ofstream seed(resume_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(seed.is_open());
        seed << "falcon_resume_v1\nseg=0 0 5120 256\n";
    }

    SegmentConfig config;
    config.num_connections = 2;
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);
    EXPECT_TRUE(downloader.start(mock_segment_download));

    EXPECT_FALSE(std::filesystem::exists(resume_path));
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
                            std::atomic<bool>& cancelled,
                            std::atomic<Bytes>& /*live_progress*/) {
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

    // gate 语义 mock（用例局部 lambda：SegmentDownloadFunc 是
    // std::function，gate 原子可捕获，无需文件级状态重置）。回调开段
    // 文件后置 started 并阻塞在 gate——gate 前零写入、回调不返回。
    // pause() 不传播进在飞回调（回调只收 cancelled_），segment 记账只
    // 发生在回调返回之后（成功记账与 best-effort tellg 两路径皆是），
    // 因此 gate 挡住全部回调 ⇒ pause 前后不存在任何记账来源，进度位
    // 型恒同。旧版「sleep 锚 + 容差断言」在 macOS CI 慢调度下被在飞
    // worker 于观测窗内写完击穿（run 36012912035：0.625 → 1.0）。
    std::atomic<int> gate_started{0};
    std::atomic<bool> gate_open{false};

    auto gate_mock = [&gate_started, &gate_open](
                         const std::string& url, Bytes start, Bytes end,
                         const std::string& mock_output_path,
                         std::atomic<bool>& cancelled,
                         std::atomic<Bytes>& /*live_progress*/) -> bool {
        (void)url;
        std::ofstream file(mock_output_path, std::ios::binary | std::ios::app);
        if (!file.is_open()) {
            return false;
        }
        gate_started.fetch_add(1, std::memory_order_release);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!gate_open.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // gate 放行后一次性写完全部段（内容 = 段内偏移 i%256，与成品
        // 断言一致）；close 后 file_size 才对成功记账的精确尺寸校验可见
        const Bytes size = end - start + 1;
        std::vector<uint8_t> buffer(static_cast<std::size_t>(size));
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<uint8_t>(i % 256);
        }
        file.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        file.flush();
        file.close();
        return !cancelled.load();
    };

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    bool success = false;
    std::thread download_thread([&]() {
        success = downloader.start(gate_mock);
    });

    // 确定性事件锚：等全部段回调到达 gate（替代 sleep 时序锚）——此后
    // pause 前后无任何回调返回，零记账是全称确定的。start() 在本进程
    // 独立线程内先算分段再 spawn worker，total_segments() 须等首个
    // 到达者事件之后才稳定可读（直接读与分段计算竞速，读到 0 会让
    // 末尾成品校验的 total_size/seg_count 除零 SIGFPE——首轮压测
    // 96/100 失败即此，release/acquire 经 gate_started 原子对可见）
    for (int i = 0; i < 5000 && gate_started.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(gate_started.load(), 1);
    const auto seg_count = downloader.total_segments();
    for (int i = 0; i < 5000 &&
                    gate_started.load() < static_cast<int>(seg_count);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(gate_started.load(), static_cast<int>(seg_count));

    // pause 期间进度必须冻结：gate 挡住全部回调 ⇒ 两条记账路径均
    // 不可达 ⇒ 两次观测间进度恒 0.0（位型相同，等值断言而非容差）
    downloader.pause();
    const auto paused_progress = downloader.progress();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto new_progress = downloader.progress();
    EXPECT_FLOAT_EQ(paused_progress, new_progress);

    // 先 resume 再放行 gate：保证 gate 之后的全部记账发生在 resume
    // 之后（worker 冲出 gate 完成各段，组照常收口为成功）
    downloader.resume();
    gate_open.store(true, std::memory_order_release);

    download_thread.join();
    EXPECT_TRUE(success);
    EXPECT_FLOAT_EQ(downloader.progress(), 1.0f);
    EXPECT_EQ(downloader.completed_segments(), downloader.total_segments());

    // 成品逐字节一致（各段内容 = 段内偏移 i%256）。seg_count 守卫：
    // 锚点失败（start 未起 worker）时报告失败而非除零崩进程
    if (seg_count > 0) {
        std::ifstream result(output_path, std::ios::binary);
        ASSERT_TRUE(result.is_open());
        const Bytes total_size = 1024 * 10;
        const Bytes segment_size = total_size / seg_count;
        std::vector<uint8_t> expected(static_cast<std::size_t>(total_size));
        for (Bytes base = 0; base < total_size; base += segment_size) {
            for (Bytes i = 0; i < segment_size; ++i) {
                expected[static_cast<std::size_t>(base + i)] =
                    static_cast<uint8_t>(i % 256);
            }
        }
        std::vector<uint8_t> content(static_cast<std::size_t>(total_size));
        result.read(reinterpret_cast<char*>(content.data()),
                    static_cast<std::streamsize>(total_size));
        EXPECT_EQ(result.gcount(), static_cast<std::streamsize>(total_size));
        EXPECT_EQ(std::memcmp(content.data(), expected.data(),
                              static_cast<std::size_t>(total_size)),
                  0);
    }
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

    // 未知文件大小无法分段：start 必须快速失败（FileIOException → false），
    // 且不产出任何半成品
    EXPECT_FALSE(downloader.start(mock_segment_download));
    EXPECT_FALSE(std::filesystem::exists(output_path));
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
                                std::atomic<bool>&, std::atomic<Bytes>&) -> bool {
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
                                               const std::string&, std::atomic<bool>&,
                                               std::atomic<Bytes>&) -> bool {
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

    // Wall-clock thresholds are informational only: shared CI runners make
    // them inherently flaky (observed 31 s on GitHub runners). Correctness
    // and hang detection are covered by EXPECT_TRUE above and the ctest
    // timeout.
    std::cout << "[  PERF    ] ManySmallSegments duration: " << duration.count()
              << " ms" << std::endl;
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

    // Informational only — see ManySmallSegments for rationale.
    std::cout << "[  PERF    ] LargeFileDownload duration: " << duration.count()
              << " ms" << std::endl;
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
                static_cast<void>(speed);
                static_cast<void>(downloaded);

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
            seg_file.write(reinterpret_cast<const char*>(data.data()),
                           static_cast<std::streamsize>(data.size()));
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
// 完整性测试（静默损坏防护）
//==============================================================================

// 旧版缺陷场景：Range 被服务器忽略时整个文件被追加进段文件，段超尺寸
// 却被 min() clamp "祝福"为完成，损坏数据静默并入成品。现在超尺寸段
// 必须在启动检测时被删除重下（对旧版遗留的损坏段文件自愈）
TEST(SegmentDownloaderIntegrity, OversizedSegmentFileIsDiscardedAndHealed) {
    DownloadOptions options;
    options.resume_enabled = true;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);  // 10 KB → 4 段 × 2560 B

    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.max_retries = 2;

    const std::string output_path = make_unique_temp_path("falcon_test_integrity_oversized.bin");

    // 预置一个超尺寸段文件（3072 > 2560）模拟旧版损坏遗留，
    // 其余段正常预置部分数据
    const auto seed = [&output_path](std::size_t idx, std::size_t bytes) {
        std::ofstream seg(output_path + ".falcon.tmp.seg" + std::to_string(idx),
                          std::ios::binary);
        std::vector<char> data(bytes, static_cast<char>(idx));
        seg.write(data.data(), static_cast<std::streamsize>(data.size()));
    };
    seed(0, 3072);  // 超尺寸：不可信
    seed(1, 512);
    seed(2, 512);

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    bool success = downloader.start(mock_segment_download);

    EXPECT_TRUE(success);
    if (success) {
        // 成品尺寸必须精确等于文件大小（超尺寸段绝不能混入 merge）
        std::error_code ec;
        const auto merged = std::filesystem::file_size(output_path, ec);
        EXPECT_FALSE(ec);
        EXPECT_EQ(merged, 10240U);
    }
    std::remove(output_path.c_str());
}

// 传输中损坏场景：mock 模拟忽略续传位置的"撒谎服务器"——每次都按
// 段全长追加（有既有前缀时段文件超尺寸）。精确尺寸校验必须拦下首次
// 尝试、删除损坏段、从段头重试后恢复，最终成品尺寸精确
TEST(SegmentDownloaderIntegrity, CorruptAppendIsDetectedAndHealed) {
    DownloadOptions options;
    options.resume_enabled = true;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);

    SegmentConfig config;
    config.num_connections = 1;  // 单段覆盖整个文件，段全长 = end + 1
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.max_retries = 2;
    config.retry_delay_ms = 0;

    const std::string output_path = make_unique_temp_path("falcon_test_integrity_append.bin");

    // 预置部分数据，迫使首次尝试走续传路径
    {
        std::ofstream seg(output_path + ".falcon.tmp.seg0", std::ios::binary);
        std::vector<char> data(512, '\0');
        seg.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    // 模拟忽略续传位置的"撒谎服务器"：无论 start 调整到哪，都按段
    // 全长（end + 1，单段场景即文件全长）追加——首次尝试后段文件
    // 超尺寸（512 + 10240），精确尺寸校验必须拦下
    auto liar_download = [](const std::string&, Bytes /*start*/, Bytes end,
                            const std::string& path,
                            std::atomic<bool>& cancelled,
                            std::atomic<Bytes>& /*live_progress*/) -> bool {
        if (cancelled.load()) return false;
        const Bytes full_length = end + 1;
        std::ofstream file(path, std::ios::binary | std::ios::app);
        if (!file.is_open()) return false;
        std::vector<char> data(static_cast<std::size_t>(full_length), 'x');
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        return true;
    };

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    bool success = downloader.start(liar_download);

    EXPECT_TRUE(success);  // 损坏段被删除重下后应恢复
    if (success) {
        std::error_code ec;
        const auto merged = std::filesystem::file_size(output_path, ec);
        EXPECT_FALSE(ec);
        EXPECT_EQ(merged, 10240U);
    }
    std::remove(output_path.c_str());
}

// 短传场景：mock 每次只写 Range 的一半，尺寸永远到不了段长——
// 精确尺寸校验必须让下载失败（旧版 validate_pieces=false 时静默报成功）
TEST(SegmentDownloaderIntegrity, ShortTransferIsRejected) {
    DownloadOptions options;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 5);

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.max_retries = 1;
    config.retry_delay_ms = 0;

    const std::string output_path = make_unique_temp_path("falcon_test_integrity_short.bin");

    auto half_download = [](const std::string&, Bytes start, Bytes end,
                            const std::string& path,
                            std::atomic<bool>& cancelled,
                            std::atomic<Bytes>& /*live_progress*/) -> bool {
        if (cancelled.load()) return false;
        const Bytes size = (end - start + 1) / 2;
        if (size == 0) return false;
        std::ofstream file(path, std::ios::binary | std::ios::app);
        if (!file.is_open()) return false;
        std::vector<char> data(static_cast<std::size_t>(size), 'y');
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        return true;
    };

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    bool success = downloader.start(half_download);

    EXPECT_FALSE(success);  // 短传必须失败，不得静默出成品
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
    config.slow_speed_threshold = 1024;  // 1 KB/s

    EXPECT_EQ(config.num_connections, 16);
    EXPECT_EQ(config.min_segment_size, 512 * 1024);
    EXPECT_EQ(config.max_segment_size, 10 * 1024 * 1024);
    EXPECT_FALSE(config.adaptive_sizing);
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
