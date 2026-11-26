/**
 * @file task_storage_listener_test.cpp
 * @brief TaskStorageListener 单元测试：引擎事件到 SQLite 的持久化
 * @author Falcon Team
 * @date 2026-09-11
 */

#include <gtest/gtest.h>

#include "storage/task_storage.hpp"
#include "storage/task_storage_listener.hpp"

#include <chrono>

using falcon::daemon::TaskRecord;
using falcon::daemon::TaskStorage;
using falcon::daemon::TaskStorageConfig;
using falcon::daemon::TaskStorageListener;
using falcon::ProgressInfo;
using falcon::TaskId;
using falcon::TaskStatus;

namespace {

TaskRecord make_record(TaskId id) {
    TaskRecord record;
    record.id = id;
    record.url = "https://example.com/file-" + std::to_string(id) + ".bin";
    record.status = TaskStatus::Pending;
    auto now = std::chrono::system_clock::now();
    record.created_at = now;
    record.updated_at = now;
    return record;
}

ProgressInfo make_progress(TaskId id, falcon::Bytes downloaded, float ratio) {
    ProgressInfo info;
    info.task_id = id;
    info.downloaded_bytes = downloaded;
    info.total_bytes = 1000;
    info.progress = ratio;
    info.speed = 1024;
    return info;
}

}  // namespace

class TaskStorageListenerTest : public ::testing::Test {
protected:
    void SetUp() override {
        TaskStorageConfig config;  // db_path 默认 ":memory:"
        storage_ = std::make_unique<TaskStorage>(config);
        ASSERT_TRUE(storage_->initialize());
        listener_ = std::make_unique<TaskStorageListener>(storage_.get());
    }

    // 模拟引擎回调的事件监听器
    std::unique_ptr<TaskStorage> storage_;
    std::unique_ptr<TaskStorageListener> listener_;
};

TEST_F(TaskStorageListenerTest, StatusChangePersists) {
    auto id = storage_->create_task(make_record(101));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_status_changed(id, TaskStatus::Pending, TaskStatus::Downloading);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Downloading);
}

TEST_F(TaskStorageListenerTest, PausePersists) {
    auto id = storage_->create_task(make_record(102));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Paused);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Paused);
}

TEST_F(TaskStorageListenerTest, CancelledPersistsAndClearsErrorCache) {
    auto id = storage_->create_task(make_record(107));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    // 先缓存一条错误消息，Cancelled 分支必须把它清掉（终态无错误语义）
    listener_->on_error(id, "transient network error");
    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Cancelled);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Cancelled);
    EXPECT_TRUE(record->error_message.empty());
}

TEST_F(TaskStorageListenerTest, CompletedMarksTerminalState) {
    auto id = storage_->create_task(make_record(103));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Completed);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Completed);
    // 终态必须带 completed_at，否则重启后无法区分历史任务
    EXPECT_TRUE(record->completed_at.has_value());
    // Completed 不在 active 列表里，重启时不会被误恢复
    EXPECT_TRUE(storage_->get_active_tasks().empty());
}

TEST_F(TaskStorageListenerTest, FailedUsesCachedErrorMessage) {
    auto id = storage_->create_task(make_record(104));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_error(id, "connection reset by peer");
    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Failed);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Failed);
    EXPECT_EQ(record->error_message, "connection reset by peer");
}

TEST_F(TaskStorageListenerTest, FailedWithoutCachedErrorKeepsEmptyMessage) {
    auto id = storage_->create_task(make_record(105));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Failed);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Failed);
    EXPECT_TRUE(record->error_message.empty());
}

TEST_F(TaskStorageListenerTest, ErrorCacheConsumedOnce) {
    auto id = storage_->create_task(make_record(106));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_error(id, "transient error");
    // 一次 Failed 消费缓存后再次 Failed 不应复用旧消息
    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Failed);
    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Failed);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->error_message.empty());
}

