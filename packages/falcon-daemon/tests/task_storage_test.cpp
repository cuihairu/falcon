/**
 * @file task_storage_test.cpp
 * @brief Unit tests for TaskStorage
 * @author Falcon Team
 * @date 2025-12-28
 */

#include <gtest/gtest.h>

#ifdef FALCON_HAS_SQLITE3

#include "storage/task_storage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <unistd.h>

#include <sqlite3.h>

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

TEST_F(TaskStorageTest, GetMaxTaskId) {
    // 空表返回 nullopt
    EXPECT_FALSE(storage_->get_max_task_id().has_value());

    TaskId first = storage_->create_task(create_test_record());
    TaskId second = storage_->create_task(create_test_record());

    auto max_id = storage_->get_max_task_id();
    ASSERT_TRUE(max_id.has_value());
    EXPECT_EQ(*max_id, std::max(first, second));

    // 删除最大 id 记录后应回落到剩余记录的最大值
    storage_->delete_task(std::max(first, second));
    max_id = storage_->get_max_task_id();
    ASSERT_TRUE(max_id.has_value());
    EXPECT_EQ(*max_id, std::min(first, second));
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

// ============================================================================
// Initialization failure / error surface / cleanup / move（覆盖率批次 L）
// ============================================================================

// 打不开的库路径（父目录不存在）：initialize 失败并留下 last_error
TEST_F(TaskStorageTest, InitializeOpenFailure) {
    TaskStorageConfig config;
    config.db_path = (std::filesystem::temp_directory_path() /
                      "falcon_no_such_dir/sub/tasks.db").string();

    TaskStorage bad(config);
    EXPECT_FALSE(bad.initialize());
    EXPECT_FALSE(bad.is_ready());
    EXPECT_FALSE(bad.get_last_error().empty());
}

// 库文件不是 SQLite 数据库：建表阶段失败，initialize 关闭连接返回 false
TEST_F(TaskStorageTest, InitializeNonDatabaseFile) {
    auto path = (std::filesystem::temp_directory_path() /
                 ("falcon_not_a_db_" +
                  std::to_string(std::chrono::steady_clock::now()
                                     .time_since_epoch().count()) + ".db")).string();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << std::string(256, (char)0xAB);
    }

    TaskStorageConfig config;
    config.db_path = path;
    config.enable_wal_mode = false;
    TaskStorage bad(config);
    EXPECT_FALSE(bad.initialize());
    EXPECT_FALSE(bad.is_ready());
    EXPECT_FALSE(bad.get_last_error().empty());
    std::filesystem::remove(path);
}

// completed_at 有值：create 写入毫秒时间戳，get 回读还原
TEST_F(TaskStorageTest, CreateTaskWithCompletedAt) {
    auto record = create_test_record();
    record.status = TaskStatus::Completed;
    auto completed = std::chrono::system_clock::now();
    record.completed_at = completed;

    TaskId id = storage_->create_task(record);
    ASSERT_NE(INVALID_TASK_ID, id);

    auto loaded = storage_->get_task(id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->completed_at.has_value());
    EXPECT_LT(std::chrono::abs(*loaded->completed_at - completed),
              std::chrono::seconds(2));
}

// 显式 id 重复插入：UNIQUE 约束使 step 失败，返回 INVALID_TASK_ID
TEST_F(TaskStorageTest, CreateTaskDuplicateExplicitIdFails) {
    auto record = create_test_record();
    record.id = 4200;
    TaskId first = storage_->create_task(record);
    ASSERT_NE(INVALID_TASK_ID, first);

    EXPECT_EQ(INVALID_TASK_ID, storage_->create_task(record));
    EXPECT_FALSE(storage_->get_last_error().empty());
}

// update 通道同样承载 completed_at
TEST_F(TaskStorageTest, UpdateTaskWithCompletedAt) {
    auto record = create_test_record();
    TaskId id = storage_->create_task(record);
    ASSERT_NE(INVALID_TASK_ID, id);

    auto completed = std::chrono::system_clock::now();
    record.completed_at = completed;
    record.status = TaskStatus::Completed;
    ASSERT_TRUE(storage_->update_task(id, record));

    auto loaded = storage_->get_task(id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->completed_at.has_value());
    EXPECT_LT(std::chrono::abs(*loaded->completed_at - completed),
              std::chrono::seconds(2));
}

// limit + offset 分页（created_at 显式错开保证 ORDER BY 次序确定）
TEST_F(TaskStorageTest, ListTasksWithOffset) {
    auto base = std::chrono::system_clock::now();
    for (int i = 0; i < 3; ++i) {
        auto record = create_test_record("https://example.com/paged" +
                                         std::to_string(i) + ".zip");
        record.created_at = base + std::chrono::minutes(i);
        storage_->create_task(record);
    }

    TaskQueryOptions options;
    options.limit = 2;
    options.offset = 1;
    auto page = storage_->list_tasks(options);
    ASSERT_EQ(2u, page.size());
    // 默认按 created_at 降序：paged2 > paged1 > paged0，跳过第 1 条
    EXPECT_EQ("https://example.com/paged1.zip", page[0].url);
    EXPECT_EQ("https://example.com/paged0.zip", page[1].url);
}

