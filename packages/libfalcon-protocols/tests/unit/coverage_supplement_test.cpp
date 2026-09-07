/**
 * @file coverage_supplement_test.cpp
 * @brief libfalcon-protocols 覆盖率补充测试
 * @author Falcon Team
 * @date 2026-09-05
 *
 * 覆盖目标（现有测试因编译宏或私有性未触达的公共 API 路径）：
 * - FileHasher: verify / verify_multiple / HashVerifyCommand 成功路径
 * - IncrementalDownloader: downloadChanged / applyPatch / 空文件哈希列表
 * - SocketPool / PooledSocket: 移动语义、无效连接清理、超限驱逐、统计
 * - SegmentDownloader: calculate_optimal_segments 边界分支
 * 全部离线运行，不依赖网络。
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#define CLOSE_SOCKET(fd) closesocket(fd)
#else
#include <unistd.h>
#include <sys/socket.h>
#define CLOSE_SOCKET(fd) close(fd)
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/file_hash.hpp>
#include <falcon/protocols/incremental_download.hpp>
#include <falcon/protocols/net/socket_pool.hpp>
#include <falcon/protocols/segment_downloader.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace falcon;

namespace {

std::string make_temp_dir() {
    static std::atomic<unsigned> counter{0};
    auto base = std::filesystem::temp_directory_path();
    auto unique = "falcon_supp_" +
                  std::to_string(static_cast<long long>(
                      std::chrono::steady_clock::now().time_since_epoch().count())) +
                  "_" + std::to_string(counter.fetch_add(1));
    auto dir = (base / unique).string();
    std::filesystem::create_directories(dir);
    return dir;
}

std::string write_temp_file(const std::string& dir, const std::string& name,
                            const std::string& content) {
    const std::string path = dir + "/" + name;
    std::ofstream file(path, std::ios::binary);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    return path;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

std::string to_upper_hex(const std::string& hash) {
    std::string upper;
    upper.reserve(hash.size());
    for (char c : hash) {
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return upper;
}

int make_raw_socket_fd() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
    return static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
}

net::SocketKey make_pool_key(const std::string& host, uint16_t port) {
    net::SocketKey key;
    key.host = host;
    key.port = port;
    return key;
}

} // namespace

//==============================================================================
// FileHasher 补充测试（file_hash.cpp）
//==============================================================================

TEST(FileHasherSupplement, VerifyWithCalculatedHashSucceeds) {
    const std::string dir = make_temp_dir();
    const std::string content = "Hello, World!";
    const std::string path = write_temp_file(dir, "verify_ok.bin", content);

    const std::string expected =
        FileHasher::calculate(content.data(), content.size(), HashAlgorithm::MD5);
    ASSERT_FALSE(expected.empty());

    auto result = FileHasher::verify(path, expected, HashAlgorithm::MD5);
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.calculated, expected);
    EXPECT_EQ(result.algorithm, HashAlgorithm::MD5);
}

TEST(FileHasherSupplement, VerifyIsCaseInsensitive) {
    const std::string dir = make_temp_dir();
    const std::string path = write_temp_file(dir, "verify_case.bin", "case-test");

    const std::string expected =
        FileHasher::calculate(path, HashAlgorithm::SHA256);
    ASSERT_EQ(expected.size(), 64U);

    auto result = FileHasher::verify(path, to_upper_hex(expected),
                                     HashAlgorithm::SHA256);
    EXPECT_TRUE(result.valid);
}

TEST(FileHasherSupplement, VerifyLengthMismatchFails) {
    const std::string dir = make_temp_dir();
    const std::string path = write_temp_file(dir, "verify_len.bin", "data");

    // 期望哈希长度与实际不同 → 直接判定失败
    auto result = FileHasher::verify(path, "short", HashAlgorithm::MD5);
    EXPECT_FALSE(result.valid);
}

TEST(FileHasherSupplement, VerifyMultipleAlgorithms) {
    const std::string dir = make_temp_dir();
    const std::string content = "multi-algo content";
    const std::string path = write_temp_file(dir, "multi.bin", content);

    std::vector<std::pair<std::string, HashAlgorithm>> expectations = {
        {FileHasher::calculate(content.data(), content.size(), HashAlgorithm::MD5),
         HashAlgorithm::MD5},
        {FileHasher::calculate(content.data(), content.size(), HashAlgorithm::SHA1),
         HashAlgorithm::SHA1},
        {FileHasher::calculate(content.data(), content.size(), HashAlgorithm::SHA256),
         HashAlgorithm::SHA256},
        {FileHasher::calculate(content.data(), content.size(), HashAlgorithm::SHA512),
         HashAlgorithm::SHA512},
    };

    auto results = FileHasher::verify_multiple(path, expectations);
    ASSERT_EQ(results.size(), 4U);
    for (const auto& r : results) {
        EXPECT_TRUE(r.valid);
    }
}

TEST(FileHasherSupplement, VerifyMultipleMixedResults) {
    const std::string dir = make_temp_dir();
    const std::string content = "mixed content";
    const std::string path = write_temp_file(dir, "mixed.bin", content);

    std::vector<std::pair<std::string, HashAlgorithm>> expectations = {
        {"wrong-hash-length", HashAlgorithm::MD5},
        {FileHasher::calculate(content.data(), content.size(), HashAlgorithm::SHA1),
         HashAlgorithm::SHA1},
    };

    auto results = FileHasher::verify_multiple(path, expectations);
    ASSERT_EQ(results.size(), 2U);
    EXPECT_FALSE(results[0].valid);
    EXPECT_TRUE(results[1].valid);
}

TEST(FileHasherSupplement, DetectAlgorithmByLength) {
    EXPECT_EQ(FileHasher::detect_algorithm(std::string(32, 'a')),
              HashAlgorithm::MD5);
    EXPECT_EQ(FileHasher::detect_algorithm(std::string(40, 'a')),
              HashAlgorithm::SHA1);
    EXPECT_EQ(FileHasher::detect_algorithm(std::string(64, 'a')),
              HashAlgorithm::SHA256);
    EXPECT_EQ(FileHasher::detect_algorithm(std::string(128, 'a')),
              HashAlgorithm::SHA512);
    // 未知长度回落到默认 SHA256
    EXPECT_EQ(FileHasher::detect_algorithm("odd"), HashAlgorithm::SHA256);
}

TEST(FileHasherSupplement, HashVerifyCommandSuccess) {
    const std::string dir = make_temp_dir();
    const std::string content = "command content";
    const std::string path = write_temp_file(dir, "cmd.bin", content);

    const std::string expected =
        FileHasher::calculate(content.data(), content.size(), HashAlgorithm::SHA256);

    HashVerifyCommand cmd(path, expected, HashAlgorithm::SHA256);
    EXPECT_TRUE(cmd.execute());
    EXPECT_TRUE(cmd.get_result().valid);
    EXPECT_EQ(cmd.get_result().expected, expected);
}

TEST(FileHasherSupplement, HashVerifyCommandNonExistentFileFails) {
    const std::string dir = make_temp_dir();
    HashVerifyCommand cmd(dir + "/no_such_file.bin", "whatever",
                          HashAlgorithm::MD5);
    EXPECT_FALSE(cmd.execute());
    EXPECT_FALSE(cmd.get_result().valid);
}

//==============================================================================
// IncrementalDownloader 补充测试（incremental_download.cpp）
//==============================================================================

TEST(IncrementalDownloadSupplement, DownloadChangedNoChangesWritesLocalData) {
    const std::string dir = make_temp_dir();
    const std::string local = write_temp_file(dir, "local.bin", "ABCDEF");

    FileDiff diff;
    diff.localPath = local;
    diff.remotePath = "http://127.0.0.1/remote.bin";
    diff.localSize = 6;
    diff.remoteSize = 6;
    diff.chunks = {{0, 6, "hash0", false}};
    diff.totalChanged = 0;
    diff.ratio = 0.0;

    uint64_t callback_count = 0;
    auto callback = [&callback_count](uint64_t, uint64_t) { callback_count++; };

    IncrementalDownloader downloader;
    const std::string output = dir + "/merged.bin";
    EXPECT_TRUE(downloader.downloadChanged(diff, output, callback));
    EXPECT_EQ(callback_count, 0U);  // 无变化分块 → 不触发回调
    EXPECT_EQ(read_file(output), "ABCDEF");
}

TEST(IncrementalDownloadSupplement, DownloadChangedChunkFetchFailureReturnsFalse) {
    const std::string dir = make_temp_dir();
    const std::string local = write_temp_file(dir, "local2.bin", "ABCDEF");

    FileDiff diff;
    diff.localPath = local;
    diff.remotePath = "http://127.0.0.1/remote.bin";
    diff.localSize = 6;
    diff.remoteSize = 6;
    diff.chunks = {{0, 6, "hash0", true}};  // 标记变化 → 触发 Range 下载（未实现，返回空）
    diff.totalChanged = 6;
    diff.ratio = 1.0;

    IncrementalDownloader downloader;
    EXPECT_FALSE(downloader.downloadChanged(diff, dir + "/out.bin"));
}

TEST(IncrementalDownloadSupplement, DownloadChangedOutputOpenFailureReturnsFalse) {
    const std::string dir = make_temp_dir();
    const std::string local = write_temp_file(dir, "local3.bin", "AB");

    FileDiff diff;
    diff.localPath = local;
    diff.remotePath = "http://127.0.0.1/remote.bin";
    diff.localSize = 2;
    diff.remoteSize = 2;
    diff.chunks = {{0, 2, "hash0", false}};
    diff.totalChanged = 0;

    IncrementalDownloader downloader;
    // 输出路径所在目录不存在 → 打开输出文件失败
    EXPECT_FALSE(downloader.downloadChanged(diff, dir + "/no_dir/out.bin"));
}

TEST(IncrementalDownloadSupplement, DownloadChangedZeroSizeOutput) {
    const std::string dir = make_temp_dir();
    const std::string local = write_temp_file(dir, "local4.bin", "");

    FileDiff diff;
    diff.localPath = local;
    diff.remotePath = "http://127.0.0.1/remote.bin";
    diff.localSize = 0;
    diff.remoteSize = 0;
    diff.totalChanged = 0;

    IncrementalDownloader downloader;
    const std::string output = dir + "/empty_out.bin";
    EXPECT_TRUE(downloader.downloadChanged(diff, output));
    EXPECT_TRUE(read_file(output).empty());
}

TEST(IncrementalDownloadSupplement, ApplyPatchRewritesFileToRemoteSize) {
    const std::string dir = make_temp_dir();
    const std::string local = write_temp_file(dir, "patch.bin", "ABCDEF");

    FileDiff diff;
    diff.localPath = local;
    diff.remotePath = "http://127.0.0.1/remote.bin";
    diff.localSize = 6;
    diff.remoteSize = 8;

    IncrementalDownloader downloader;
    EXPECT_TRUE(downloader.applyPatch(local, "patch-data", diff));
    EXPECT_EQ(read_file(local).size(), 8U);
    EXPECT_EQ(read_file(local).substr(0, 6), "ABCDEF");
}

TEST(IncrementalDownloadSupplement, ApplyPatchWriteFailureReturnsFalse) {
    FileDiff diff;
    diff.localPath = "/nonexistent_falcon_dir/patch.bin";
    diff.remotePath = "http://127.0.0.1/remote.bin";
    diff.localSize = 4;
    diff.remoteSize = 4;

    IncrementalDownloader downloader;
    EXPECT_FALSE(downloader.applyPatch(diff.localPath, "patch", diff));
}

TEST(IncrementalDownloadSupplement, GenerateHashListForEmptyFile) {
    const std::string dir = make_temp_dir();
    const std::string empty = write_temp_file(dir, "empty.bin", "");

    IncrementalDownloader downloader;
    auto chunks = downloader.generateHashList(empty, 1024);
    EXPECT_TRUE(chunks.empty());
}

TEST(IncrementalDownloadSupplement, GenerateHashListSingleByteFile) {
    const std::string dir = make_temp_dir();
    const std::string path = write_temp_file(dir, "one.bin", "X");

    IncrementalDownloader downloader;
    auto chunks = downloader.generateHashList(path, 1024);
    ASSERT_EQ(chunks.size(), 1U);
    EXPECT_EQ(chunks[0].offset, 0U);
    EXPECT_EQ(chunks[0].size, 1U);
    EXPECT_FALSE(chunks[0].hash.empty());
}

//==============================================================================
// SocketPool / PooledSocket 补充测试（socket_pool.hpp）
//==============================================================================

TEST(PooledSocketSupplement, MoveSemanticsTransferFd) {
    const int fd = make_raw_socket_fd();
    ASSERT_GE(fd, 0);

    auto key = make_pool_key("move.example.com", 80);
    net::PooledSocket original(fd, key);

    net::PooledSocket moved_constructed(std::move(original));
    EXPECT_EQ(original.fd(), -1);
    EXPECT_EQ(moved_constructed.fd(), fd);
    EXPECT_EQ(moved_constructed.key().to_string(), "move.example.com:80");

    net::PooledSocket assign_target(-1, make_pool_key("other.example.com", 81));
    assign_target = std::move(moved_constructed);
    EXPECT_EQ(moved_constructed.fd(), -1);
    EXPECT_EQ(assign_target.fd(), fd);
    // assign_target 析构时关闭 fd
}

TEST(PooledSocketSupplement, InvalidFdIsNotValid) {
    auto key = make_pool_key("invalid.example.com", 80);
    net::PooledSocket invalid_socket(-1, key);

    EXPECT_FALSE(invalid_socket.is_valid());
    invalid_socket.touch();   // 不应崩溃
    EXPECT_GE(invalid_socket.idle_time().count(), 0);
    invalid_socket.close_fd();  // fd < 0：无操作
    EXPECT_EQ(invalid_socket.fd(), -1);
}

TEST(SocketPoolSupplement, ReleaseNullIsNoOp) {
    net::SocketPool pool(std::chrono::seconds(30), 4);
    pool.release(nullptr);
    EXPECT_EQ(pool.size(), 0U);
}

TEST(SocketPoolSupplement, AcquireDropsInvalidSockets) {
    net::SocketPool pool(std::chrono::seconds(30), 4);
    auto key = make_pool_key("invalid-pool.example.com", 80);

    // fd 无效的连接进入池中：acquire 应清理并返回空
    pool.release(std::make_shared<net::PooledSocket>(-1, key));
    EXPECT_EQ(pool.size(), 1U);

    auto acquired = pool.acquire(key);
    EXPECT_EQ(acquired, nullptr);
    EXPECT_EQ(pool.size(), 0U);
}

TEST(SocketPoolSupplement, ReleaseBeyondMaxIdleEvictsOldest) {
    net::SocketPool pool(std::chrono::seconds(30), /*max_idle=*/1);
    auto key = make_pool_key("evict.example.com", 80);

    int fd1 = make_raw_socket_fd();
    int fd2 = make_raw_socket_fd();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    pool.release(std::make_shared<net::PooledSocket>(fd1, key));
    EXPECT_EQ(pool.size(), 1U);

    // 第二个连接入池后超过 max_idle=1：最老的连接被驱逐
    pool.release(std::make_shared<net::PooledSocket>(fd2, key));
    EXPECT_EQ(pool.size(), 1U);
}

