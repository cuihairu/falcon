// Falcon Task Manager Persistence Tests

#include <falcon/task_manager.hpp>
#include <falcon/download_task.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <map>

namespace {

std::filesystem::path unique_temp_file(const std::string& prefix) {
    auto base = std::filesystem::temp_directory_path();
    auto name = prefix + std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    return base / name;
}

} // namespace

//==============================================================================
// 基础持久化测试
//==============================================================================

TEST(TaskManagerPersistenceTest, SaveAndLoadState) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.output_directory = "out";
    opt.output_filename = "file.bin";
    opt.max_connections = 8;
    opt.speed_limit = 1234;
    opt.headers = {{"X-Test", "1"}, {"User", "Alice"}};
    opt.proxy = "http://proxy:8080";
    opt.verify_ssl = false;

    auto t1 = std::make_shared<falcon::DownloadTask>(1, "https://example.com/a.bin", opt);
    t1->set_output_path("out/file.bin");
    t1->set_status(falcon::TaskStatus::Paused);
    tm.add_task(t1);

    auto t2 = std::make_shared<falcon::DownloadTask>(2, "https://example.com/b.bin", falcon::DownloadOptions{});
    t2->set_output_path("b.bin");
    t2->set_error("oops");
    t2->set_status(falcon::TaskStatus::Failed);
    tm.add_task(t2);

    auto state_path = unique_temp_file("falcon_task_state_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto r1 = tm2.get_task(1);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(r1->url(), "https://example.com/a.bin");
    EXPECT_EQ(r1->output_path(), "out/file.bin");
    EXPECT_EQ(r1->status(), falcon::TaskStatus::Paused);
    EXPECT_EQ(r1->options().output_directory, "out");
    EXPECT_EQ(r1->options().output_filename, "file.bin");
    EXPECT_EQ(r1->options().max_connections, 8u);
    EXPECT_EQ(r1->options().speed_limit, 1234u);
    EXPECT_EQ(r1->options().proxy, "http://proxy:8080");
    EXPECT_FALSE(r1->options().verify_ssl);
    EXPECT_EQ(r1->options().headers.at("X-Test"), "1");
    EXPECT_EQ(r1->options().headers.at("User"), "Alice");

    auto r2 = tm2.get_task(2);
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(r2->url(), "https://example.com/b.bin");
    EXPECT_EQ(r2->output_path(), "b.bin");
    EXPECT_EQ(r2->status(), falcon::TaskStatus::Failed);
    EXPECT_EQ(r2->error_message(), "oops");
}

//==============================================================================
// 任务状态持久化测试
//==============================================================================

TEST(TaskManagerPersistenceStates, PendingTask) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    task->set_status(falcon::TaskStatus::Pending);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_pending_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->status(), falcon::TaskStatus::Pending);
}

TEST(TaskManagerPersistenceStates, DownloadingTask) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    task->set_status(falcon::TaskStatus::Downloading);
    task->update_progress(500, 1000, 1024);  // 50% progress, 1 MB/s
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_downloading_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->status(), falcon::TaskStatus::Downloading);
    EXPECT_FLOAT_EQ(loaded->progress(), 0.5f);
}

TEST(TaskManagerPersistenceStates, PausedTask) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    task->set_status(falcon::TaskStatus::Paused);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_paused_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->status(), falcon::TaskStatus::Paused);
}

TEST(TaskManagerPersistenceStates, CompletedTask) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    task->set_status(falcon::TaskStatus::Completed);
    task->update_progress(1000, 1000, 0);  // 100% progress
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_completed_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->status(), falcon::TaskStatus::Completed);
    EXPECT_FLOAT_EQ(loaded->progress(), 1.0f);
}

TEST(TaskManagerPersistenceStates, FailedTask) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    task->set_status(falcon::TaskStatus::Failed);
    task->set_error("Connection timeout");
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_failed_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->status(), falcon::TaskStatus::Failed);
    EXPECT_EQ(loaded->error_message(), "Connection timeout");
}

TEST(TaskManagerPersistenceStates, CancelledTask) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    task->set_status(falcon::TaskStatus::Cancelled);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_cancelled_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->status(), falcon::TaskStatus::Cancelled);
}

//==============================================================================
// 下载选项持久化测试
//==============================================================================

TEST(TaskManagerPersistenceOptions, MaxConnections) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.max_connections = 16;

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_max_conn_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().max_connections, 16u);
}

