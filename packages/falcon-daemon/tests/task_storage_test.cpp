/**
 * @file task_storage_test.cpp
 * @brief Unit tests for TaskStorage
 * @author Falcon Team
 * @date 2025-12-28
 */

#include <gtest/gtest.h>

#ifdef FALCON_HAS_SQLITE3

#include "storage/task_storage.hpp"

#include <fstream>
#include <cstdio>

namespace {

using falcon::INVALID_TASK_ID;
using falcon::TaskId;
using falcon::TaskStatus;
using falcon::daemon::TaskQueryOptions;
using falcon::daemon::TaskRecord;
using falcon::daemon::TaskStorage;
using falcon::daemon::TaskStorageConfig;

class TaskStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use in-memory database for tests
        TaskStorageConfig config;
        config.db_path = ":memory:";
        config.enable_wal_mode = false;  // WAL not supported for in-memory

        storage_ = std::make_unique<TaskStorage>(config);
        ASSERT_TRUE(storage_->initialize());
    }

    void TearDown() override {
        storage_.reset();
    }

    TaskRecord create_test_record(const std::string& url = "https://example.com/file.zip") {
        TaskRecord record;
        record.url = url;
        record.output_path = "/tmp/file.zip";
        record.status = TaskStatus::Pending;
        record.progress = 0.0;
        record.total_bytes = 1024 * 1024;
        record.downloaded_bytes = 0;
        record.speed = 0;
        record.created_at = std::chrono::system_clock::now();
        record.updated_at = std::chrono::system_clock::now();

        record.options.max_connections = 4;
        record.options.timeout_seconds = 30;
        record.options.user_agent = "Falcon/0.1.0";

        return record;
    }

    std::unique_ptr<TaskStorage> storage_;
};

// ============================================================================
// Initialization Tests
// ============================================================================

TEST_F(TaskStorageTest, InitializeSuccess) {
    EXPECT_TRUE(storage_->is_ready());
}

TEST_F(TaskStorageTest, GetDbPath) {
    EXPECT_EQ(":memory:", storage_->get_db_path());
}

// ============================================================================
// CRUD Tests
// ============================================================================

TEST_F(TaskStorageTest, CreateTask) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    EXPECT_NE(INVALID_TASK_ID, id);
    EXPECT_GT(id, 0);
}

TEST_F(TaskStorageTest, GetTask) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    auto retrieved = storage_->get_task(id);
    ASSERT_TRUE(retrieved.has_value());

    EXPECT_EQ(id, retrieved->id);
    EXPECT_EQ(record.url, retrieved->url);
    EXPECT_EQ(record.output_path, retrieved->output_path);
    EXPECT_EQ(record.status, retrieved->status);
}

TEST_F(TaskStorageTest, GetTaskNotFound) {
    auto result = storage_->get_task(99999);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TaskStorageTest, UpdateTask) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    // Update the record
    record.url = "https://example.com/updated.zip";
    record.status = TaskStatus::Downloading;
    record.progress = 0.5;
    record.downloaded_bytes = 512 * 1024;
    record.updated_at = std::chrono::system_clock::now();

    EXPECT_TRUE(storage_->update_task(id, record));

    auto retrieved = storage_->get_task(id);
    ASSERT_TRUE(retrieved.has_value());

    EXPECT_EQ("https://example.com/updated.zip", retrieved->url);
    EXPECT_EQ(TaskStatus::Downloading, retrieved->status);
    EXPECT_DOUBLE_EQ(0.5, retrieved->progress);
}

TEST_F(TaskStorageTest, DeleteTask) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    EXPECT_TRUE(storage_->task_exists(id));
    EXPECT_TRUE(storage_->delete_task(id));
    EXPECT_FALSE(storage_->task_exists(id));
}

TEST_F(TaskStorageTest, DeleteNonExistentTask) {
    EXPECT_FALSE(storage_->delete_task(99999));
}

