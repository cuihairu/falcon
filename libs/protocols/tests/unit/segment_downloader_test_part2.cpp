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

//==============================================================================
// 覆盖率批次 N：start 门禁 / 等分策略 / 续传完成态 / 暂停循环 / 合并闸门
//==============================================================================

namespace {

// 带超时轮询原子标志（防环境抖动挂死）
template <typename Pred>
static bool wait_for_cond(Pred&& pred,
                          std::chrono::milliseconds timeout =
                              std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// 预置段文件：写入 bytes 个重复字节
static void seed_segment_file(const std::string& output_path, std::size_t idx,
                              std::size_t bytes, char fill) {
    std::ofstream seg(output_path + ".falcon.tmp.seg" + std::to_string(idx),
                      std::ios::binary);
    ASSERT_TRUE(seg.is_open());
    std::string data(bytes, fill);
    seg.write(data.data(), static_cast<std::streamsize>(data.size()));
}

}  // namespace

// start() 运行中重入：直接拒绝（不抛错、不重置正在进行的下载）
TEST(SegmentDownloaderEdges, StartRejectedWhileRunning) {
    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    task->set_test_file_info(1024 * 10);

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024;
    config.min_file_size = 1;

    const std::string output_path = make_unique_temp_path("falcon_test_seg_reentry.bin");
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // seg0 的下载阻塞在栅栏上，让首个 start 保持运行态
    std::promise<void> gate;
    std::shared_future<void> gate_future = gate.get_future().share();
    std::atomic<bool> first_result{true};

    auto gated_download = [&](const std::string& url, Bytes start, Bytes end,
                              const std::string& path,
                              std::atomic<bool>& cancelled,
                                  std::atomic<Bytes>& live_progress) -> bool {
        if (gate_future.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready) {
            return false;
        }
        return mock_segment_download(url, start, end, path, cancelled, live_progress);
    };

    std::thread download_thread(
        [&]() { first_result = downloader.start(gated_download); });
    ASSERT_TRUE(wait_for_cond([&]() { return downloader.is_active(); }));

    // 运行中重入 start：false
    EXPECT_FALSE(downloader.start(gated_download));

    gate.set_value();
    download_thread.join();
    EXPECT_TRUE(first_result.load());
    std::remove(output_path.c_str());
}

// 下载函数抛异常：worker 段级 catch 记录 last_error 后重试，
// 恢复的段正常完成（异常路径不吞掉整次下载）
TEST(SegmentDownloaderEdges, ThrowingDownloadFuncRetriedThenSucceeds) {
    DownloadOptions options;
    options.max_connections = 2;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);

    const std::string output_path = make_unique_temp_path("falcon_test_seg_throw.bin");
    SegmentConfig config;
    config.num_connections = 2;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.max_retries = 3;

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // 前 2 次调用直接抛异常（覆盖 worker 段级 catch），其后恢复正常
    std::atomic<int> calls{0};
    auto throwing_then_ok = [&](const std::string& url, Bytes start, Bytes end,
                                const std::string& path,
                                std::atomic<bool>& cancelled,
                                    std::atomic<Bytes>& live_progress) -> bool {
        if (calls.fetch_add(1) < 2) {
            throw std::runtime_error("transient seg explode");
        }
        return mock_segment_download(url, start, end, path, cancelled, live_progress);
    };

    EXPECT_TRUE(downloader.start(throwing_then_ok));
    EXPECT_GE(calls.load(), 3);  // 异常确实发生过且被重试吸收
    EXPECT_EQ(downloader.completed_segments(), downloader.total_segments());

    // 成品完整（10240 字节且逐字节模式正确）
    std::ifstream out(output_path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out.seekg(0, std::ios::end);
    EXPECT_EQ(static_cast<size_t>(out.tellg()), 1024u * 10);
    out.close();
    std::remove(output_path.c_str());
}

// 输出父目录被普通文件阻塞：start 内 create_directories 抛出 →
// FileIOException → 外层收口返回 false，且从未发起任何段下载
TEST(SegmentDownloaderEdges, CreateDirectoryFailureFailsFast) {
    DownloadOptions options;
    options.max_connections = 2;
    options.create_directory = true;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);

