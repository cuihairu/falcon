/**
 * @file coverage_supplement_test.cpp
 * @brief libfalcon-protocols 覆盖率补充测试
 * @author Falcon Team
 * @date 2026-09-05
 *
 * 覆盖目标（现有测试因编译宏或私有性未触达的公共 API 路径）：
 * - FileHasher: verify / verify_multiple / HashVerifyCommand 成功路径
 * - IncrementalDownloader: downloadChanged / applyPatch / 空文件哈希列表
 * - SegmentDownloader: calculate_optimal_segments 边界分支
 * 全部离线运行，不依赖网络。
 */

#include <gtest/gtest.h>
#include <falcon/protocols/file_hash.hpp>
#include <falcon/protocols/incremental_download.hpp>
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