TEST_F(TaskStorageListenerTest, ProgressPersistsImmediately) {
    auto id = storage_->create_task(make_record(107));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_progress(make_progress(id, 100, 0.1f));

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->downloaded_bytes, 100);
    // progress 经历 float → double 转换，用容差断言
    EXPECT_NEAR(record->progress, 0.1, 1e-6);
    EXPECT_EQ(record->speed, 1024);
}

TEST_F(TaskStorageListenerTest, ProgressThrottledPerTask) {
    auto id = storage_->create_task(make_record(108));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_progress(make_progress(id, 100, 0.1f));
    // 默认 1 秒节流窗口内的第二次更新不落库
    listener_->on_progress(make_progress(id, 200, 0.2f));

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->downloaded_bytes, 100);
}

TEST_F(TaskStorageListenerTest, CompletedFlushesThrottleSwallowedFinalProgress) {
    // 真实走查曝光的缺陷（2026-10-01）：1s 节流吞掉 final_update 后，
    // Completed 行保留陈旧字节计数——重启后已完成记录显示 131072/192KB。
    // 修复契约：终态转换无条件补写内存里缓存的最新进度。
    auto id = storage_->create_task(make_record(111));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_progress(make_progress(id, 131072, 0.66f));  // 首次，立即落库
    // 终态事件携带真实总长（此前 update_progress 不落 total_bytes，
    // 重启后 tellStatus totalLength 恒 0）
    ProgressInfo final_info = make_progress(id, 196608, 1.0f);
    final_info.total_bytes = 196608;
    listener_->on_progress(final_info);                        // 节流窗内，被吞

    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Completed);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Completed);
    // 被节流吞掉的最终字节计数必须出现在库里
    EXPECT_EQ(record->downloaded_bytes, 196608);
    // 总长同步落库（MAX 只升不降：0 = 未知不覆盖已知值）
    EXPECT_EQ(record->total_bytes, 196608);
}

TEST_F(TaskStorageListenerTest, PausedFlushesLatestProgress) {
    auto id = storage_->create_task(make_record(112));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_progress(make_progress(id, 100, 0.1f));  // 立即落库
    listener_->on_progress(make_progress(id, 900, 0.9f));  // 被节流吞掉

    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Paused);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Paused);
    // 暂停是停机恢复的落点——行内字节必须是最新的
    EXPECT_EQ(record->downloaded_bytes, 900);
}

TEST_F(TaskStorageListenerTest, FailedFlushesPartialProgress) {
    auto id = storage_->create_task(make_record(113));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->on_progress(make_progress(id, 400, 0.4f));  // 立即落库
    listener_->on_progress(make_progress(id, 600, 0.6f));  // 被节流吞掉

    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Failed);

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Failed);
    // 失败时已下载的部分字节同样真实
    EXPECT_EQ(record->downloaded_bytes, 600);
}

TEST_F(TaskStorageListenerTest, ProgressUnthrottledWithZeroInterval) {
    TaskStorageListener fast_listener(storage_.get(), std::chrono::milliseconds(0));

    auto id = storage_->create_task(make_record(109));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    fast_listener.on_progress(make_progress(id, 100, 0.1f));
    fast_listener.on_progress(make_progress(id, 200, 0.2f));

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->downloaded_bytes, 200);
    EXPECT_NEAR(record->progress, 0.2, 1e-6);
}

TEST_F(TaskStorageListenerTest, InvalidTaskIdIgnored) {
    listener_->on_progress(make_progress(falcon::INVALID_TASK_ID, 100, 0.1f));
    EXPECT_TRUE(storage_->list_tasks().empty());
}

TEST_F(TaskStorageListenerTest, ShutdownStopsPersisting) {
    auto id = storage_->create_task(make_record(110));
    ASSERT_NE(id, falcon::INVALID_TASK_ID);

    listener_->shutdown();

    // shutdown 之后所有回调都不再触碰 storage（可安全析构 storage）
    listener_->on_status_changed(id, TaskStatus::Downloading, TaskStatus::Completed);
    listener_->on_progress(make_progress(id, 100, 0.1f));
    listener_->on_error(id, "late error");

    auto record = storage_->get_task(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, TaskStatus::Pending);
    EXPECT_EQ(record->downloaded_bytes, 0);
}
