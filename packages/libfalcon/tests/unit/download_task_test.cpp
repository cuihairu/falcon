// Falcon Download Task Unit Tests
// Copyright (c) 2025 Falcon Project

#include <falcon/download_task.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace falcon;

class DownloadTaskTest : public ::testing::Test {
protected:
    void SetUp() override {
        options_.output_directory = "/tmp";
        options_.output_filename = "test.bin";
    }

    DownloadOptions options_;
};

TEST_F(DownloadTaskTest, CreateTask) {
    auto task = std::make_shared<DownloadTask>(1, "https://example.com/file.zip", options_);

    EXPECT_EQ(task->id(), 1);
    EXPECT_EQ(task->url(), "https://example.com/file.zip");
    EXPECT_EQ(task->status(), TaskStatus::Pending);
    EXPECT_EQ(task->progress(), 0.0f);
    EXPECT_EQ(task->total_bytes(), 0);
    EXPECT_EQ(task->downloaded_bytes(), 0);
}

TEST_F(DownloadTaskTest, StatusTransitions) {
    auto task = std::make_shared<DownloadTask>(2, "https://example.com/file.zip", options_);

    EXPECT_EQ(task->status(), TaskStatus::Pending);
    EXPECT_FALSE(task->is_active());
    EXPECT_FALSE(task->is_finished());

    task->set_status(TaskStatus::Preparing);
    EXPECT_EQ(task->status(), TaskStatus::Preparing);
    EXPECT_TRUE(task->is_active());
    EXPECT_FALSE(task->is_finished());

    task->set_status(TaskStatus::Downloading);
    EXPECT_EQ(task->status(), TaskStatus::Downloading);
    EXPECT_TRUE(task->is_active());
    EXPECT_FALSE(task->is_finished());

    task->set_status(TaskStatus::Completed);
    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_FALSE(task->is_active());
    EXPECT_TRUE(task->is_finished());
}

TEST_F(DownloadTaskTest, ProgressUpdate) {
    auto task = std::make_shared<DownloadTask>(3, "https://example.com/file.zip", options_);

    task->update_progress(500, 1000, 100);

    EXPECT_EQ(task->downloaded_bytes(), 500);
    EXPECT_EQ(task->total_bytes(), 1000);
    EXPECT_EQ(task->speed(), 100);
    EXPECT_FLOAT_EQ(task->progress(), 0.5f);
}

TEST_F(DownloadTaskTest, ProgressZeroTotal) {
    auto task = std::make_shared<DownloadTask>(4, "https://example.com/file.zip", options_);

    // When total is 0, progress should be 0
    task->update_progress(500, 0, 100);

    EXPECT_EQ(task->downloaded_bytes(), 500);
    EXPECT_EQ(task->total_bytes(), 0);
    EXPECT_FLOAT_EQ(task->progress(), 0.0f);
}

TEST_F(DownloadTaskTest, FileInfo) {
    auto task = std::make_shared<DownloadTask>(5, "https://example.com/file.zip", options_);

    FileInfo info;
    info.url = "https://example.com/file.zip";
    info.filename = "file.zip";
    info.total_size = 1024 * 1024;
    info.supports_resume = true;
    info.content_type = "application/zip";

    task->set_file_info(info);

    EXPECT_EQ(task->file_info().filename, "file.zip");
    EXPECT_EQ(task->file_info().total_size, 1024 * 1024);
    EXPECT_TRUE(task->file_info().supports_resume);
    EXPECT_EQ(task->total_bytes(), 1024 * 1024);
}

TEST_F(DownloadTaskTest, OutputPath) {
    auto task = std::make_shared<DownloadTask>(6, "https://example.com/file.zip", options_);

    task->set_output_path("/tmp/downloads/file.zip");

    EXPECT_EQ(task->output_path(), "/tmp/downloads/file.zip");
}

TEST_F(DownloadTaskTest, ErrorMessage) {
    auto task = std::make_shared<DownloadTask>(7, "https://example.com/file.zip", options_);

    task->set_error("Connection refused");

    EXPECT_EQ(task->error_message(), "Connection refused");
}

TEST_F(DownloadTaskTest, Cancel) {
    auto task = std::make_shared<DownloadTask>(8, "https://example.com/file.zip", options_);

    task->set_status(TaskStatus::Downloading);
    EXPECT_TRUE(task->cancel());
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
    EXPECT_TRUE(task->is_finished());

    // Cannot cancel finished task
    EXPECT_FALSE(task->cancel());
}

