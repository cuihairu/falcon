/**
 * @file request_group_test.cpp
 * @brief 请求组管理单元测试
 * @author Falcon Team
 * @date 2025-12-24
 */

#ifdef _WIN32
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/request_group.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/resume_control.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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

#ifdef FALCON_ENABLE_OPENSSL
    // HTTPS 协议门禁放行（TLS 支持在连接命令层，init 只做协议检查）
    EXPECT_TRUE(group.init());
    EXPECT_EQ(group.status(), RequestGroupStatus::WAITING);
#else
    // 无 OpenSSL：HTTPS 明确拒绝
    EXPECT_FALSE(group.init());
    EXPECT_EQ(group.status(), RequestGroupStatus::FAILED);
    EXPECT_FALSE(group.error_message().empty());
#endif
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
    // 不受支持协议（HTTPS 在启用 OpenSSL 的构建中已放行，不能再用作
    // 无效协议样本）
    std::vector<std::string> urls = {"ftp://example.com/file.zip"};
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
                TaskId id = static_cast<TaskId>(i * OPERATIONS_PER_THREAD + j + 1);
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
// 终态组回收测试（宿主化常驻引擎依赖）
//==============================================================================

/// purge_finished_groups 回收 COMPLETED/FAILED/REMOVED 组并释放
/// all_groups_ 持有的对象；WAITING/PAUSED 组必须保留——PAUSED 组是
/// 停机恢复的挂点，绝不可被回收
TEST(RequestGroupManTest, PurgeFinishedGroupsReclaimsAndKeepsPaused) {
    RequestGroupMan manager(5);
    DownloadOptions options;

    auto make_group = [&options](TaskId id) {
        return std::make_unique<RequestGroup>(
            id, std::vector<std::string>{
                    "http://example.com/purge-" + std::to_string(id) + ".bin"},
            options);
    };

    manager.add_request_group(make_group(1));  // 将标 COMPLETED
    manager.add_request_group(make_group(2));  // 将标 FAILED
    manager.add_request_group(make_group(3));  // 将经 remove_group 标 REMOVED
    manager.add_request_group(make_group(4));  // 保持 WAITING
    manager.add_request_group(make_group(5));  // 将标 PAUSED

    ASSERT_NE(manager.find_group(1), nullptr);
    manager.find_group(1)->set_status(RequestGroupStatus::COMPLETED);
    ASSERT_NE(manager.find_group(2), nullptr);
    manager.find_group(2)->set_status(RequestGroupStatus::FAILED);
    ASSERT_TRUE(manager.remove_group(3));
    ASSERT_NE(manager.find_group(5), nullptr);
    manager.find_group(5)->set_status(RequestGroupStatus::PAUSED);

    manager.purge_finished_groups();

    // 终态组全部回收：find_group 返回 nullptr 且对象已析构
    EXPECT_EQ(manager.find_group(1), nullptr);
    EXPECT_EQ(manager.find_group(2), nullptr);
    EXPECT_EQ(manager.find_group(3), nullptr);

    // 非终态组保留原状态
    ASSERT_NE(manager.find_group(4), nullptr);
    EXPECT_EQ(manager.find_group(4)->status(), RequestGroupStatus::WAITING);
    ASSERT_NE(manager.find_group(5), nullptr);
    EXPECT_EQ(manager.find_group(5)->status(), RequestGroupStatus::PAUSED);

    // 回收后调度队列不含悬垂指针：WAITING 组仍可被激活计数看到
    EXPECT_EQ(manager.waiting_count(), 2);  // 组 4（WAITING）+ 组 5（PAUSED）
}

//==============================================================================
// 离线路径收口（覆盖率批次 R）：输出路径推导 / 门禁 / 续传校验 / 调度守卫
//==============================================================================

namespace {

std::string make_unique_temp_dir(const std::string& tag) {
    static std::atomic<int> seq{0};
    const auto dir = std::filesystem::path(std::filesystem::temp_directory_path()) /
                     ("falcon_rg_" + tag + "_" + std::to_string(seq.fetch_add(1)));
    std::filesystem::create_directories(dir);
    return dir.string();
}

} // namespace

// URL 文件名推导：查询串不落入文件名
TEST(RequestGroupCovR, DeriveFilenameStripsUrlQuery) {
    DownloadOptions options;
    options.output_directory = make_unique_temp_dir("query");

    RequestGroup group(1, {"http://127.0.0.1:1/pkg.tar.gz?sig=abc"}, options);
    ASSERT_TRUE(group.init());

    const std::filesystem::path out(group.download_task()->output_path());
    EXPECT_EQ(out.filename().string(), "pkg.tar.gz");
}