    // 先造一个普通文件，让 output 的 parent_path 恰好落在它身上
    const std::string blocker_dir = make_unique_temp_path("falcon_blocker");
    const std::string blocker = blocker_dir + "_file";
    {
        std::ofstream f(blocker, std::ios::binary);
        f << "i am a file, not a directory";
    }
    const std::string output_path = blocker + "/sub/out.bin";

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, SegmentConfig{});

    std::atomic<int> calls{0};
    auto counting_download = [&](const std::string& url, Bytes start, Bytes end,
                                 const std::string& path,
                                 std::atomic<bool>& cancelled,
                                     std::atomic<Bytes>& live_progress) -> bool {
        ++calls;
        return mock_segment_download(url, start, end, path, cancelled, live_progress);
    };

    EXPECT_FALSE(downloader.start(counting_download));
    EXPECT_EQ(calls.load(), 0);  // 目录创建失败先于任何段下载
    EXPECT_EQ(downloader.completed_segments(), 0u);
    std::remove(blocker.c_str());
}

// start() 前已取消：直接拒绝
TEST(SegmentDownloaderEdges, StartRejectedAfterCancel) {
    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    task->set_test_file_info(1024 * 5);

    const std::string output_path = make_unique_temp_path("falcon_test_seg_precancel.bin");
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, SegmentConfig{});

    downloader.cancel();
    EXPECT_FALSE(downloader.start(mock_segment_download));
    EXPECT_FALSE(std::filesystem::exists(output_path));
    std::remove(output_path.c_str());
}

// 等分策略（adaptive_sizing=false）：段边界等分、末段带走余数
TEST(SegmentDownloaderEdges, EqualSizedSegmentsWithRemainder) {
    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    const Bytes file_size = 1024 * 10 + 3;  // 非整除，末段 +3
    task->set_test_file_info(file_size);

    SegmentConfig config;
    config.num_connections = 2;
    config.min_segment_size = 512;
    config.min_file_size = 1;
    config.adaptive_sizing = false;

    const std::string output_path = make_unique_temp_path("falcon_test_seg_equal.bin");
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    EXPECT_TRUE(downloader.start(mock_segment_download));
    EXPECT_EQ(downloader.total_segments(), 2u);

    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(output_path, ec), file_size);
    std::remove(output_path.c_str());
}

// 续传：恰好整段的既有段文件被标记完成，其余段照常补齐
TEST(SegmentDownloaderEdges, ResumeMarksFullSegmentComplete) {
    DownloadOptions options;
    options.resume_enabled = true;

    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);

    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.adaptive_sizing = false;  // 等分：4 × 2560

    const std::string output_path = make_unique_temp_path("falcon_test_seg_resume_full.bin");
    seed_segment_file(output_path, 0, 2560, '\xAA');  // 段 0 恰好整段
    seed_segment_file(output_path, 1, 512, '\xBB');   // 其余部分

    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    EXPECT_TRUE(downloader.start(mock_segment_download));

    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(output_path, ec), 10240U);
    std::remove(output_path.c_str());
}