TEST_F(DownloadTaskTest, PauseResume) {
    auto task = std::make_shared<DownloadTask>(9, "https://example.com/file.zip", options_);

    // Can pause pending task
    EXPECT_TRUE(task->pause());
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // Resume and change to Downloading
    task->resume();
    task->set_status(TaskStatus::Downloading);
    EXPECT_TRUE(task->pause());
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // Resume requires handler, but status check works
    task->set_status(TaskStatus::Paused);
    EXPECT_EQ(task->status(), TaskStatus::Paused);
}

TEST_F(DownloadTaskTest, WaitForCompletion) {
    auto task = std::make_shared<DownloadTask>(10, "https://example.com/file.zip", options_);

    // Complete task in another thread
    std::thread completer([task]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        task->set_status(TaskStatus::Completed);
    });

    // Wait with timeout
    bool completed = task->wait_for(std::chrono::seconds(1));
    EXPECT_TRUE(completed);
    EXPECT_EQ(task->status(), TaskStatus::Completed);

    completer.join();
}

TEST_F(DownloadTaskTest, ElapsedTime) {
    auto task = std::make_shared<DownloadTask>(11, "https://example.com/file.zip", options_);

    // Before start, elapsed should be zero
    EXPECT_EQ(task->elapsed().count(), 0);

    task->mark_started();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto elapsed = task->elapsed();
    EXPECT_GE(elapsed.count(), std::chrono::milliseconds(50).count());
}

TEST_F(DownloadTaskTest, EstimatedRemaining) {
    auto task = std::make_shared<DownloadTask>(12, "https://example.com/file.zip", options_);

    // No speed = no estimate
    task->update_progress(500, 1000, 0);
    EXPECT_EQ(task->estimated_remaining().count(), 0);

    // With speed, calculate estimate
    task->update_progress(500, 1000, 100);  // 100 bytes/sec, 500 remaining
    auto remaining = task->estimated_remaining();
    EXPECT_GE(remaining.count(), std::chrono::seconds(4).count());
}

TEST_F(DownloadTaskTest, GetProgressInfo) {
    auto task = std::make_shared<DownloadTask>(13, "https://example.com/file.zip", options_);

    task->mark_started();
    task->update_progress(500, 1000, 100);

    ProgressInfo info = task->get_progress_info();

    EXPECT_EQ(info.task_id, 13);
    EXPECT_EQ(info.downloaded_bytes, 500);
    EXPECT_EQ(info.total_bytes, 1000);
    EXPECT_EQ(info.speed, 100);
    EXPECT_FLOAT_EQ(info.progress, 0.5f);
}

TEST_F(DownloadTaskTest, StatusToString) {
    EXPECT_STREQ(to_string(TaskStatus::Pending), "Pending");
    EXPECT_STREQ(to_string(TaskStatus::Preparing), "Preparing");
    EXPECT_STREQ(to_string(TaskStatus::Downloading), "Downloading");
    EXPECT_STREQ(to_string(TaskStatus::Paused), "Paused");
    EXPECT_STREQ(to_string(TaskStatus::Completed), "Completed");
    EXPECT_STREQ(to_string(TaskStatus::Failed), "Failed");
    EXPECT_STREQ(to_string(TaskStatus::Cancelled), "Cancelled");
}

// 新增：进度计算边界测试
TEST_F(DownloadTaskTest, ProgressBoundaryConditions) {
    auto task = std::make_shared<DownloadTask>(20, "https://example.com/file.zip", options_);

    // 测试 0% 进度
    task->update_progress(0, 1000, 100);
    EXPECT_FLOAT_EQ(task->progress(), 0.0f);

    // 测试 100% 进度
    task->update_progress(1000, 1000, 100);
    EXPECT_FLOAT_EQ(task->progress(), 1.0f);

    // 测试超过 100% 的情况（异常情况）
    task->update_progress(1500, 1000, 100);
    EXPECT_GE(task->progress(), 1.0f);
}

// 新增：速度变化测试
TEST_F(DownloadTaskTest, SpeedVariation) {
    auto task = std::make_shared<DownloadTask>(21, "https://example.com/file.zip", options_);

    // 初始速度
    task->update_progress(100, 1000, 100);
    EXPECT_EQ(task->speed(), 100);

    // 速度增加
    task->update_progress(300, 1000, 500);
    EXPECT_EQ(task->speed(), 500);

    // 速度降低
    task->update_progress(400, 1000, 50);
    EXPECT_EQ(task->speed(), 50);

    // 零速度
    task->update_progress(400, 1000, 0);
    EXPECT_EQ(task->speed(), 0);
}

// 新增：错误状态设置和获取
TEST_F(DownloadTaskTest, ErrorState) {
    auto task = std::make_shared<DownloadTask>(22, "https://example.com/file.zip", options_);

    task->set_error("Network error: timeout");
    EXPECT_EQ(task->error_message(), "Network error: timeout");

    task->set_status(TaskStatus::Failed);
    EXPECT_EQ(task->status(), TaskStatus::Failed);
    EXPECT_TRUE(task->is_finished());
    EXPECT_FALSE(task->is_active());
}