TEST(SocketPoolSupplement, GetStatsCountsConnections) {
    net::SocketPool pool(std::chrono::seconds(30), 8);
    auto key = make_pool_key("stats.example.com", 80);

    int fd1 = make_raw_socket_fd();
    int fd2 = make_raw_socket_fd();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    pool.release(std::make_shared<net::PooledSocket>(fd1, key));
    pool.release(std::make_shared<net::PooledSocket>(fd2, key));

    auto stats = pool.get_stats();
    EXPECT_EQ(stats.total_connections, 2U);
    // 未连接的裸 socket 对 is_valid() 可能返回 true/false，仅验证统计一致性
    EXPECT_LE(stats.idle_connections, stats.total_connections);
}

TEST(SocketPoolSupplement, ClearThenReuse) {
    net::SocketPool pool(std::chrono::seconds(30), 8);
    auto key = make_pool_key("clear.example.com", 80);

    int fd = make_raw_socket_fd();
    ASSERT_GE(fd, 0);
    pool.release(std::make_shared<net::PooledSocket>(fd, key));
    EXPECT_EQ(pool.size(), 1U);

    pool.clear();
    EXPECT_EQ(pool.size(), 0U);

    auto acquired = pool.acquire(key);
    EXPECT_EQ(acquired, nullptr);
}