// URL 以 '?' 段收尾时推导结果为空 → 回退默认名 "download"
TEST(RequestGroupCovR, DeriveFilenameEmptySegmentFallsBackToDownload) {
    DownloadOptions options;
    options.output_directory = make_unique_temp_dir("emptyname");

    RequestGroup group(2, {"http://127.0.0.1:1/?q=1"}, options);
    ASSERT_TRUE(group.init());

    const std::filesystem::path out(group.download_task()->output_path());
    EXPECT_EQ(out.filename().string(), "download");
}

// 自定义目录 + 相对文件名 → 目录拼接（而非丢弃目录）
TEST(RequestGroupCovR, CustomDirJoinsRelativeFilename) {
    const std::string dir = make_unique_temp_dir("custdir");
    DownloadOptions options;
    options.output_directory = dir;
    options.output_filename = "custom.bin";

    RequestGroup group(3, {"http://127.0.0.1:1/anything.bin"}, options);
    ASSERT_TRUE(group.init());

    EXPECT_EQ(std::filesystem::path(group.download_task()->output_path()),
              std::filesystem::path(dir) / "custom.bin");
}

// 代理门禁：socks 系列在 init 阶段明确失败，不静默直连
TEST(RequestGroupCovR, InitRejectsUnsupportedProxy) {
    DownloadOptions options;
    options.proxy = "socks5://127.0.0.1:1080";

    RequestGroup group(4, {"http://127.0.0.1:1/f.bin"}, options);
    EXPECT_FALSE(group.init());
    EXPECT_EQ(group.status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group.error_message().find("代理"), std::string::npos);
    ASSERT_NE(group.download_task(), nullptr);
    EXPECT_EQ(group.download_task()->status(), TaskStatus::Failed);
}

// init 失败经 create_initial_command 收口：命令为空、组保持 FAILED
TEST(RequestGroupCovR, CreateInitialCommandNullWhenInitFails) {
    DownloadOptions options;
    options.proxy = "https://127.0.0.1:8443";

    RequestGroup group(5, {"http://127.0.0.1:1/f.bin"}, options);
    ASSERT_FALSE(group.init());
    EXPECT_EQ(group.create_initial_command(), nullptr);
    EXPECT_EQ(group.status(), RequestGroupStatus::FAILED);
}

// 单段模式（未建立分段跟踪）的段完成查询恒真
TEST(RequestGroupCovR, FinishSegmentSingleSegmentModeReturnsTrue) {
    RequestGroup group(6, {"http://127.0.0.1:1/f.bin"}, {});
    EXPECT_TRUE(group.finish_segment(true));
    EXPECT_TRUE(group.finish_segment(false));
}

// 输出路径辅助方法在任务未初始化时返回空串
TEST(RequestGroupCovR, PathHelpersGuardWithoutDownloadTask) {
    RequestGroup group(7, {"http://127.0.0.1:1/f.bin"}, {});
    EXPECT_EQ(group.temp_file_path(), "");
    EXPECT_EQ(group.control_file_path(), "");

    group.set_temp_extension(".falcon.tmp");
    EXPECT_EQ(group.temp_file_path(), "");  // 仍无任务对象

    DownloadOptions options;
    options.output_directory = make_unique_temp_dir("pathhelpers");
    options.output_filename = "f.bin";
    RequestGroup inited(8, {"http://127.0.0.1:1/f.bin"}, options);
    inited.set_temp_extension(".falcon.tmp");
    ASSERT_TRUE(inited.init());
    ASSERT_NE(inited.download_task(), nullptr);
    EXPECT_EQ(inited.temp_file_path(),
              inited.download_task()->output_path() + ".falcon.tmp");
    EXPECT_EQ(inited.control_file_path(),
              inited.download_task()->output_path() + ".falcon.ctrl");
}

// 跨会话恢复校验：控制文件 URL 与任务不一致 → 放弃续传并删除控制文件
TEST(RequestGroupCovR, ResumeLoadUrlMismatchAbandonsResume) {
    const std::string dir = make_unique_temp_dir("urlmismatch");
    DownloadOptions options;
    options.output_directory = dir;
    options.output_filename = "file.bin";

    const std::string out = (std::filesystem::path(dir) / "file.bin").string();
    const std::string ctrl = out + ".falcon.ctrl";
    const std::string temp = out + ".falcon.tmp";
    {
        std::ofstream f(temp, std::ios::binary);
        f << std::string(50, 'x');
    }
    ResumeControl control;
    control.url = "http://other.example/file.bin";  // 与任务 URL 不一致
    control.total = 100;
    control.segments = {ResumeSegment{/*offset=*/0, /*length=*/100, /*downloaded=*/50}};
    ASSERT_TRUE(save_resume_control(ctrl, control));

    RequestGroup group(9, {"http://127.0.0.1:1/file.bin"}, options);
    group.set_temp_extension(".falcon.tmp");
    ASSERT_TRUE(group.init());  // init 内校验失败 → 放弃续传，按全新下载继续
    EXPECT_FALSE(std::filesystem::exists(ctrl));
    EXPECT_FALSE(group.has_resume_state());
}