// 续传：全部段已完成 → 不触达下载函数，直接合并出成品
TEST(SegmentDownloaderEdges, ResumeAllCompleteSkipsDownload) {
    DownloadOptions options;
    options.resume_enabled = true;

    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", options);
    task->set_test_file_info(1024 * 10);

    SegmentConfig config;
    config.num_connections = 4;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.adaptive_sizing = false;  // 等分：4 × 2560

    const std::string output_path = make_unique_temp_path("falcon_test_seg_resume_all.bin");
    for (std::size_t i = 0; i < 4; ++i) {
        seed_segment_file(output_path, i, 2560, static_cast<char>(i + 1));
    }

    std::atomic<bool> invoked{false};
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    EXPECT_TRUE(downloader.start([&](const std::string&, Bytes, Bytes,
                                     const std::string&,
                                     std::atomic<bool>&, std::atomic<Bytes>&) {
        invoked = true;
        return false;
    }));
    EXPECT_FALSE(invoked.load());  // 一次网络调用都不该发生

    // 合并次序校验：成品 = 各段按序拼接
    std::ifstream out(output_path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    std::string merged(10240, '\0');
    out.read(merged.data(), static_cast<std::streamsize>(merged.size()));
    for (std::size_t seg = 0; seg < 4; ++seg) {
        for (std::size_t off = 0; off < 2560; ++off) {
            ASSERT_EQ(merged[seg * 2560 + off], static_cast<char>(seg + 1))
                << "seg" << seg << " offset " << off;
        }
    }
    std::remove(output_path.c_str());
}

// 暂停阻塞 worker 循环与监控线程：is_active=false、resume 后恢复推进
TEST(SegmentDownloaderEdges, PauseBlocksWorkerAndMonitorLoops) {
    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    task->set_test_file_info(1024 * 10);

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.adaptive_sizing = false;  // 2 × 5120

    const std::string output_path = make_unique_temp_path("falcon_test_seg_pause.bin");
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    std::atomic<bool> paused_flag{false};
    std::atomic<bool> first_result{true};

    auto pause_on_first = [&](const std::string& url, Bytes start, Bytes end,
                              const std::string& path,
                              std::atomic<bool>& cancelled,
                                  std::atomic<Bytes>& live_progress) -> bool {
        const bool ok = mock_segment_download(url, start, end, path, cancelled, live_progress);
        if (ok && !paused_flag.exchange(true)) {
            downloader.pause();  // seg0 完成后暂停：worker 循环停在暂停分支
        }
        return ok;
    };

    std::thread download_thread(
        [&]() { first_result = downloader.start(pause_on_first); });
    ASSERT_TRUE(wait_for_cond([&]() { return paused_flag.load(); }));

    // 暂停态：is_active=false；只读查询照常可用
    EXPECT_FALSE(downloader.is_active());
    EXPECT_EQ(downloader.total_bytes(), 10240U);
    EXPECT_NO_FATAL_FAILURE((void)downloader.active_connections());

    // 持续暂停 >1s：让监控线程走到暂停分支（其节拍为 1s）
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));

    downloader.resume();
    download_thread.join();

    EXPECT_TRUE(first_result.load());
    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(output_path, ec), 10240U);
    std::remove(output_path.c_str());
}

// 暂停命中段内重试循环：失败重试在暂停期间挂起，resume 后从断点续上
TEST(SegmentDownloaderEdges, PauseBlocksSegmentRetryLoop) {
    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    task->set_test_file_info(1024 * 5);

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.adaptive_sizing = false;  // 单段 5120
    config.max_retries = 2;
    config.retry_delay_ms = 10;

    const std::string output_path = make_unique_temp_path("falcon_test_seg_pause_retry.bin");
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    std::atomic<int> calls{0};
    std::atomic<bool> paused_flag{false};
    std::atomic<bool> first_result{true};

    auto fail_then_pause = [&](const std::string& url, Bytes start, Bytes end,
                               const std::string& path,
                               std::atomic<bool>& cancelled,
                                   std::atomic<Bytes>& live_progress) -> bool {
        if (calls++ == 0) {
            // 首次：半量写入后失败，并把下载置入暂停
            std::ofstream f(path, std::ios::binary | std::ios::app);
            if (!f.is_open()) return false;
            const Bytes half = (end - start + 1) / 2;
            std::string data(static_cast<std::size_t>(half), 'a');
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
            f.close();
            downloader.pause();
            paused_flag = true;
            return false;
        }
        return mock_segment_download(url, start, end, path, cancelled, live_progress);
    };

    std::thread download_thread(
        [&]() { first_result = downloader.start(fail_then_pause); });
    ASSERT_TRUE(wait_for_cond([&]() { return paused_flag.load(); }));

    // 暂停保持 300ms：重试循环在暂停分支空转（100ms 节拍 ≥2 轮）
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    downloader.resume();
    download_thread.join();

    EXPECT_TRUE(first_result.load());
    EXPECT_GE(calls.load(), 2);  // 至少一次失败 + 一次续传成功
    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(output_path, ec), 5120U);
    std::remove(output_path.c_str());
}