//==============================================================================
// SegmentDownloader::calculate_optimal_segments 补充测试
//==============================================================================

TEST(SegmentOptimalCountSupplement, ZeroFileSizeYieldsSingleSegment) {
    SegmentConfig config;
    EXPECT_EQ(SegmentDownloader::calculate_optimal_segments(0, config), 1U);
}

TEST(SegmentOptimalCountSupplement, SmallFileNotSplit) {
    SegmentConfig config;
    const Bytes small_file = config.min_file_size - 1;
    EXPECT_EQ(SegmentDownloader::calculate_optimal_segments(small_file, config), 1U);
}

TEST(SegmentOptimalCountSupplement, ConnectionsClampedToValidRange) {
    SegmentConfig config;
    config.num_connections = 100;  // 超出 max_segments 上限

    const Bytes file_size = 10ULL * 1024 * 1024;  // 10MB
    const std::size_t max_segments =
        static_cast<std::size_t>(file_size / config.min_segment_size);  // 10
    const std::size_t result =
        SegmentDownloader::calculate_optimal_segments(file_size, config);

    EXPECT_EQ(result, max_segments);
    EXPECT_LE(result, 10U);
}

TEST(SegmentOptimalCountSupplement, ExplicitConnectionsInRangeUsed) {
    SegmentConfig config;
    config.num_connections = 4;

    const Bytes file_size = 10ULL * 1024 * 1024;  // 10MB
    EXPECT_EQ(SegmentDownloader::calculate_optimal_segments(file_size, config), 4U);
}

TEST(SegmentOptimalCountSupplement, AutoDetectDefaultsToFourSegments) {
    SegmentConfig config;
    config.num_connections = 0;  // 自动检测

    const Bytes file_size = 10ULL * 1024 * 1024;  // 10MB
    EXPECT_EQ(SegmentDownloader::calculate_optimal_segments(file_size, config), 4U);
}

TEST(SegmentOptimalCountSupplement, AutoDetectCappedByMaxSegments) {
    SegmentConfig config;
    config.num_connections = 0;
    config.min_segment_size = 2ULL * 1024 * 1024;  // 2MB → max 5 segments (10MB)

    const Bytes file_size = 10ULL * 1024 * 1024;
    EXPECT_EQ(SegmentDownloader::calculate_optimal_segments(file_size, config), 4U);

    // 极小 min_segment_size：仍被 clamp 到最多 8
    SegmentConfig tiny;
    tiny.num_connections = 0;
    tiny.min_segment_size = 1024;
    tiny.min_file_size = 0;
    const std::size_t result =
        SegmentDownloader::calculate_optimal_segments(file_size, tiny);
    EXPECT_LE(result, 8U);
    EXPECT_GE(result, 1U);
}