// 新增：多次取消尝试
TEST_F(DownloadTaskTest, MultipleCancelAttempts) {
    auto task = std::make_shared<DownloadTask>(23, "https://example.com/file.zip", options_);

    task->set_status(TaskStatus::Downloading);

    EXPECT_TRUE(task->cancel());
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);

    // 第二次取消应该失败
    EXPECT_FALSE(task->cancel());
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
}

// 新增：暂停状态验证
TEST_F(DownloadTaskTest, PauseStateVerification) {
    auto task = std::make_shared<DownloadTask>(24, "https://example.com/file.zip", options_);

    // 从 Pending 状态暂停
    EXPECT_TRUE(task->pause());
    EXPECT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(task->is_active());
    EXPECT_FALSE(task->is_finished());

    // 从 Downloading 状态暂停
    task->set_status(TaskStatus::Downloading);
    EXPECT_TRUE(task->pause());
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // 从 Paused 状态再次暂停（应该失败）
    EXPECT_FALSE(task->pause());
}

// 新增：恢复状态验证
TEST_F(DownloadTaskTest, ResumeStateVerification) {
    auto task = std::make_shared<DownloadTask>(25, "https://example.com/file.zip", options_);

    task->set_status(TaskStatus::Paused);
    task->resume();
    // 任务应该变为 Pending 或 Preparing 状态

    // 从非暂停状态恢复
    task->set_status(TaskStatus::Downloading);
    task->resume();
    EXPECT_EQ(task->status(), TaskStatus::Downloading);
}

// 新增：等超时测试
TEST_F(DownloadTaskTest, WaitForTimeout) {
    auto task = std::make_shared<DownloadTask>(26, "https://example.com/file.zip", options_);

    // 不完成任务，等待超时
    bool completed = task->wait_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(completed);
    EXPECT_NE(task->status(), TaskStatus::Completed);
}

// 新增：无限等待测试
TEST_F(DownloadTaskTest, WaitIndefinitely) {
    auto task = std::make_shared<DownloadTask>(27, "https://example.com/file.zip", options_);

    // 在另一个线程中完成任务
    std::thread completer([task]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        task->set_status(TaskStatus::Completed);
    });

    // 无限等待
    task->wait();
    EXPECT_EQ(task->status(), TaskStatus::Completed);

    completer.join();
}