// 返回 false 但段文件恰好整段：best-effort 记账识别完成态，不浪费重试
TEST(SegmentDownloaderEdges, FalseReturnWithExactSizeCompletes) {
    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    task->set_test_file_info(1024 * 5);

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.adaptive_sizing = false;  // 单段 5120
    config.max_retries = 3;

    const std::string output_path =
        make_unique_temp_path("falcon_test_seg_false_exact.bin");
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // 数据齐全但返回 false：精确尺寸记账必须判定完成并跳出重试
    auto complete_but_false = [](const std::string&, Bytes start, Bytes end,
                                 const std::string& path,
                                 std::atomic<bool>& cancelled,
                                 std::atomic<Bytes>& /*live_progress*/) -> bool {
        if (cancelled.load()) return false;
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f.is_open()) return false;
        const Bytes size = end - start + 1;
        std::string data(static_cast<std::size_t>(size), 'z');
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        return false;
    };

    EXPECT_TRUE(downloader.start(complete_but_false));

    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(output_path, ec), 5120U);
    std::remove(output_path.c_str());
}

// merge 前逐段闸门：已完成段在合并前被外部篡改（尺寸漂移）→ 拒绝出成品
TEST(SegmentDownloaderEdges, MergeGateRejectsDriftedSegment) {
    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    task->set_test_file_info(1024 * 10);

    SegmentConfig config;
    config.num_connections = 2;  // 2 段（1 连接时 calculate_optimal_segments 返回 1 段）
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.adaptive_sizing = false;  // 等分 2 × 5120
    config.retry_delay_ms = 10;

    const std::string output_path = make_unique_temp_path("falcon_test_seg_gate.bin");
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // 确定性时序：等 seg0「完成记账」（completed_segments 原子计数）
    // 后实施篡改——用完成计数而非段文件尺寸：macOS CI 红面根因是
    // 篡改落在「mock 写完（尺寸可见）→ download_segment 尺寸校验读」
    // 的微秒窗，校验得 100≠5120 触发重试、重试从断点续传恰好补齐
    // 5120，篡改被自愈、merge 闸门合法通过（start true + 成品存在，
    // 与红面 Actual true/Expected false 完全吻合）。完成记账后段不
    // 再进入任何重试路径，篡改严格后置无自愈窗口
    auto corrupt_after_seg0_complete = [&](const std::string& url, Bytes start,
                                           Bytes end, const std::string& path,
                                           std::atomic<bool>& cancelled,
                                               std::atomic<Bytes>& live_progress) -> bool {
        const std::string seg0_path = output_path + ".falcon.tmp.seg0";
        if (path != seg0_path) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (downloader.completed_segments() < 1) {
                if (std::chrono::steady_clock::now() > deadline ||
                    cancelled.load()) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            // 篡改已完成的 seg0：trunc 后写 100 字节
            std::ofstream f(seg0_path, std::ios::binary | std::ios::trunc);
            std::string drift(100, 'X');
            f.write(drift.data(), static_cast<std::streamsize>(drift.size()));
            f.close();
        }
        return mock_segment_download(url, start, end, path, cancelled, live_progress);
    };

    EXPECT_FALSE(downloader.start(corrupt_after_seg0_complete));
    EXPECT_FALSE(std::filesystem::exists(output_path));  // 绝不产出坏成品
    std::remove(output_path.c_str());
}