// 过期清理：只删「Completed 且 completed_at 早于保留期」的行；
// 未初始化实例返回 0
TEST_F(TaskStorageTest, CleanupCompletedTasks) {
    namespace chrono = std::chrono;
    auto old_completed = create_test_record("https://example.com/old1.zip");
    old_completed.status = TaskStatus::Completed;
    old_completed.completed_at = chrono::system_clock::now() - chrono::hours(24 * 30);
    storage_->create_task(old_completed);

    auto old_completed2 = create_test_record("https://example.com/old2.zip");
    old_completed2.status = TaskStatus::Completed;
    old_completed2.completed_at = chrono::system_clock::now() - chrono::hours(24 * 30);
    TaskId keep_pending_check = storage_->create_task(old_completed2);

    auto fresh_completed = create_test_record("https://example.com/fresh.zip");
    fresh_completed.status = TaskStatus::Completed;
    fresh_completed.completed_at = chrono::system_clock::now();
    storage_->create_task(fresh_completed);

    auto old_pending = create_test_record("https://example.com/pending.zip");
    old_pending.status = TaskStatus::Pending;
    storage_->create_task(old_pending);

    EXPECT_EQ(2, storage_->cleanup_completed_tasks(7));
    EXPECT_FALSE(storage_->task_exists(keep_pending_check));
    EXPECT_EQ(2, storage_->count_all_tasks());  // fresh + pending 留存
    EXPECT_EQ(1, storage_->count_tasks_by_status(TaskStatus::Completed));
    EXPECT_EQ(1, storage_->count_tasks_by_status(TaskStatus::Pending));

    // 再次清理无可删行
    EXPECT_EQ(0, storage_->cleanup_completed_tasks(7));

    // 未初始化实例：返回 0
    TaskStorageConfig config;
    TaskStorage cold(config);
    EXPECT_EQ(0, cold.cleanup_completed_tasks(7));
}

// move 语义：move 构造与 move 赋值后目标实例接管连接可正常工作
TEST_F(TaskStorageTest, MoveSemantics) {
    TaskId id = storage_->create_task(create_test_record());
    ASSERT_NE(INVALID_TASK_ID, id);

    TaskStorage moved_to(std::move(*storage_));
    EXPECT_TRUE(moved_to.is_ready());
    EXPECT_TRUE(moved_to.task_exists(id));

    TaskStorage assigned_to(TaskStorageConfig{});
    assigned_to = std::move(moved_to);
    EXPECT_TRUE(assigned_to.is_ready());
    EXPECT_TRUE(assigned_to.task_exists(id));
}

} // anonymous namespace

// ============================================================================
// 文件库专属：外部直连篡改/加锁触发的 sqlite 失败路径（内存库无法第二连接）
// ============================================================================

namespace {

/// 临时文件库目录（进程内唯一名，测完清理）
class ScopedDbDir {
public:
    ScopedDbDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("falcon_ts_busy_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~ScopedDbDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::filesystem::path db_path() const { return path_ / "tasks.db"; }

private:
    static inline unsigned long long counter_ = 0;
    std::filesystem::path path_;
};

TaskRecord file_test_record() {
    TaskRecord record;
    record.url = "https://example.com/file.zip";
    record.output_path = "/tmp/file.zip";
    record.status = TaskStatus::Pending;
    record.total_bytes = 4096;
    return record;
}

} // namespace

// 第二连接持 BEGIN EXCLUSIVE 事务：TaskStorage 的每个 CRUD/统计方法在
// prepare/step 上撞 SQLITE_BUSY，必须走各自的失败返回并记录 last_error
TEST(TaskStorageFileTest, MethodsFailWhileDbLockedExclusively) {
    ScopedDbDir dir;
    TaskStorageConfig config;
    config.db_path = dir.db_path().string();
    config.enable_wal_mode = false;
    config.busy_timeout_ms = 10;  // 锁期间各方法快速失败，不拖慢测试
    TaskStorage storage(config);
    ASSERT_TRUE(storage.initialize());
    const TaskId id = storage.create_task(file_test_record());
    ASSERT_NE(INVALID_TASK_ID, id);

    sqlite3* locker = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(dir.db_path().string().c_str(), &locker));
    char* errmsg = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_exec(locker, "BEGIN EXCLUSIVE;", nullptr, nullptr, &errmsg))
        << (errmsg ? errmsg : "");
    sqlite3_free(errmsg);

    const TaskRecord record = file_test_record();
    EXPECT_EQ(INVALID_TASK_ID, storage.create_task(record));
    EXPECT_FALSE(storage.get_task(id).has_value());
    EXPECT_FALSE(storage.update_task(id, record));
    EXPECT_FALSE(storage.delete_task(id));
    EXPECT_FALSE(storage.task_exists(id));
    EXPECT_TRUE(storage.list_tasks().empty());
    EXPECT_TRUE(storage.get_tasks_by_status(TaskStatus::Pending).empty());
    EXPECT_TRUE(storage.get_active_tasks().empty());
    EXPECT_EQ(0, storage.count_tasks_by_status(TaskStatus::Pending));
    EXPECT_EQ(0, storage.count_all_tasks());
    EXPECT_FALSE(storage.get_max_task_id().has_value());
    EXPECT_FALSE(storage.update_progress(id, 1, 0.5, 1));
    EXPECT_FALSE(storage.update_status(id, TaskStatus::Paused));
    EXPECT_FALSE(storage.mark_completed(id));
    EXPECT_FALSE(storage.mark_failed(id, "boom"));
    EXPECT_FALSE(storage.delete_all_tasks());
    EXPECT_FALSE(storage.vacuum());
    EXPECT_EQ(0, storage.cleanup_completed_tasks(7));
    EXPECT_FALSE(storage.get_last_error().empty());

    sqlite3_close(locker);  // 释放锁后库必须完好可用
    EXPECT_TRUE(storage.task_exists(id));
    EXPECT_EQ(1, storage.count_all_tasks());
}