TEST(TaskManagerPersistenceOptions, SpeedLimit) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.speed_limit = 1024 * 1024;  // 1 MB/s

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_speed_limit_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().speed_limit, 1024u * 1024u);
}

TEST(TaskManagerPersistenceOptions, Timeout) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.timeout_seconds = 60;

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_timeout_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().timeout_seconds, 60u);
}

TEST(TaskManagerPersistenceOptions, RetryOptions) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.max_retries = 10;
    opt.retry_delay_seconds = 5;

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_retry_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().max_retries, 10u);
    EXPECT_EQ(loaded->options().retry_delay_seconds, 5u);
}

TEST(TaskManagerPersistenceOptions, UserAgent) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.user_agent = "Falcon/1.0 CustomAgent";

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_user_agent_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().user_agent, "Falcon/1.0 CustomAgent");
}

TEST(TaskManagerPersistenceOptions, CustomHeaders) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.headers = {
        {"Authorization", "Bearer token123"},
        {"Accept", "application/json"},
        {"X-Custom-Header", "custom-value"}
    };

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_headers_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().headers.size(), 3u);
    EXPECT_EQ(loaded->options().headers.at("Authorization"), "Bearer token123");
    EXPECT_EQ(loaded->options().headers.at("Accept"), "application/json");
    EXPECT_EQ(loaded->options().headers.at("X-Custom-Header"), "custom-value");
}

TEST(TaskManagerPersistenceOptions, ProxyOptions) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.proxy = "http://proxy.example.com:8080";
    opt.proxy_username = "user";
    opt.proxy_password = "pass";

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_proxy_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().proxy, "http://proxy.example.com:8080");
    EXPECT_EQ(loaded->options().proxy_username, "user");
    EXPECT_EQ(loaded->options().proxy_password, "pass");
}

TEST(TaskManagerPersistenceOptions, SslOptions) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.verify_ssl = false;

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_ssl_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_FALSE(loaded->options().verify_ssl);
}

TEST(TaskManagerPersistenceOptions, OutputPaths) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.output_directory = "/downloads";
    opt.output_filename = "custom_name.bin";

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_output_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().output_directory, "/downloads");
    EXPECT_EQ(loaded->options().output_filename, "custom_name.bin");
}

//==============================================================================
// 多任务持久化测试
//==============================================================================

TEST(TaskManagerPersistenceMultiple, MultipleTasks) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    for (int i = 1; i <= 10; i++) {
        auto task = std::make_shared<falcon::DownloadTask>(
            i, "https://example.com/file" + std::to_string(i) + ".bin",
            falcon::DownloadOptions{});
        task->set_status(falcon::TaskStatus::Pending);
        tm.add_task(task);
    }

    auto state_path = unique_temp_file("falcon_multiple_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    for (int i = 1; i <= 10; i++) {
        auto loaded = tm2.get_task(i);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->status(), falcon::TaskStatus::Pending);
    }
}

TEST(TaskManagerPersistenceMultiple, MixedStatusTasks) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto t1 = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file1.bin", falcon::DownloadOptions{});
    t1->set_status(falcon::TaskStatus::Pending);
    tm.add_task(t1);

    auto t2 = std::make_shared<falcon::DownloadTask>(
        2, "https://example.com/file2.bin", falcon::DownloadOptions{});
    t2->set_status(falcon::TaskStatus::Downloading);
    t2->update_progress(300, 1000, 1024);  // 30% progress
    tm.add_task(t2);

    auto t3 = std::make_shared<falcon::DownloadTask>(
        3, "https://example.com/file3.bin", falcon::DownloadOptions{});
    t3->set_status(falcon::TaskStatus::Paused);
    tm.add_task(t3);

    auto t4 = std::make_shared<falcon::DownloadTask>(
        4, "https://example.com/file4.bin", falcon::DownloadOptions{});
    t4->set_status(falcon::TaskStatus::Completed);
    tm.add_task(t4);

    auto t5 = std::make_shared<falcon::DownloadTask>(
        5, "https://example.com/file5.bin", falcon::DownloadOptions{});
    t5->set_status(falcon::TaskStatus::Failed);
    t5->set_error("Network error");
    tm.add_task(t5);

    auto state_path = unique_temp_file("falcon_mixed_status_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto l1 = tm2.get_task(1);
    EXPECT_EQ(l1->status(), falcon::TaskStatus::Pending);

    auto l2 = tm2.get_task(2);
    EXPECT_EQ(l2->status(), falcon::TaskStatus::Downloading);
    EXPECT_FLOAT_EQ(l2->progress(), 0.3f);

    auto l3 = tm2.get_task(3);
    EXPECT_EQ(l3->status(), falcon::TaskStatus::Paused);

    auto l4 = tm2.get_task(4);
    EXPECT_EQ(l4->status(), falcon::TaskStatus::Completed);

    auto l5 = tm2.get_task(5);
    EXPECT_EQ(l5->status(), falcon::TaskStatus::Failed);
    EXPECT_EQ(l5->error_message(), "Network error");
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(TaskManagerPersistenceBoundary, EmptyTaskList) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto state_path = unique_temp_file("falcon_empty_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    EXPECT_EQ(tm2.get_all_tasks().size(), 0u);
}

TEST(TaskManagerPersistenceBoundary, VeryLongUrl) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    std::string long_url = "https://example.com/";
    for (int i = 0; i < 100; i++) {
        long_url += "very/long/path/";
    }
    long_url += "file.bin";

    auto task = std::make_shared<falcon::DownloadTask>(
        1, long_url, falcon::DownloadOptions{});
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_long_url_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->url(), long_url);
}