TEST_F(TaskStorageTest, TaskExists) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    EXPECT_TRUE(storage_->task_exists(id));
    EXPECT_FALSE(storage_->task_exists(99999));
}

// ============================================================================
// Query Tests
// ============================================================================

TEST_F(TaskStorageTest, ListTasks) {
    // Create multiple tasks
    for (int i = 0; i < 5; ++i) {
        auto record = create_test_record("https://example.com/file" + std::to_string(i) + ".zip");
        storage_->create_task(record);
    }

    auto tasks = storage_->list_tasks();
    EXPECT_EQ(5, tasks.size());
}

TEST_F(TaskStorageTest, ListTasksWithLimit) {
    for (int i = 0; i < 10; ++i) {
        auto record = create_test_record("https://example.com/file" + std::to_string(i) + ".zip");
        storage_->create_task(record);
    }

    TaskQueryOptions options;
    options.limit = 3;

    auto tasks = storage_->list_tasks(options);
    EXPECT_EQ(3, tasks.size());
}

TEST_F(TaskStorageTest, ListTasksWithStatusFilter) {
    // Create tasks with different statuses
    auto record1 = create_test_record("https://example.com/pending.zip");
    TaskId id1 = storage_->create_task(record1);

    auto record2 = create_test_record("https://example.com/downloading.zip");
    TaskId id2 = storage_->create_task(record2);
    storage_->update_status(id2, TaskStatus::Downloading);

    auto record3 = create_test_record("https://example.com/completed.zip");
    TaskId id3 = storage_->create_task(record3);
    storage_->update_status(id3, TaskStatus::Completed);

    TaskQueryOptions options;
    options.status_filter = TaskStatus::Downloading;

    auto tasks = storage_->list_tasks(options);
    ASSERT_EQ(1, tasks.size());
    EXPECT_EQ(id2, tasks[0].id);
}

TEST_F(TaskStorageTest, GetTasksByStatus) {
    auto record1 = create_test_record("https://example.com/file1.zip");
    TaskId id1 = storage_->create_task(record1);
    storage_->update_status(id1, TaskStatus::Completed);

    auto record2 = create_test_record("https://example.com/file2.zip");
    TaskId id2 = storage_->create_task(record2);
    storage_->update_status(id2, TaskStatus::Completed);

    auto record3 = create_test_record("https://example.com/file3.zip");
    storage_->create_task(record3);  // Pending

    auto completed = storage_->get_tasks_by_status(TaskStatus::Completed);
    EXPECT_EQ(2, completed.size());

    auto pending = storage_->get_tasks_by_status(TaskStatus::Pending);
    EXPECT_EQ(1, pending.size());
}

TEST_F(TaskStorageTest, GetActiveTasks) {
    auto record1 = create_test_record("https://example.com/file1.zip");
    TaskId id1 = storage_->create_task(record1);
    storage_->update_status(id1, TaskStatus::Downloading);

    auto record2 = create_test_record("https://example.com/file2.zip");
    TaskId id2 = storage_->create_task(record2);
    storage_->update_status(id2, TaskStatus::Preparing);

    auto record3 = create_test_record("https://example.com/file3.zip");
    storage_->create_task(record3);  // Pending

    auto active = storage_->get_active_tasks();
    EXPECT_EQ(2, active.size());
}

TEST_F(TaskStorageTest, CountTasks) {
    for (int i = 0; i < 5; ++i) {
        auto record = create_test_record("https://example.com/file" + std::to_string(i) + ".zip");
        TaskId id = storage_->create_task(record);
        if (i < 2) {
            storage_->update_status(id, TaskStatus::Completed);
        }
    }

    EXPECT_EQ(5, storage_->count_all_tasks());
    EXPECT_EQ(2, storage_->count_tasks_by_status(TaskStatus::Completed));
    EXPECT_EQ(3, storage_->count_tasks_by_status(TaskStatus::Pending));
}