// 库内 options_json 非法（外部篡改/旧版本残留）：list 读回必须吞掉解析
// 异常，该行以默认 options 返回而不是向上传播
TEST(TaskStorageFileTest, ListTasksToleratesMalformedOptionsJson) {
    ScopedDbDir dir;
    TaskStorageConfig config;
    config.db_path = dir.db_path().string();
    config.enable_wal_mode = false;
    TaskStorage storage(config);
    ASSERT_TRUE(storage.initialize());
    const TaskId id = storage.create_task(file_test_record());
    ASSERT_NE(INVALID_TASK_ID, id);

    sqlite3* raw = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(dir.db_path().string().c_str(), &raw));
    char* errmsg = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_exec(raw, "UPDATE tasks SET options_json = '{bad json';",
                           nullptr, nullptr, &errmsg))
        << (errmsg ? errmsg : "");
    sqlite3_free(errmsg);
    sqlite3_close(raw);

    const auto tasks = storage.list_tasks();
    ASSERT_EQ(1u, tasks.size());
    EXPECT_EQ(id, tasks[0].id);
    // 解析失败吞掉异常，options 落回默认构造值（而非崩溃/丢行）
    EXPECT_EQ(falcon::DownloadOptions{}.max_connections,
              tasks[0].options.max_connections);
}

// 第二连接 DROP TABLE 后 schema cookie 变化：TaskStorage 连接下次 prepare
// 强制重读 schema 得 "no such table" —— 覆盖各方法 prepare 失败分支
TEST(TaskStorageFileTest, AllMethodsFailWhenTableDropped) {
    ScopedDbDir dir;
    TaskStorageConfig config;
    config.db_path = dir.db_path().string();
    config.enable_wal_mode = false;
    TaskStorage storage(config);
    ASSERT_TRUE(storage.initialize());
    const TaskId id = storage.create_task(file_test_record());
    ASSERT_NE(INVALID_TASK_ID, id);

    sqlite3* raw = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(dir.db_path().string().c_str(), &raw));
    char* errmsg = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_exec(raw, "DROP TABLE tasks;", nullptr, nullptr, &errmsg))
        << (errmsg ? errmsg : "");
    sqlite3_free(errmsg);
    sqlite3_close(raw);

    const TaskRecord record = file_test_record();
    // 首条语句的 prepare_v2 会吞掉一次 SQLITE_SCHEMA 自动重编并走 step
    // 失败收口，因此先用一条查询让 schema cache 失效落到文件真身，
    // 其后 create/get 才能命中各自的 prepare 失败分支
    EXPECT_FALSE(storage.task_exists(id));
    EXPECT_FALSE(storage.get_task(id).has_value());
    EXPECT_EQ(INVALID_TASK_ID, storage.create_task(record));
    EXPECT_FALSE(storage.update_task(id, record));
    EXPECT_FALSE(storage.delete_task(id));
    EXPECT_FALSE(storage.task_exists(id));
    EXPECT_TRUE(storage.list_tasks().empty());
    EXPECT_TRUE(storage.get_tasks_by_status(TaskStatus::Pending).empty());
    EXPECT_TRUE(storage.get_active_tasks().empty());
    EXPECT_EQ(0, storage.count_tasks_by_status(TaskStatus::Pending));
    EXPECT_EQ(0, storage.count_all_tasks());
    EXPECT_FALSE(storage.get_max_task_id().has_value());
    EXPECT_FALSE(storage.update_progress(id, 1, 0.5, 1));
    EXPECT_FALSE(storage.update_status(id, TaskStatus::Paused));
    EXPECT_FALSE(storage.mark_completed(id));
    EXPECT_FALSE(storage.mark_failed(id, "boom"));
    EXPECT_EQ(0, storage.cleanup_completed_tasks(7));
    // vacuum 不引用 tasks 表，schema 损坏下仍可成功——不在此例断言
    EXPECT_NE(std::string::npos, storage.get_last_error().find("no such table"));
}

#endif // FALCON_HAS_SQLITE3
