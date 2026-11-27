/**
 * @file incremental_download_test_part3.cpp
 * @brief 增量下载功能测试
 * @author Falcon Team
 * @date 2025-12-31
 */

#include <csignal>
#include <gtest/gtest.h>
#include <falcon/protocols/incremental_download.hpp>
#include <falcon/detail/injection.hpp>
#include <falcon/types.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <random>
#include <chrono>
#include <cstddef>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// Windows 缺少 POSIX socket 语义的符号，测试服务器代码统一走这些别名
#include <cstddef> // std::ptrdiff_t（MSVC 不经其他头传递提供）
using ssize_t = std::ptrdiff_t;
#define SHUT_WR SD_SEND
#define CLOSE_SOCKET(fd) closesocket(fd)
// socket()/bind() 前必须初始化 Winsock，否则 socket() 返回 INVALID_SOCKET

TEST_F(IncrementalDownloadTest, PlanRsyncDegenerateInputsFallToFullDownload) {
    const std::string local = createTestFile("rsync_degen_local.bin", 4096);
    IncrementalDownloader downloader;

    RsyncBlockList bad_size;
    bad_size.blockSize = 0;
    bad_size.fileSize = 4096;
    bad_size.fileHash = "deadbeef";
    bad_size.blocks = {};
    auto plan = downloader.planRsync(local, bad_size);
    ASSERT_EQ(plan.segments.size(), std::size_t{1});
    EXPECT_EQ(plan.segments[0].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.downloadBytes, 4096u);

    RsyncBlockList too_big;
    too_big.blockSize = 17ull * 1024 * 1024;
    too_big.fileSize = 4096;
    too_big.blocks = {};
    plan = downloader.planRsync(local, too_big);
    ASSERT_EQ(plan.segments.size(), std::size_t{1});

    RsyncBlockList zero_file;
    zero_file.blockSize = 1024;
    zero_file.fileSize = 0;
    zero_file.blocks = {};
    plan = downloader.planRsync(local, zero_file);
    ASSERT_EQ(plan.segments.size(), std::size_t{1});
}

// planRsync：远端尾部（不足 blockSize）恒为下载段（1178）。
TEST_F(IncrementalDownloadTest, PlanRsyncTrailingTailIsDownloadSegment) {
    const std::string content = make_rsync_remote(11);
    const std::string remote_path = testDir_ + "/rsync_tail_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    const std::string local_path = testDir_ + "/rsync_tail_local.bin";
    {
        std::ofstream file(local_path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(remote_path, 1024, list));

    const RsyncPlan plan = downloader.planRsync(local_path, list);
    // 完全相同的本地/远端 → 全部 LocalCopy + 尾巴下载段
    ASSERT_GE(plan.segments.size(), std::size_t{2});
    EXPECT_EQ(plan.segments.back().kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments.back().length, 100u);  // 5*1024+100 的尾巴
    EXPECT_EQ(plan.copyBytes, 5u * 1024);
    EXPECT_EQ(plan.downloadBytes, 100u);
}

// planRsync：空本地文件 → 全部下载（含尾巴），无拷贝。
TEST_F(IncrementalDownloadTest, PlanRsyncEmptyLocalAllDownload) {
    const std::string content = make_rsync_remote(12);
    const std::string remote_path = testDir_ + "/rsync_el_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    const std::string local_path = testDir_ + "/rsync_el_local.bin";
    {
        std::ofstream file(local_path, std::ios::binary);
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(remote_path, 1024, list));

    const RsyncPlan plan = downloader.planRsync(local_path, list);
    ASSERT_GE(plan.segments.size(), std::size_t{2});
    EXPECT_EQ(plan.copyBytes, 0u);
    EXPECT_EQ(plan.downloadBytes, content.size());
    EXPECT_EQ(plan.segments.front().kind, RsyncSegment::Kind::Download);
}
