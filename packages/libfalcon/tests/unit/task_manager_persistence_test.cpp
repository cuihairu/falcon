// Falcon Task Manager Persistence Tests

#include <falcon/task_manager.hpp>
#include <falcon/download_task.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path unique_temp_file(const std::string& prefix) {
    auto base = std::filesystem::temp_directory_path();
    auto name = prefix + std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    return base / name;
}

} // namespace

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

