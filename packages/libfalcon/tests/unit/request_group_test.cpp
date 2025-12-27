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
// 主函数
//==============================================================================