TEST(TaskManagerPersistenceBoundary, SpecialCharactersInUrl) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    std::string url = "https://example.com/path with spaces/file%20name.bin?param=value&other=123";

    auto task = std::make_shared<falcon::DownloadTask>(
        1, url, falcon::DownloadOptions{});
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_special_url_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->url(), url);
}

TEST(TaskManagerPersistenceBoundary, ManyHeaders) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    for (int i = 0; i < 50; i++) {
        opt.headers["X-Custom-" + std::to_string(i)] = "value" + std::to_string(i);
    }

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_many_headers_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().headers.size(), 50u);
}

TEST(TaskManagerPersistenceBoundary, EmptyHeaders) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.headers.clear();

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_empty_headers_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().headers.size(), 0u);
}

//==============================================================================
// 错误处理测试
//==============================================================================

TEST(TaskManagerPersistenceError, InvalidStateFile) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto state_path = unique_temp_file("falcon_invalid_");
    // Create invalid file
    std::ofstream(state_path.string()) << "invalid content";

    EXPECT_FALSE(tm.load_state(state_path.string()));
}

TEST(TaskManagerPersistenceError, NonExistentFile) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    EXPECT_FALSE(tm.load_state("/nonexistent/path/to/file.json"));
}

TEST(TaskManagerPersistenceError, CorruptedStateFile) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_corrupted_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    // Corrupt the file
    std::ofstream(state_path.string(), std::ios::app) << "corruption";

    falcon::TaskManager tm2(cfg, nullptr);
    EXPECT_FALSE(tm2.load_state(state_path.string()));
}

//==============================================================================
// 进度信息持久化测试
//==============================================================================

TEST(TaskManagerPersistenceProgress, DownloadedBytes) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    task->update_progress(1024 * 1024, 10 * 1024 * 1024, 0);  // 1 MB / 10 MB
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_bytes_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->downloaded_bytes(), 1024u * 1024u);
    EXPECT_EQ(loaded->total_bytes(), 10u * 1024u * 1024u);
}

TEST(TaskManagerPersistenceProgress, SpeedInfo) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", falcon::DownloadOptions{});
    task->update_progress(0, 0, 5 * 1024 * 1024);  // 5 MB/s
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_speed_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->speed(), 5u * 1024u * 1024u);
}

//==============================================================================
// 断点续传持久化测试
//==============================================================================

TEST(TaskManagerPersistenceResume, ResumeEnabled) {
    falcon::TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);

    falcon::TaskManager tm(cfg, nullptr);

    falcon::DownloadOptions opt;
    opt.resume_enabled = true;

    auto task = std::make_shared<falcon::DownloadTask>(
        1, "https://example.com/file.bin", opt);
    task->update_progress(1024 * 1024, 0, 0);
    tm.add_task(task);

    auto state_path = unique_temp_file("falcon_resume_");
    ASSERT_TRUE(tm.save_state(state_path.string()));

    falcon::TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(1);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->options().resume_enabled);
    EXPECT_EQ(loaded->downloaded_bytes(), 1024u * 1024u);
}

//==============================================================================
// 主函数
//==============================================================================