// 输出路径是已存在目录：段文件与合并临时文件都正常（是兄弟路径），
// 最终 rename(文件 → 目录) 失败 → 失败收尾而非假报完成
TEST(SegmentDownloaderEdges, RenameFailureWhenOutputIsDirectory) {
    namespace fs = std::filesystem;

    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    task->set_test_file_info(1024 * 5);

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.adaptive_sizing = false;

    const auto dir = fs::temp_directory_path() /
                     ("falcon_test_dir_out_" +
                      std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch().count()));
    fs::create_directories(dir);
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 dir.string(), config);

    EXPECT_FALSE(downloader.start(mock_segment_download));
    EXPECT_FALSE(fs::is_regular_file(dir));  // 目录没有被成品顶替

    // 段文件/临时文件是 dir 的兄弟路径，析构清理段文件后移除目录
    std::error_code ec;
    fs::remove(dir, ec);
    SUCCEED();
}

// 传输中 cancel：worker 与尚存活的监控线程都由 cancel 收尸
// （此后 start 收尾路径的二次 join 必须跳过已收割的线程）
TEST(SegmentDownloaderEdges, CancelMidFlightJoinsLiveMonitor) {
    auto task = std::make_shared<MockDownloadTask>(
        1, "http://test.example.com/file.bin", DownloadOptions{});
    task->set_test_file_info(1024 * 10);

    SegmentConfig config;
    config.num_connections = 1;
    config.min_segment_size = 1024;
    config.min_file_size = 1;
    config.adaptive_sizing = false;

    const std::string output_path = make_unique_temp_path("falcon_test_seg_midcancel.bin");
    SegmentDownloader downloader(task, "http://test.example.com/file.bin",
                                 output_path, config);

    // mock 堵在栅栏上并响应取消：cancel 时 worker 立即退出而非等满超时
    std::promise<void> gate;
    std::shared_future<void> gate_future = gate.get_future().share();
    std::atomic<bool> first_result{true};

    auto cancellable_gated = [&](const std::string& url, Bytes start, Bytes end,
                                 const std::string& path,
                                 std::atomic<bool>& cancelled,
                                     std::atomic<Bytes>& live_progress) -> bool {
        while (gate_future.wait_for(std::chrono::milliseconds(20)) !=
               std::future_status::ready) {
            if (cancelled.load()) {
                return false;
            }
        }
        return mock_segment_download(url, start, end, path, cancelled, live_progress);
    };

    std::thread download_thread(
        [&]() { first_result = downloader.start(cancellable_gated); });
    ASSERT_TRUE(wait_for_cond([&]() { return downloader.is_active(); }));

    downloader.cancel();  // join worker + 收割存活的监控线程
    download_thread.join();

    EXPECT_FALSE(first_result.load());
    gate.set_value();  // 兜底放行（线程已退出，无害）
    std::remove(output_path.c_str());
}

//==============================================================================
// 覆盖率批次 Y:merge 临时文件冲突与重试边界的兄弟段取消
//==============================================================================