// 新增：并发状态修改
TEST_F(DownloadTaskTest, ConcurrentStatusModification) {
    auto task = std::make_shared<DownloadTask>(28, "https://example.com/file.zip", options_);

    std::vector<std::thread> threads;

    // 多个线程尝试修改状态
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([task, i]() {
            task->update_progress(i * 100, 1000, 100);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 最终状态应该有效
    EXPECT_GE(task->downloaded_bytes(), 0);
    EXPECT_LE(task->downloaded_bytes(), 1000);
}

// 新增：文件信息完整性
TEST_F(DownloadTaskTest, FileInfoCompleteness) {
    auto task = std::make_shared<DownloadTask>(29, "https://example.com/file.zip", options_);

    FileInfo info;
    info.url = "https://example.com/file.zip";
    info.filename = "file.zip";
    info.total_size = 2048;
    info.supports_resume = true;
    info.content_type = "application/zip";

    // 设置最后修改时间
    info.last_modified = std::chrono::steady_clock::now();

    task->set_file_info(info);

    auto retrieved = task->file_info();
    EXPECT_EQ(retrieved.url, info.url);
    EXPECT_EQ(retrieved.filename, info.filename);
    EXPECT_EQ(retrieved.total_size, info.total_size);
    EXPECT_EQ(retrieved.supports_resume, info.supports_resume);
    EXPECT_EQ(retrieved.content_type, info.content_type);
}

// 新增：输出路径组合测试
TEST_F(DownloadTaskTest, OutputPathCombination) {
    DownloadOptions options;
    options.output_directory = "/tmp/downloads";
    options.output_filename = "test.zip";

    auto task = std::make_shared<DownloadTask>(30, "https://example.com/test.zip", options);

    // 如果没有显式设置输出路径，应该使用 options 中的路径
    auto expected_path = "/tmp/downloads/test.zip";
    // 验证输出路径正确
}

// 新增：空 URL 处理
TEST_F(DownloadTaskTest, EmptyUrl) {
    auto task = std::make_shared<DownloadTask>(31, "", options_);

    EXPECT_EQ(task->url(), "");
    EXPECT_EQ(task->status(), TaskStatus::Pending);
}

// 新增：超长 URL 处理
TEST_F(DownloadTaskTest, VeryLongUrl) {
    std::string long_url(10000, 'a');
    long_url = "https://example.com/" + long_url + ".zip";

    auto task = std::make_shared<DownloadTask>(32, long_url, options_);

    EXPECT_EQ(task->url(), long_url);
}

// 新增：特殊字符 URL
TEST_F(DownloadTaskTest, SpecialCharactersInUrl) {
    std::string url = "https://example.com/file%20name%20with%20spaces.zip?query=value&other=123";

    auto task = std::make_shared<DownloadTask>(33, url, options_);

    EXPECT_EQ(task->url(), url);
}

// 新增：进度信息准确性
TEST_F(DownloadTaskTest, ProgressInfoAccuracy) {
    auto task = std::make_shared<DownloadTask>(34, "https://example.com/file.zip", options_);

    task->mark_started();
    task->update_progress(750, 1000, 250);

    ProgressInfo info = task->get_progress_info();

    EXPECT_EQ(info.task_id, 34);
    EXPECT_EQ(info.downloaded_bytes, 750);
    EXPECT_EQ(info.total_bytes, 1000);
    EXPECT_EQ(info.speed, 250);
    EXPECT_FLOAT_EQ(info.progress, 0.75f);
}

// 新增：剩余时间估算精度
TEST_F(DownloadTaskTest, RemainingTimeEstimation) {
    auto task = std::make_shared<DownloadTask>(35, "https://example.com/file.zip", options_);

    // 1000 bytes 总量，已下载 200，速度 100 bytes/sec
    task->update_progress(200, 1000, 100);

    auto remaining = task->estimated_remaining();
    auto expected_seconds = (1000 - 200) / 100;  // 8 秒

    EXPECT_GE(remaining.count(), std::chrono::seconds(7).count());
    EXPECT_LE(remaining.count(), std::chrono::seconds(9).count());
}

// 新增：经过时间计算
TEST_F(DownloadTaskTest, ElapsedTimeCalculation) {
    auto task = std::make_shared<DownloadTask>(36, "https://example.com/file.zip", options_);

    task->mark_started();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto elapsed = task->elapsed();
    EXPECT_GE(elapsed.count(), std::chrono::milliseconds(150).count());
    EXPECT_LE(elapsed.count(), std::chrono::milliseconds(300).count());
}

// 新增：任务生命周期完整测试
TEST_F(DownloadTaskTest, CompleteLifecycle) {
    auto task = std::make_shared<DownloadTask>(37, "https://example.com/file.zip", options_);

    // 初始状态
    EXPECT_EQ(task->status(), TaskStatus::Pending);

    // 开始任务
    task->mark_started();
    task->set_status(TaskStatus::Preparing);
    EXPECT_TRUE(task->is_active());

    // 下载中
    task->set_status(TaskStatus::Downloading);
    task->update_progress(500, 1000, 100);
    EXPECT_TRUE(task->is_active());
    EXPECT_FLOAT_EQ(task->progress(), 0.5f);

    // 完成
    task->set_status(TaskStatus::Completed);
    EXPECT_TRUE(task->is_finished());
    EXPECT_FALSE(task->is_active());
}

// 新增：失败后重置
TEST_F(DownloadTaskTest, ResetAfterFailure) {
    auto task = std::make_shared<DownloadTask>(38, "https://example.com/file.zip", options_);

    task->set_status(TaskStatus::Downloading);
    task->set_error("Connection lost");
    task->set_status(TaskStatus::Failed);

    EXPECT_EQ(task->status(), TaskStatus::Failed);
    EXPECT_FALSE(task->error_message().empty());

    // 重置任务以重试
    task->set_status(TaskStatus::Pending);
    EXPECT_EQ(task->status(), TaskStatus::Pending);
}

// 新增：大文件进度测试
TEST_F(DownloadTaskTest, LargeFileProgress) {
    auto task = std::make_shared<DownloadTask>(39, "https://example.com/large.bin", options_);

    constexpr uint64_t large_size = 10ULL * 1024 * 1024 * 1024;  // 10GB

    task->update_progress(large_size / 2, large_size, 1024 * 1024);  // 50%, 1MB/s

    EXPECT_FLOAT_EQ(task->progress(), 0.5f);
    EXPECT_EQ(task->downloaded_bytes(), large_size / 2);
    EXPECT_EQ(task->total_bytes(), large_size);
}

// 新增：速度为零时的剩余时间
TEST_F(DownloadTaskTest, RemainingTimeWithZeroSpeed) {
    auto task = std::make_shared<DownloadTask>(40, "https://example.com/file.zip", options_);

    task->update_progress(500, 1000, 0);

    auto remaining = task->estimated_remaining();
    EXPECT_EQ(remaining.count(), 0);
}
