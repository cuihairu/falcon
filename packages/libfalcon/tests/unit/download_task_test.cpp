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
