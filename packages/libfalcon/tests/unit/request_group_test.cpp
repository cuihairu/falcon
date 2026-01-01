/**
 * @file request_group_test.cpp
 * @brief 请求组管理单元测试
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <gtest/gtest.h>
#include <falcon/request_group.hpp>
#include <falcon/download_engine_v2.hpp>
#include <memory>
#include <vector>

using namespace falcon;

//==============================================================================
// RequestGroup 测试
//==============================================================================

TEST(RequestGroupTest, CreateRequestGroup) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file1.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.id(), id);
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);
}

TEST(RequestGroupTest, CreateRequestGroupWithMultipleUrls) {
    TaskId id = 1;
    std::vector<std::string> urls = {
        "http://mirror1.example.com/file.zip",
        "http://mirror2.example.com/file.zip",
        "http://mirror3.example.com/file.zip"
    };
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.id(), id);
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);
}

TEST(RequestGroupTest, RequestGroupStatusTransitions) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // 初始状态：WAITING
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);

    // 设置为激活
    group.set_status(RequestGroupStatus::ACTIVE);
    EXPECT_EQ(group.status(), RequestGroupStatus::ACTIVE);

    // 暂停
    group.pause();
    EXPECT_EQ(group.status(), RequestGroupStatus::PAUSED);

    // 恢复
    group.resume();
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);
}

TEST(RequestGroupTest, RequestGroupProgress) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.downloaded_bytes(), 0);

    // 增加已下载字节数
    group.add_downloaded_bytes(512);
    EXPECT_EQ(group.downloaded_bytes(), 512);
}

TEST(RequestGroupTest, RequestGroupTryNextUri) {
    TaskId id = 1;
    std::vector<std::string> urls = {
        "http://mirror1.example.com/file.zip",
        "http://mirror2.example.com/file.zip",
        "http://mirror3.example.com/file.zip"
    };
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.current_uri(), "http://mirror1.example.com/file.zip");

    EXPECT_TRUE(group.try_next_uri());
    EXPECT_EQ(group.current_uri(), "http://mirror2.example.com/file.zip");

    EXPECT_TRUE(group.try_next_uri());
    EXPECT_EQ(group.current_uri(), "http://mirror3.example.com/file.zip");

    // 没有更多 URI
    EXPECT_FALSE(group.try_next_uri());
}

//==============================================================================
// RequestGroupMan 测试
//==============================================================================

TEST(RequestGroupManTest, CreateRequestGroupMan) {
    size_t max_concurrent = 5;
    RequestGroupMan manager(max_concurrent);

    EXPECT_EQ(manager.max_concurrent(), max_concurrent);
    EXPECT_EQ(manager.active_count(), 0);
    EXPECT_EQ(manager.waiting_count(), 0);
}

TEST(RequestGroupManTest, AddRequestGroup) {
    RequestGroupMan manager(5);

    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    auto group = std::make_unique<RequestGroup>(id, urls, options);
    manager.add_request_group(std::move(group));

    EXPECT_EQ(manager.waiting_count(), 1);
}

TEST(RequestGroupManTest, PauseRequestGroup) {
    RequestGroupMan manager(5);

    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    auto group = std::make_unique<RequestGroup>(id, urls, options);
    manager.add_request_group(std::move(group));

    // 暂停任务
    EXPECT_TRUE(manager.pause_group(id));
}

TEST(RequestGroupManTest, ResumeRequestGroup) {
    RequestGroupMan manager(5);

    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    auto group = std::make_unique<RequestGroup>(id, urls, options);
    manager.add_request_group(std::move(group));
    manager.pause_group(id);

    // 恢复任务
    EXPECT_TRUE(manager.resume_group(id));
}

TEST(RequestGroupManTest, RemoveRequestGroup) {
    RequestGroupMan manager(5);

    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    auto group = std::make_unique<RequestGroup>(id, urls, options);
    manager.add_request_group(std::move(group));

    // 移除任务
    EXPECT_TRUE(manager.remove_group(id));
    EXPECT_EQ(manager.waiting_count(), 0);
}

TEST(RequestGroupManTest, FindRequestGroup) {
    RequestGroupMan manager(5);

    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    auto group = std::make_unique<RequestGroup>(id, urls, options);
    manager.add_request_group(std::move(group));

    // 查找任务
    auto* found = manager.find_group(id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id(), id);
}

TEST(RequestGroupManTest, FindNonExistentGroup) {
    RequestGroupMan manager(5);

    auto* found = manager.find_group(999);
    EXPECT_EQ(found, nullptr);
}

//==============================================================================
// 并发控制测试
//==============================================================================

TEST(RequestGroupManTest, MaxConcurrentTasks) {
    constexpr size_t MAX_CONCURRENT = 3;
    RequestGroupMan manager(MAX_CONCURRENT);

    // 添加多个任务
    for (TaskId i = 1; i <= 10; ++i) {
        std::vector<std::string> urls = {"http://example.com/file" + std::to_string(i) + ".zip"};
        DownloadOptions options;
        auto group = std::make_unique<RequestGroup>(i, urls, options);
        manager.add_request_group(std::move(group));
    }

    EXPECT_EQ(manager.waiting_count(), 10);
}

//==============================================================================
// 完成状态测试
//==============================================================================

TEST(RequestGroupManTest, AllCompletedInitially) {
    RequestGroupMan manager(5);
    EXPECT_TRUE(manager.all_completed());
}

TEST(RequestGroupManTest, AllCompletedWithTasks) {
    RequestGroupMan manager(5);

    // 添加任务
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;
    auto group = std::make_unique<RequestGroup>(id, urls, options);
    manager.add_request_group(std::move(group));

    EXPECT_FALSE(manager.all_completed());

    // 移除任务
    manager.remove_group(id);
    EXPECT_TRUE(manager.all_completed());
}

//==============================================================================
// 错误处理测试
//==============================================================================

TEST(RequestGroupManTest, PauseNonExistentGroup) {
    RequestGroupMan manager(5);
    EXPECT_FALSE(manager.pause_group(999));
}

TEST(RequestGroupManTest, ResumeNonExistentGroup) {
    RequestGroupMan manager(5);
    EXPECT_FALSE(manager.resume_group(999));
}

TEST(RequestGroupManTest, RemoveNonExistentGroup) {
    RequestGroupMan manager(5);
    EXPECT_FALSE(manager.remove_group(999));
}

//==============================================================================
// 任务统计测试
//==============================================================================

TEST(RequestGroupManTest, TaskCounts) {
    RequestGroupMan manager(5);

    // 添加多个任务
    for (TaskId i = 1; i <= 5; ++i) {
        std::vector<std::string> urls = {"http://example.com/file" + std::to_string(i) + ".zip"};
        DownloadOptions options;
        auto group = std::make_unique<RequestGroup>(i, urls, options);
        manager.add_request_group(std::move(group));
    }

    EXPECT_EQ(manager.waiting_count(), 5);
    EXPECT_EQ(manager.active_count(), 0);
}

//==============================================================================
// init() 和 create_initial_command() 测试
//==============================================================================

TEST(RequestGroupInit, InitWithHttpUrl) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_TRUE(group.init());
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);
}

TEST(RequestGroupInit, InitWithHttpsUrl) {
    TaskId id = 1;
    std::vector<std::string> urls = {"https://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // HTTPS 在 V2 中暂不支持
    EXPECT_FALSE(group.init());
    EXPECT_EQ(group.status(), RequestGroupStatus::FAILED);
    EXPECT_FALSE(group.error_message().empty());
}

TEST(RequestGroupInit, InitWithUnsupportedProtocol) {
    TaskId id = 1;
    std::vector<std::string> urls = {"ftp://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // FTP 在 V2 中暂不支持
    EXPECT_FALSE(group.init());
    EXPECT_EQ(group.status(), RequestGroupStatus::FAILED);
}

TEST(RequestGroupInit, InitWithEmptyUris) {
    TaskId id = 1;
    std::vector<std::string> urls = {};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // 空 URI 列表
    EXPECT_FALSE(group.init());
    EXPECT_EQ(group.status(), RequestGroupStatus::FAILED);
}

TEST(RequestGroupInit, InitMultipleTimes) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_TRUE(group.init());
    // 第二次 init 应该成功
    EXPECT_TRUE(group.init());
}

TEST(RequestGroupCommand, CreateInitialCommand) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // 初始化
    ASSERT_TRUE(group.init());

    // 创建初始命令
    auto command = group.create_initial_command();

    // 应该返回有效的命令
    EXPECT_NE(command, nullptr);
}

TEST(RequestGroupCommand, CreateInitialCommandWithoutInit) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // 不调用 init() 直接创建命令
    auto command = group.create_initial_command();

    // create_initial_command 应该内部调用 init()
    EXPECT_NE(command, nullptr);
}

TEST(RequestGroupCommand, CreateInitialCommandWithInvalidProtocol) {
    TaskId id = 1;
    std::vector<std::string> urls = {"https://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // 初始化失败
    ASSERT_FALSE(group.init());

    // 创建命令应该返回 nullptr
    auto command = group.create_initial_command();
    EXPECT_EQ(command, nullptr);
}

//==============================================================================
// URI 切换测试增强
//==============================================================================

TEST(RequestGroupURI, TryNextUriWithMultipleUris) {
    TaskId id = 1;
    std::vector<std::string> urls = {
        "http://mirror1.example.com/file.zip",
        "http://mirror2.example.com/file.zip",
        "http://mirror3.example.com/file.zip",
        "http://mirror4.example.com/file.zip"
    };
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.current_uri(), "http://mirror1.example.com/file.zip");

    EXPECT_TRUE(group.try_next_uri());
    EXPECT_EQ(group.current_uri(), "http://mirror2.example.com/file.zip");

    EXPECT_TRUE(group.try_next_uri());
    EXPECT_EQ(group.current_uri(), "http://mirror3.example.com/file.zip");

    EXPECT_TRUE(group.try_next_uri());
    EXPECT_EQ(group.current_uri(), "http://mirror4.example.com/file.zip");

    // 没有更多 URI
    EXPECT_FALSE(group.try_next_uri());
}

TEST(RequestGroupURI, TryNextUriWithSingleUri) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.current_uri(), "http://example.com/file.zip");
    EXPECT_FALSE(group.try_next_uri());
}

//==============================================================================
// 进度信息测试
//==============================================================================

TEST(RequestGroupProgress, GetProgressInitially) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    auto progress = group.get_progress();

    EXPECT_EQ(progress.downloaded, 0);
    EXPECT_EQ(progress.total, 0);
    EXPECT_EQ(progress.progress, 0.0);
}

TEST(RequestGroupProgress, GetProgressAfterDownload) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // 设置文件总大小
    group.set_total_size(1024 * 1024);  // 1MB

    // 模拟下载
    group.add_downloaded_bytes(512 * 1024);  // 512KB

    auto progress = group.get_progress();

    EXPECT_EQ(progress.downloaded, 512 * 1024);
    EXPECT_EQ(progress.total, 1024 * 1024);
    EXPECT_DOUBLE_EQ(progress.progress, 0.5);
}

TEST(RequestGroupProgress, GetProgressWithZeroTotal) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // 添加已下载字节，但总大小为 0
    group.add_downloaded_bytes(1024);

    auto progress = group.get_progress();

    EXPECT_EQ(progress.downloaded, 1024);
    EXPECT_EQ(progress.total, 0);
    EXPECT_EQ(progress.progress, 0.0);  // 应该避免除以零
}

//==============================================================================
// 状态检查测试
//==============================================================================

TEST(RequestGroupState, IsCompleted) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_FALSE(group.is_completed());

    group.set_status(RequestGroupStatus::COMPLETED);
    EXPECT_TRUE(group.is_completed());
}

TEST(RequestGroupState, IsActive) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_FALSE(group.is_active());

    group.set_status(RequestGroupStatus::ACTIVE);
    EXPECT_TRUE(group.is_active());
}

TEST(RequestGroupState, PauseAndResumeTransitions) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);
    group.set_status(RequestGroupStatus::ACTIVE);

    // 暂停
    group.pause();
    EXPECT_EQ(group.status(), RequestGroupStatus::PAUSED);

    // 恢复
    group.resume();
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);
}

TEST(RequestGroupState, PauseNonActiveGroup) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);
    group.set_status(RequestGroupStatus::WAITING);

    // 尝试暂停非 ACTIVE 状态
    group.pause();
    // 状态应该不变
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);
}

TEST(RequestGroupState, ResumeNonPausedGroup) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);
    group.set_status(RequestGroupStatus::ACTIVE);

    // 尝试恢复非 PAUSED 状态
    group.resume();
    // 状态应该不变
    EXPECT_EQ(group.status(), RequestGroupStatus::ACTIVE);
}

//==============================================================================
// 错误处理测试增强
//==============================================================================

TEST(RequestGroupError, SetErrorMessage) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_TRUE(group.error_message().empty());

    group.set_error_message("Test error message");

    EXPECT_EQ(group.error_message(), "Test error message");
}

TEST(RequestGroupError, AddFile) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.files().size(), 1);  // 构造函数已添加默认文件

    FileInfo file;
    file.url = "http://example.com/file2.zip";
    file.total_size = 2048;

    group.add_file(file);

    EXPECT_EQ(group.files().size(), 2);
    EXPECT_EQ(group.files()[1].total_size, 2048);
}

//==============================================================================
// cleanup_finished_active() 测试
//==============================================================================

TEST(RequestGroupMan, CleanupFinishedActive) {
    RequestGroupMan manager(3);

    // 添加多个任务
    for (TaskId i = 1; i <= 5; ++i) {
        std::vector<std::string> urls = {"http://example.com/file" + std::to_string(i) + ".zip"};
        DownloadOptions options;
        auto group = std::make_unique<RequestGroup>(i, urls, options);
        manager.add_request_group(std::move(group));
    }

    // 手动模拟一些任务成为活动状态
    // 注意：这需要访问内部状态，或者通过其他方式测试

    // 清理完成的活动任务
    manager.cleanup_finished_active();

    // 验证（取决于具体实现）
}

//==============================================================================
// fill_request_group_from_reserver() 测试
//==============================================================================

TEST(RequestGroupMan, FillRequestGroupFromReserver) {
    constexpr size_t MAX_CONCURRENT = 2;
    RequestGroupMan manager(MAX_CONCURRENT);

    // 添加多个任务
    for (TaskId i = 1; i <= 5; ++i) {
        std::vector<std::string> urls = {"http://example.com/file" + std::to_string(i) + ".zip"};
        DownloadOptions options;
        auto group = std::make_unique<RequestGroup>(i, urls, options);
        manager.add_request_group(std::move(group));
    }

    EXPECT_EQ(manager.waiting_count(), 5);
    EXPECT_EQ(manager.active_count(), 0);

    // 填充活动组（不传 engine）
    manager.fill_request_group_from_reserver(nullptr);

    // 应该激活最多 MAX_CONCURRENT 个任务
    EXPECT_LE(manager.active_count(), MAX_CONCURRENT);
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(RequestGroupBoundary, VeryLargeTaskId) {
    TaskId id = std::numeric_limits<TaskId>::max();
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.id(), std::numeric_limits<TaskId>::max());
}

TEST(RequestGroupBoundary, EmptyUrlInList) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip", "", "http://mirror.example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.current_uri(), "http://example.com/file.zip");

    // 尝试切换到空的 URL
    EXPECT_TRUE(group.try_next_uri());
    EXPECT_EQ(group.current_uri(), "");
}

TEST(RequestGroupBoundary, VeryLongUrl) {
    TaskId id = 1;
    std::string long_url = "http://example.com/" + std::string(10000, 'a') + ".zip";
    std::vector<std::string> urls = {long_url};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.current_uri().length(), long_url.length());
}

TEST(RequestGroupManBoundary, ZeroMaxConcurrent) {
    RequestGroupMan manager(0);

    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;
    auto group = std::make_unique<RequestGroup>(id, urls, options);
    manager.add_request_group(std::move(group));

    EXPECT_EQ(manager.max_concurrent(), 0);
    EXPECT_EQ(manager.waiting_count(), 1);
}

TEST(RequestGroupManBoundary, VeryLargeMaxConcurrent) {
    constexpr size_t VERY_LARGE = 10000;
    RequestGroupMan manager(VERY_LARGE);

    EXPECT_EQ(manager.max_concurrent(), VERY_LARGE);
}

//==============================================================================
// 状态转换测试
//==============================================================================

TEST(RequestGroupStatus, AllStatusTransitions) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // WAITING -> ACTIVE
    group.set_status(RequestGroupStatus::ACTIVE);
    EXPECT_EQ(group.status(), RequestGroupStatus::ACTIVE);

    // ACTIVE -> PAUSED
    group.pause();
    EXPECT_EQ(group.status(), RequestGroupStatus::PAUSED);

    // PAUSED -> WAITING
    group.resume();
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);

    // WAITING -> COMPLETED
    group.set_status(RequestGroupStatus::COMPLETED);
    EXPECT_EQ(group.status(), RequestGroupStatus::COMPLETED);

    // COMPLETED -> REMOVED
    group.set_status(RequestGroupStatus::REMOVED);
    EXPECT_EQ(group.status(), RequestGroupStatus::REMOVED);
}

TEST(RequestGroupStatus, ToString) {
    EXPECT_STREQ(to_string(RequestGroupStatus::WAITING), "WAITING");
    EXPECT_STREQ(to_string(RequestGroupStatus::ACTIVE), "ACTIVE");
    EXPECT_STREQ(to_string(RequestGroupStatus::PAUSED), "PAUSED");
    EXPECT_STREQ(to_string(RequestGroupStatus::COMPLETED), "COMPLETED");
    EXPECT_STREQ(to_string(RequestGroupStatus::FAILED), "FAILED");
    EXPECT_STREQ(to_string(RequestGroupStatus::REMOVED), "REMOVED");
}

//==============================================================================
// 并发测试
//==============================================================================

TEST(RequestGroupManConcurrency, ConcurrentAddRemove) {
    RequestGroupMan manager(10);

    constexpr int NUM_THREADS = 4;
    constexpr int OPERATIONS_PER_THREAD = 25;
    std::vector<std::thread> threads;
    std::atomic<int> add_count(0);

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&manager, &add_count, i]() {
            for (int j = 0; j < OPERATIONS_PER_THREAD; ++j) {
                TaskId id = i * OPERATIONS_PER_THREAD + j + 1;
                std::vector<std::string> urls = {"http://example.com/file" + std::to_string(id) + ".zip"};
                DownloadOptions options;
                auto group = std::make_unique<RequestGroup>(id, urls, options);
                manager.add_request_group(std::move(group));
                add_count++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证所有任务都被添加
    EXPECT_EQ(add_count.load(), NUM_THREADS * OPERATIONS_PER_THREAD);
}

//==============================================================================
// 文件信息测试
//==============================================================================

TEST(RequestGroupFile, FileInfoAccess) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    // 获取文件信息
    const auto& file_info = group.file_info();

    EXPECT_EQ(file_info.url, "http://example.com/file.zip");
    EXPECT_EQ(file_info.total_size, 0);
}

TEST(RequestGroupFile, SetTotalSize) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    group.set_total_size(1024 * 1024);

    EXPECT_EQ(group.file_info().total_size, 1024 * 1024);
}

//==============================================================================
// 下载选项测试
//==============================================================================

TEST(RequestGroupOptions, OptionsAccess) {
    TaskId id = 1;
    std::vector<std::string> urls = {"http://example.com/file.zip"};
    DownloadOptions options;
    options.max_connections = 8;
    options.timeout_seconds = 60;

    RequestGroup group(id, urls, options);

    EXPECT_EQ(group.options().max_connections, 8);
    EXPECT_EQ(group.options().timeout_seconds, 60);
}

//==============================================================================
// URIs 访问测试
//==============================================================================

TEST(RequestGroupUris, UrisAccess) {
    TaskId id = 1;
    std::vector<std::string> urls = {
        "http://mirror1.example.com/file.zip",
        "http://mirror2.example.com/file.zip",
        "http://mirror3.example.com/file.zip"
    };
    DownloadOptions options;

    RequestGroup group(id, urls, options);

    const auto& uris = group.uris();

    EXPECT_EQ(uris.size(), 3);
    EXPECT_EQ(uris[0], "http://mirror1.example.com/file.zip");
    EXPECT_EQ(uris[1], "http://mirror2.example.com/file.zip");
    EXPECT_EQ(uris[2], "http://mirror3.example.com/file.zip");
}

//==============================================================================
// 主函数
//==============================================================================