// merge 的临时文件路径(<out>.falcon.tmp.merge)被目录占用时
// ofstream 打开失败 → merge 抛 FileIOException,被 start() 顶层
// catch 收为 false——两段 completed 且无 failed/cancelled 时,该
// false 只能来自 merge 失败,成品绝不拼出。
// 两阶段构造:阶段 1 只完成段 0(真实段文件留存);阶段 2 恢复加载
// 段 0 直接置 completed,段 1 由 mock 补齐 → 全部完成后 merge 撞目录
TEST(SegmentDownloaderBoundary, MergeTempOccupiedByDirectoryThrows) {
    const std::string url = "http://test.example.com/merge.bin";
    const std::string output_path = make_unique_temp_path("falcon_merge_occ.bin");

    auto make_config = [] {
        SegmentConfig config;
        config.num_connections = 2;
        config.min_segment_size = 1;
        config.min_file_size = 1;
        config.adaptive_sizing = false;  // 等分:64B → 两段各 32B
        config.max_retries = 0;
        config.retry_delay_ms = 0;
        return config;
    };

    // 阶段 1:段 0 满尺寸落盘,段 1 失败
    {
        DownloadOptions options;
        options.resume_enabled = true;
        auto task = std::make_shared<MockDownloadTask>(1, url, options);
        task->set_test_file_info(64);
        SegmentDownloader downloader(task, url, output_path, make_config());
        EXPECT_FALSE(downloader.start(
            [](const std::string&, Bytes start, Bytes end,
               const std::string& seg_path, std::atomic<bool>&,
               std::atomic<Bytes>&) {
                if (start != 0) return false;  // 段 1 失败
                std::ofstream f(seg_path, std::ios::binary);
                const Bytes n = end - start + 1;
                const std::string payload(n, 'M');
                f.write(payload.data(), static_cast<std::streamsize>(n));
                return f.good();
            }));
    }

    // merge 临时路径被目录占用
    std::filesystem::create_directories(output_path + ".falcon.tmp.merge");

    // 阶段 2:恢复加载段 0(满尺寸 → 直接 completed),段 1 补齐后
    // merge → ofstream 打开目录失败
    {
        DownloadOptions options;
        options.resume_enabled = true;
        auto task = std::make_shared<MockDownloadTask>(2, url, options);
        task->set_test_file_info(64);
        SegmentDownloader downloader(task, url, output_path, make_config());
        // merge 的 FileIOException 被 start() 的顶层 catch 吞为返回
        // false——但两段均已 completed 且无 failed/cancelled,start()
        // 返回 false 的唯一路径就是 merge 抛出(521)
        EXPECT_FALSE(downloader.start(mock_segment_download));
        EXPECT_EQ(downloader.completed_segments(), 2u);
        EXPECT_FALSE(std::filesystem::exists(output_path));  // 成品未拼出
    }

    std::error_code ec;
    std::filesystem::remove_all(output_path + ".falcon.tmp.merge", ec);
    for (int i : {0, 1}) {
        std::filesystem::remove(output_path + ".falcon.tmp.seg" + std::to_string(i), ec);
    }
    std::filesystem::remove(output_path, ec);
}

// 一段即时失败(重试耗尽 → 置全局 cancelled),另一段晚失败——其
// 重试边界先见 cancelled 即 break,不再进入失败记账路径
TEST(SegmentDownloaderBoundary, RetryBoundaryRespectsSiblingCancellation) {
    DownloadOptions options;

    auto task = std::make_shared<MockDownloadTask>(1, "http://test.example.com/race.bin", options);
    task->set_test_file_info(64);  // 2 段 × 32B

    const std::string output_path = make_unique_temp_path("falcon_retry_boundary.bin");

    SegmentConfig config;
    config.num_connections = 2;
    config.min_segment_size = 1;
    config.min_file_size = 1;
    config.adaptive_sizing = false;
    config.max_retries = 0;      // 一次失败即记账
    config.retry_delay_ms = 0;

    SegmentDownloader downloader(task, "http://test.example.com/race.bin",
                                 output_path, config);

    // 段 0(start=0)立即失败:重试边界记账 + 置全局 cancelled;
    // 段 1(start=32)延迟失败:mock 返回后的 cancelled 检查(450-451)
    // 命中 → 立即 break,绝不空转重试
    EXPECT_FALSE(downloader.start([](const std::string&, Bytes start, Bytes,
                                     const std::string&, std::atomic<bool>&,
                                     std::atomic<Bytes>&) {
        if (start == 0) return false;  // 即时失败
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return false;  // 段 0 记账后才返回,兄弟段取消即时生效
    }));
    EXPECT_LT(downloader.progress(), 1.0f);

    std::error_code ec;
    std::filesystem::remove(output_path, ec);
}