// ============================================================================
// Update Operations Tests
// ============================================================================

TEST_F(TaskStorageTest, UpdateProgress) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    EXPECT_TRUE(storage_->update_progress(id, 512 * 1024, 0.5, 1024 * 1024));

    auto retrieved = storage_->get_task(id);
    ASSERT_TRUE(retrieved.has_value());

    EXPECT_EQ(512 * 1024, retrieved->downloaded_bytes);
    EXPECT_DOUBLE_EQ(0.5, retrieved->progress);
    EXPECT_EQ(1024 * 1024, retrieved->speed);
}

TEST_F(TaskStorageTest, UpdateStatus) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    EXPECT_TRUE(storage_->update_status(id, TaskStatus::Paused));

    auto retrieved = storage_->get_task(id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(TaskStatus::Paused, retrieved->status);
}

TEST_F(TaskStorageTest, MarkCompleted) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    EXPECT_TRUE(storage_->mark_completed(id));

    auto retrieved = storage_->get_task(id);
    ASSERT_TRUE(retrieved.has_value());

    EXPECT_EQ(TaskStatus::Completed, retrieved->status);
    EXPECT_DOUBLE_EQ(1.0, retrieved->progress);
    EXPECT_TRUE(retrieved->completed_at.has_value());
}

TEST_F(TaskStorageTest, MarkFailed) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);

    EXPECT_TRUE(storage_->mark_failed(id, "Connection timeout"));

    auto retrieved = storage_->get_task(id);
    ASSERT_TRUE(retrieved.has_value());

    EXPECT_EQ(TaskStatus::Failed, retrieved->status);
    EXPECT_EQ("Connection timeout", retrieved->error_message);
}

// ============================================================================
// Options Serialization Tests
// ============================================================================

TEST_F(TaskStorageTest, OptionsSerialization) {
    auto record = create_test_record();
    record.options.max_connections = 8;
    record.options.timeout_seconds = 60;
    record.options.user_agent = "CustomAgent/1.0";
    record.options.proxy = "http://proxy:8080";
    record.options.headers["Authorization"] = "Bearer token123";
    record.options.headers["X-Custom"] = "value";

    TaskId id = storage_->create_task(record);

    auto retrieved = storage_->get_task(id);
    ASSERT_TRUE(retrieved.has_value());

    EXPECT_EQ(8, retrieved->options.max_connections);
    EXPECT_EQ(60, retrieved->options.timeout_seconds);
    EXPECT_EQ("CustomAgent/1.0", retrieved->options.user_agent);
    EXPECT_EQ("http://proxy:8080", retrieved->options.proxy);
    EXPECT_EQ(2, retrieved->options.headers.size());
    EXPECT_EQ("Bearer token123", retrieved->options.headers["Authorization"]);
    EXPECT_EQ("value", retrieved->options.headers["X-Custom"]);
}

// ============================================================================
// Maintenance Tests
// ============================================================================

TEST_F(TaskStorageTest, DeleteAllTasks) {
    for (int i = 0; i < 5; ++i) {
        auto record = create_test_record("https://example.com/file" + std::to_string(i) + ".zip");
        storage_->create_task(record);
    }

    EXPECT_EQ(5, storage_->count_all_tasks());
    EXPECT_TRUE(storage_->delete_all_tasks());
    EXPECT_EQ(0, storage_->count_all_tasks());
}

TEST_F(TaskStorageTest, Vacuum) {
    // Create and delete some tasks to fragment the database
    for (int i = 0; i < 10; ++i) {
        auto record = create_test_record("https://example.com/file" + std::to_string(i) + ".zip");
        TaskId id = storage_->create_task(record);
        if (i % 2 == 0) {
            storage_->delete_task(id);
        }
    }

    EXPECT_TRUE(storage_->vacuum());
}

} // anonymous namespace

#endif // FALCON_HAS_SQLITE3