// 跨会话恢复校验：临时文件尺寸低于已记录进度 → 数据未完整落盘，放弃续传
TEST(RequestGroupCovR, ResumeLoadTempSizeBelowProgressAbandonsResume) {
    const std::string dir = make_unique_temp_dir("sizemismatch");
    DownloadOptions options;
    options.output_directory = dir;
    options.output_filename = "file.bin";

    const std::string out = (std::filesystem::path(dir) / "file.bin").string();
    const std::string ctrl = out + ".falcon.ctrl";
    const std::string temp = out + ".falcon.tmp";
    {
        std::ofstream f(temp, std::ios::binary);
        f << std::string(10, 'x');  // 低于控制文件记录的 500 字节进度
    }
    ResumeControl control;
    control.url = "http://127.0.0.1:1/file.bin";
    control.total = 1000;
    control.segments = {ResumeSegment{/*offset=*/0, /*length=*/1000, /*downloaded=*/500}};
    ASSERT_TRUE(save_resume_control(ctrl, control));

    RequestGroup group(10, {"http://127.0.0.1:1/file.bin"}, options);
    group.set_temp_extension(".falcon.tmp");
    ASSERT_TRUE(group.init());
    EXPECT_FALSE(std::filesystem::exists(ctrl));
    EXPECT_FALSE(group.has_resume_state());
}

// 控制文件初版写入失败（路径被目录占位）只降级为无断点，不中断下载
TEST(RequestGroupCovR, BeginResumeTrackingToleratesControlSaveFailure) {
    const std::string dir = make_unique_temp_dir("savectrl");
    DownloadOptions options;
    options.output_directory = dir;
    options.output_filename = "file.bin";

    RequestGroup group(11, {"http://127.0.0.1:1/file.bin"}, options);
    group.set_temp_extension(".falcon.tmp");
    ASSERT_TRUE(group.init());

    const std::string ctrl = group.control_file_path();
    std::filesystem::create_directories(ctrl);  // 目录占位 → 写入必败
    group.begin_resume_tracking("http://127.0.0.1:1/file.bin", 1000,
                                "etag-1", "yesterday",
                                {ResumeSegment{/*offset=*/0, /*length=*/1000,
                                               /*downloaded=*/0}});
    EXPECT_TRUE(std::filesystem::is_directory(ctrl));  // 写失败，占位目录原样
    EXPECT_TRUE(group.has_resume_state());  // 内存追踪照常建立
}

// 无续传状态时多分段恢复是幂等空操作
TEST(RequestGroupCovR, PrepareResumedMultiSegmentNoopWithoutResume) {
    RequestGroup group(12, {"http://127.0.0.1:1/f.bin"}, {});
    group.prepare_resumed_multi_segment();
    EXPECT_FALSE(group.is_multi_segment());
}

// 管理器守卫：空指针与重复 id 拒绝入库
TEST(RequestGroupManCovR, AddRejectsNullAndDuplicateId) {
    RequestGroupMan manager(4);
    manager.add_request_group(nullptr);

    auto first = std::make_unique<RequestGroup>(
        42, std::vector<std::string>{"http://127.0.0.1:1/a.bin"}, DownloadOptions{});
    auto* raw = first.get();
    manager.add_request_group(std::move(first));
    ASSERT_EQ(manager.find_group(42), raw);

    auto duplicate = std::make_unique<RequestGroup>(
        42, std::vector<std::string>{"http://127.0.0.1:1/b.bin"}, DownloadOptions{});
    manager.add_request_group(std::move(duplicate));
    EXPECT_EQ(manager.find_group(42), raw);  // 仍是先注册的组
}

// 调度守卫：排队期间被外部置为非等待态的组被调度循环丢弃出队（仍留在
// 组表）；此后 resume 将这个“孤儿”重新入队（两个队列都不在的补插路径）
TEST(RequestGroupManCovR, FillDropsStaleGroupAndResumeRequeuesOrphan) {
    RequestGroupMan manager(4);
    auto group = std::make_unique<RequestGroup>(
        9, std::vector<std::string>{"http://127.0.0.1:1/f.bin"}, DownloadOptions{});
    auto* raw = group.get();
    manager.add_request_group(std::move(group));
    ASSERT_EQ(manager.waiting_count(), 1u);

    raw->set_status(RequestGroupStatus::FAILED);  // 排队期间的外部扰动
    manager.fill_request_group_from_reserver(nullptr);
    EXPECT_EQ(manager.find_group(9), raw);  // 组表保留
    EXPECT_EQ(manager.waiting_count(), 0u);  // 已被丢弃出队

    EXPECT_TRUE(manager.resume_group(9));
    EXPECT_EQ(manager.waiting_count(), 1u);  // 重新入队
}

//==============================================================================
// 主函数
//==============================================================================
