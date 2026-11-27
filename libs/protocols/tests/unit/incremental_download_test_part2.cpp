/**
 * @file incremental_download_test_part2.cpp
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

TEST_F(IncrementalDownloadTest, DownloadChangedVerifiesFileHashEndToEnd) {
    const std::string remote_content = make_content(2500, 7);
    std::string local_content = remote_content;
    for (std::size_t i = 1024; i < 2048; ++i) {
        local_content[i] = static_cast<char>((local_content[i] + 11) % 251);
    }

    const std::string local_path = testDir_ + "/verify_local.bin";
    {
        std::ofstream file(local_path, std::ios::binary);
        file.write(local_content.data(),
                   static_cast<std::streamsize>(local_content.size()));
    }

    const std::string remote_path = testDir_ + "/verify_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(remote_content.data(),
                   static_cast<std::streamsize>(remote_content.size()));
    }
    IncrementalDownloader list_generator;
    const auto remote_chunks = list_generator.generateHashList(remote_path, 1024);
    const std::string file_hash = list_generator.calculateFileHash(remote_path);
    ASSERT_EQ(file_hash.size(), std::size_t{64});

    IncrementalTestServer server;
    ASSERT_TRUE(server.start(
        remote_content,
        IncrementalDownloader::serializeHashList(
            remote_chunks, 1024, "sha256", 2500, file_hash)));

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";

    const FileDiff diff = downloader.compare(
        local_path,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);
    ASSERT_FALSE(diff.chunks.empty());
    // compare 应把列表声明的整文件哈希带回 diff
    EXPECT_EQ(diff.fileHash, file_hash);

    const std::string out_path = testDir_ + "/verify_out.bin";
    ASSERT_TRUE(downloader.downloadChanged(diff, out_path));
    EXPECT_EQ(read_file(out_path), remote_content);
}

// 错误 fileHash（分块内容与分块哈希全部正确）→ 整文件校验拦截，
// 失败且输出被删除——损坏成品绝不顶着成功语义留下
TEST_F(IncrementalDownloadTest, DownloadChangedDetectsFileHashMismatch) {
    const std::string remote_content = make_content(2048, 11);
    const std::string remote_path = testDir_ + "/mismatch_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(remote_content.data(),
                   static_cast<std::streamsize>(remote_content.size()));
    }
    IncrementalDownloader generator;
    const auto chunks = generator.generateHashList(remote_path, 1024);

    const std::string wrong_hash(64, '0');
    IncrementalTestServer server;
    ASSERT_TRUE(server.start(
        remote_content,
        IncrementalDownloader::serializeHashList(
            chunks, 1024, "sha256", 2048, wrong_hash)));

    const std::string local_path =
        createTestFile("mismatch_local.bin", 2048);

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";
    const FileDiff diff = downloader.compare(
        local_path,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);
    ASSERT_FALSE(diff.chunks.empty());
    EXPECT_EQ(diff.fileHash, wrong_hash);

    const std::string out_path = testDir_ + "/mismatch_out.bin";
    EXPECT_FALSE(downloader.downloadChanged(diff, out_path));
    EXPECT_FALSE(std::filesystem::exists(out_path));
}

// verifyDownload=false：显式关闭整文件校验（开关方向钉住）
TEST_F(IncrementalDownloadTest, VerifyDownloadDisabledSkipsFileHashCheck) {
    const std::string remote_content = make_content(2048, 17);
    const std::string remote_path = testDir_ + "/noskip_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(remote_content.data(),
                   static_cast<std::streamsize>(remote_content.size()));
    }
    IncrementalDownloader generator;
    const auto chunks = generator.generateHashList(remote_path, 1024);

    const std::string wrong_hash(64, '0');
    IncrementalTestServer server;
    ASSERT_TRUE(server.start(
        remote_content,
        IncrementalDownloader::serializeHashList(
            chunks, 1024, "sha256", 2048, wrong_hash)));

    const std::string local_path =
        createTestFile("noskip_local.bin", 2048);

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";
    options.verifyDownload = false;
    const FileDiff diff = downloader.compare(
        local_path,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);
    ASSERT_FALSE(diff.chunks.empty());
    EXPECT_FALSE(diff.verifyDownload);

    const std::string out_path = testDir_ + "/noskip_out.bin";
    ASSERT_TRUE(downloader.downloadChanged(diff, out_path));
    EXPECT_EQ(read_file(out_path), remote_content);
}

// 服务器投毒（尺寸正确、字节翻转）→ 分块哈希校验拦下，失败且无输出
TEST_F(IncrementalDownloadTest, DownloadChangedDetectsChunkHashMismatch) {
    const std::string remote_content = make_content(2048, 23);
    const std::string remote_path = testDir_ + "/poison_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(remote_content.data(),
                   static_cast<std::streamsize>(remote_content.size()));
    }
    IncrementalDownloader generator;
    const auto chunks = generator.generateHashList(remote_path, 1024);

    IncrementalTestServer server;
    server.set_corrupt_ranges(true);
    ASSERT_TRUE(server.start(
        remote_content,
        IncrementalDownloader::serializeHashList(chunks, 1024, "sha256", 2048)));

    const std::string local_path =
        createTestFile("poison_local.bin", 2048);

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";
    const FileDiff diff = downloader.compare(
        local_path,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);
    ASSERT_FALSE(diff.chunks.empty());
    ASSERT_GT(diff.totalChanged, 0u);

    const std::string out_path = testDir_ + "/poison_out.bin";
    EXPECT_FALSE(downloader.downloadChanged(diff, out_path));
    EXPECT_FALSE(std::filesystem::exists(out_path));
}

// ============================================================================
// 覆盖率批次 O：解析容错与失败路由收口
// ============================================================================

// 行尾 \r 与首尾空白的裁剪路径（CRLF 哈希列表）
TEST_F(IncrementalDownloadTest, ParseHashListTrimsCRLFAndPadding) {
    const std::string hash_hex(64, 'a');
    const std::string text =
        "# algorithm: sha256\r\n   " + hash_hex + " \t\r\n";
    const auto chunks = IncrementalDownloader::parseHashList(text, 100, "sha256");
    ASSERT_EQ(chunks.size(), std::size_t{1});
    EXPECT_EQ(chunks[0].hash, hash_hex);
    EXPECT_EQ(chunks[0].offset, 0u);
    EXPECT_EQ(chunks[0].size, 100u);
    EXPECT_FALSE(chunks[0].changed);
}

// 零分块 → 列表无效（元数据 chunkSize: 0 被 v>0 守卫忽略，
// defaultChunkSize=0 且元数据不覆盖时直达零分块校验）
TEST_F(IncrementalDownloadTest, ParseHashListRejectsZeroChunkSize) {
    const std::string text =
        "# falcon-hash-list v1\r\n# chunkSize: 0\r\n# algorithm: sha256\r\n" +
        std::string(64, 'a');
    // 元数据 0 被忽略，默认值生效 → 合法
    EXPECT_EQ(IncrementalDownloader::parseHashList(text, 100, "sha256").size(),
              std::size_t{1});
    // 默认值 0 且无有效覆盖 → 零分块校验拒绝
    EXPECT_TRUE(IncrementalDownloader::parseHashList(text, 0, "sha256").empty());
}

// 未知哈希算法：EVP_get_digestbyname 失败 → 哈希空串，compare 优雅降级
TEST_F(IncrementalDownloadTest, CompareUnknownHashAlgorithmDegradesGracefully) {
    const std::string local = createTestFile("bogus_alg.bin", 2048);
    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.hashAlgorithm = "no-such-digest";
    const FileDiff diff =
        downloader.compare(local, "http://127.0.0.1:1/remote.bin", options);
    EXPECT_GT(diff.localSize, 0u);
    EXPECT_TRUE(diff.chunks.empty());
}

// 目录不可读（POSIX 下 fopen 目录成功而 read 必败）→ 读取错误干净中断
#ifndef _WIN32
TEST_F(IncrementalDownloadTest, GenerateHashListDirectoryReadFailsCleanly) {
    IncrementalDownloader downloader;
    const auto chunks = downloader.generateHashList(
        std::filesystem::temp_directory_path().string(), 512);
    EXPECT_TRUE(chunks.empty());
}

// 根目录形态：libstdc++ 对 / 的 tellg 返回 INT64_MAX（非 -1）→ fileSize
// 巨大进入读取循环 → read 失败命中循环内错误分支（/tmp 的 tellg=-1 只走
// 零尺寸早退，到不了该分支）——两条目录形态都断言空 chunks
TEST_F(IncrementalDownloadTest, CalculateChunkHashesDirectoryReadFailsAfterSeek) {
    IncrementalDownloader downloader;
    const auto chunks = downloader.generateHashList("/", 512);
    EXPECT_TRUE(chunks.empty());
}
#endif

// 远程哈希列表非法（算法元数据与请求不一致）→ 回退全量下载建议
TEST_F(IncrementalDownloadTest, CompareInvalidRemoteHashListFallsBackToFullDownload) {
    const std::string local = createTestFile("invalid_list.bin", 2048);

    IncrementalTestServer server;
    // sha512 元数据 vs 请求 sha256 → 解析为空
    ASSERT_TRUE(server.start(
        std::string(2048, '\0'),
        "# falcon-hash-list v1\r\n# chunkSize: 1024\r\n# algorithm: sha512\r\n"
        "# fileSize: 2048\r\n# chunks: 2\r\n" + std::string(128, 'a') + "\r\n"));

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";
    const FileDiff diff = downloader.compare(
        local,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);
    EXPECT_TRUE(diff.chunks.empty());
    EXPECT_EQ(diff.remoteSize, 0u);
}

// Range 服务器短传（Content-Length 与实际一致但尺寸不足）→ 尺寸不匹配 → 干净失败
TEST_F(IncrementalDownloadTest, DownloadChangedRangeShortServerFailsCleanly) {
    const std::string remote_content = make_content(2048, 5);
    const std::string remote_path = testDir_ + "/range_short_remote.bin";
    {
        std::ofstream f(remote_path, std::ios::binary);
        f.write(remote_content.data(),
                static_cast<std::streamsize>(remote_content.size()));
    }
    IncrementalDownloader generator;
    const auto chunks = generator.generateHashList(remote_path, 1024);

    IncrementalTestServer server;
    server.set_range_short(true);
    ASSERT_TRUE(server.start(
        remote_content,
        IncrementalDownloader::serializeHashList(chunks, 1024, "sha256", 2048)));

    const std::string old_content = make_content(2048, 6);
    const std::string local_path = createTestFile("range_short_local.bin", 2048);
    {
        std::ofstream f(local_path, std::ios::binary);
        f.write(old_content.data(),
                static_cast<std::streamsize>(old_content.size()));
    }

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";
    const FileDiff diff = downloader.compare(
        local_path,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);
    ASSERT_FALSE(diff.chunks.empty());
    ASSERT_GT(diff.totalChanged, 0u);

    const std::string out_path = testDir_ + "/range_short_out.bin";
    EXPECT_FALSE(downloader.downloadChanged(diff, out_path));
    EXPECT_FALSE(std::filesystem::exists(out_path));
}

#if defined(FALCON_FAILURE_INJECTION)

// calculateHash 的 EVP 防御链注入：哈希计算失败返回空串 → 校验失败收口
// （正确哈希在注入前生成；注入后本地哈希恒空 → 与期望不匹配）

TEST_F(IncrementalDownloadTest, InjectedHashInitFailureFailsVerify) {
    IncrementalDownloader downloader;
    std::string filePath = createTestFile("inj_init.bin", 1024);
    auto chunks = downloader.generateHashList(filePath, 2048);
    ASSERT_FALSE(chunks.empty());

    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::IncrementalHashInit);
    EXPECT_FALSE(downloader.verifyFile(filePath, chunks[0].hash));
}

TEST_F(IncrementalDownloadTest, InjectedHashUpdateFailureFailsVerify) {
    IncrementalDownloader downloader;
    std::string filePath = createTestFile("inj_update.bin", 1024);
    auto chunks = downloader.generateHashList(filePath, 2048);
    ASSERT_FALSE(chunks.empty());

    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::IncrementalHashUpdate);
    EXPECT_FALSE(downloader.verifyFile(filePath, chunks[0].hash));
}

TEST_F(IncrementalDownloadTest, InjectedHashFinalFailureFailsVerify) {
    IncrementalDownloader downloader;
    std::string filePath = createTestFile("inj_final.bin", 1024);
    auto chunks = downloader.generateHashList(filePath, 2048);
    ASSERT_FALSE(chunks.empty());

    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::IncrementalHashFinal);
    EXPECT_FALSE(downloader.verifyFile(filePath, chunks[0].hash));
}

// curl 句柄创建失败 → 远程哈希列表拉取失败 → compare 回退全量下载建议
// （downloadRemoteHashList 私有，经公开入口 compare 触发）
TEST_F(IncrementalDownloadTest, InjectedCurlInitFailFailsRemoteHashList) {
    const std::string local = createTestFile("inj_hashlist.bin", 2048);
    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 512;

    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::CurlEasyInit);
    const FileDiff diff = downloader.compare(
        local, "http://127.0.0.1:1/remote.bin", options);
    EXPECT_GT(diff.localSize, 0u);
    EXPECT_TRUE(diff.chunks.empty());
}

// curl 句柄创建失败 → 分块 Range 拉取返回空 → downloadChanged 干净失败
// （不产出输出文件；手工构造 FileDiff 免起 HTTP 服务器）
TEST_F(IncrementalDownloadTest, InjectedCurlInitFailFailsDownloadChanged) {
    const std::string local = createTestFile("inj_changed_local.bin", 2048);

    FileDiff diff;
    diff.localPath = local;
    diff.remotePath = "http://127.0.0.1:1/remote.bin";
    diff.localSize = 2048;
    diff.remoteSize = 2048;
    ChunkInfo chunk;
    chunk.offset = 0;
    chunk.size = 512;
    chunk.changed = true;
    diff.chunks.push_back(chunk);
    diff.totalChanged = 512;

    IncrementalDownloader downloader;
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::CurlEasyInit);
    EXPECT_FALSE(downloader.downloadChanged(
        diff, testDir_ + "/inj_changed_out.bin"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ + "/inj_changed_out.bin"));
}

#endif  // FALCON_FAILURE_INJECTION

// ============================================================================
// rsync 滚动校验和（v2 块列表：生成 / 序列化 / 解析 / 计划 / 端到端比较）
// ============================================================================

namespace {

/// 与实现独立重导的 Adler 风格弱校验和（防同一 bug 自我印证的对照 oracle）
uint32_t naive_weak(const std::string& data, std::size_t off, std::size_t len) {
    uint32_t a = 0;
    uint32_t b = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const auto byte = static_cast<uint8_t>(data[off + i]);
        a += byte;
        b += a;
    }
    return (a & 0xFFFFu) | ((b & 0xFFFFu) << 16);
}

/// 替换第一处出现的行片段（构造损坏变体用）
std::string replace_first(std::string text, const std::string& from,
                          const std::string& to) {
    const auto pos = text.find(from);
    EXPECT_NE(pos, std::string::npos);
    if (pos != std::string::npos) {
        text.replace(pos, from.size(), to);
    }
    return text;
}

/// 生成一个「5 整块 + 100 字节尾巴」的远端内容（blockSize=1024）
std::string make_rsync_remote(unsigned seed) {
    // 伪随机字节（LCG，uint32 溢出回绕确定性）：不能用 make_content——
    // 其 (i*13+seed)%251 序列以 251 为周期，周期内容会让错位扫描在预期
    // 位置之前命中别名窗口（内容真相同，强哈希照常确认），破坏偏移断言
    constexpr std::size_t kSize = 5 * 1024 + 100;
    std::string content;
    content.reserve(kSize);
    uint32_t x = seed * 2654435761u + 1u;
    for (std::size_t i = 0; i < kSize; ++i) {
        x = x * 1103515245u + 12345u;
        content.push_back(static_cast<char>((x >> 16) & 0xFFu));
    }
    return content;
}

}  // namespace

// init 与连续 roll 必须等价：对 data[k..k+1024) 直接 init 的值 == 从
// data[0..1024) init 后滑 k 格的值；且两者都与独立重导的 oracle 一致
TEST_F(IncrementalDownloadTest, RollingChecksumRollEqualsInitAtOffset) {
    const std::string data = make_content(4096 + 16, 5);
    const uint32_t window = 1024;

    RollingChecksum rc;
    rc.init(reinterpret_cast<const uint8_t*>(data.data()), window);
    EXPECT_EQ(rc.value(), naive_weak(data, 0, window));

    for (uint32_t k = 1; k <= 8; ++k) {
        rc.roll(static_cast<uint8_t>(data[k - 1]),
                static_cast<uint8_t>(data[window + k - 1]));
        EXPECT_EQ(rc.value(), naive_weak(data, k, window));

        // 直接以偏移 k 重算（init 路径）必须与滚动结果一致
        RollingChecksum direct;
        direct.init(reinterpret_cast<const uint8_t*>(data.data()) + k, window);
        EXPECT_EQ(direct.value(), rc.value());
    }
}

// v2 块列表生成：整块切分、尾巴排除、weak 与 oracle 一致、strong 为 64 位
// hex、fileHash 为整文件 sha256
TEST_F(IncrementalDownloadTest, GenerateRsyncBlockListCoversFullBlocksOnly) {
    const std::string content = make_rsync_remote(1);
    const std::string path = testDir_ + "/rsync_remote.bin";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(path, 1024, list));
    EXPECT_EQ(list.blockSize, 1024u);
    EXPECT_EQ(list.fileSize, content.size());
    EXPECT_EQ(list.blocks.size(), std::size_t{5});  // 100 字节尾巴不进列表
    EXPECT_EQ(list.fileHash, downloader.calculateFileHash(path, "sha256"));
    EXPECT_EQ(list.fileHash.size(), std::size_t{64});

    for (std::size_t i = 0; i < list.blocks.size(); ++i) {
        EXPECT_EQ(list.blocks[i].offset, i * 1024u);
        EXPECT_EQ(list.blocks[i].weak,
                  naive_weak(content, i * 1024, 1024));
        EXPECT_EQ(list.blocks[i].strong.size(), std::size_t{64});
    }
}

TEST_F(IncrementalDownloadTest, GenerateRsyncBlockListRejectsBadBlockSize) {
    const std::string path = createTestFile("rsync_bad_bs.bin", 4096);
    IncrementalDownloader downloader;
    RsyncBlockList list;
    EXPECT_FALSE(downloader.generateRsyncBlockList(path, 0, list));
    EXPECT_FALSE(downloader.generateRsyncBlockList(
        path, 17ull * 1024 * 1024, list));
}

// 空文件：无整块，但 fileHash 必须仍是合法 sha256（RFC 独立向量：
// sha256("") 的已知常量，防空输入退化成空哈希被当失败）
TEST_F(IncrementalDownloadTest, GenerateRsyncBlockListEmptyFile) {
    const std::string path = testDir_ + "/rsync_empty.bin";
    {
        std::ofstream file(path, std::ios::binary);
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(path, 1024, list));
    EXPECT_TRUE(list.blocks.empty());
    EXPECT_EQ(list.fileHash,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// 序列化 → 解析往返（版本魔数行修复的回归钉子：魔数行无冒号，必须
// 在 parse_metadata_line 之前直接串比较命中）
TEST_F(IncrementalDownloadTest, RsyncSerializeParseRoundTrip) {
    const std::string content = make_rsync_remote(2);
    const std::string path = testDir_ + "/rsync_roundtrip.bin";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(path, 1024, list));

    const std::string text =
        IncrementalDownloader::serializeRsyncBlockList(list);
    EXPECT_NE(text.find("# falcon-hash-list v2\n"), std::string::npos);

    RsyncBlockList parsed;
    ASSERT_TRUE(IncrementalDownloader::parseRsyncBlockList(text, parsed));
    EXPECT_EQ(parsed.blockSize, list.blockSize);
    EXPECT_EQ(parsed.fileSize, list.fileSize);
    EXPECT_EQ(parsed.fileHash, list.fileHash);
    ASSERT_EQ(parsed.blocks.size(), list.blocks.size());
    for (std::size_t i = 0; i < list.blocks.size(); ++i) {
        EXPECT_EQ(parsed.blocks[i].offset, list.blocks[i].offset);
        EXPECT_EQ(parsed.blocks[i].weak, list.blocks[i].weak);
        EXPECT_EQ(parsed.blocks[i].strong, list.blocks[i].strong);
    }
}

// 解析面拒绝矩阵：v1 列表、缺魔数、mode 错、缺 fileHash、fileHash 非 hex、
// 算法非 sha256、blocks 计数不符、weak 非 8 位 hex、块偏移不连续、
// 数值垃圾——任何一处损坏都使整个列表无效
TEST_F(IncrementalDownloadTest, ParseRsyncBlockListRejectsCorruptions) {
    const std::string content = make_rsync_remote(3);
    const std::string path = testDir_ + "/rsync_corrupt.bin";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(path, 1024, list));
    const std::string valid =
        IncrementalDownloader::serializeRsyncBlockList(list);

    // v1 分块哈希列表必须被拒（magic 后无 v2 元数据 → 数据行前置门禁）
    const auto chunks = downloader.generateHashList(path, 1024);
    const std::string v1_text = IncrementalDownloader::serializeHashList(
        chunks, 1024, "sha256", content.size());
    RsyncBlockList out;
    EXPECT_FALSE(IncrementalDownloader::parseRsyncBlockList(v1_text, out));

    const auto drop_magic = [](const std::string& text) {
        const auto pos = text.find('\n');
        return text.substr(pos + 1);
    };
    // 重写首条数据行（offset 0）的 weak 字段——确定性破坏，不依赖具体
    // 哈希内容。数据行以 "\n0:" 开头（元数据行都以 '#' 开头）
    const auto rewrite_first_row = [](const std::string& text,
                                      const std::string& weak) {
        const auto start = text.find("\n0:");
        EXPECT_NE(start, std::string::npos);
        if (start == std::string::npos) return text;
        const auto end = text.find('\n', start + 1);
        const std::string row = text.substr(start + 1, end - start - 1);
        const auto c1 = row.find(':');
        const auto c2 = row.find(':', c1 + 1);
        return text.substr(0, start + 1) + row.substr(0, c1 + 1) + weak +
               row.substr(c2) + text.substr(end);
    };

    const auto replace_first_row = [](const std::string& text,
                                      std::size_t row_offset,
                                      const std::string& new_row_prefix) {
        // 以 "\n<row_offset>:" 定位首条数据行（元数据行都以 '#' 开头）
        const std::string needle = "\n" + std::to_string(row_offset) + ":";
        const auto start = text.find(needle);
        EXPECT_NE(start, std::string::npos);
        if (start == std::string::npos) return text;
        const auto end = text.find('\n', start + 1);
        const std::string row = text.substr(start + 1, end - start - 1);
        return text.substr(0, start + 1) + new_row_prefix +
               row.substr(new_row_prefix.size()) + text.substr(end);
    };

    const std::vector<std::string> corrupt = {
        drop_magic(valid),
        replace_first(valid, "# mode: rsync", "# mode: chunk"),
        replace_first(valid, "# fileHash: ", "# xfileHash: "),
        replace_first(valid, "# fileHash: ", "# fileHash: zz"),
        replace_first(valid, "# algorithm: sha256", "# algorithm: md5"),
        replace_first(valid, "# blocks: 5", "# blocks: 4"),
        rewrite_first_row(valid, "0011223"),   // weak 7 位 hex
        rewrite_first_row(valid, "zz112233"),  // weak 非十六进制
        replace_first(valid, "\n2048:", "\n2049:"),  // 块偏移非连续
        replace_first(valid, "# blockSize: 1024", "# blockSize: zz"),
        replace_first(valid, "# blockSize: 1024", "# blockSize: 0"),
        replace_first(valid, "# blockSize: 1024", "# blockSize: 20000000"),
        // 数据行缺强哈希列（去掉首行第二个冒号）
        ([&] {
            const std::string& t = valid;
            const auto start = t.find("\n0:");
            EXPECT_NE(start, std::string::npos);
            const auto end = t.find('\n', start + 1);
            std::string row = t.substr(start + 1, end - start - 1);
            const auto c1 = row.find(':');
            const auto c2 = row.find(':', c1 + 1);
            if (c2 == std::string::npos) return t;
            row.erase(c2, 1);  // 删除第二个冒号
            return t.substr(0, start + 1) + row + t.substr(end);
        })(),
        // 偏移非数字
        replace_first_row(valid, 0, "x0:"),
        // 强哈希非十六进制
        ([&] {
            const std::string& t = valid;
            const auto start = t.find("\n0:");
            EXPECT_NE(start, std::string::npos);
            const auto end = t.find('\n', start + 1);
            std::string row = t.substr(start + 1, end - start - 1);
            row[row.size() - 1] = 'z';
            return t.substr(0, start + 1) + row + t.substr(end);
        })(),
        // 偏移溢出（stoull 抛）
        ([&] {
            const std::string& t = valid;
            const auto start = t.find("\n0:");
            EXPECT_NE(start, std::string::npos);
            const auto end = t.find('\n', start + 1);
            std::string row = t.substr(start + 1, end - start - 1);
            const auto c1 = row.find(':');
            return t.substr(0, start + 1) +
                   "99999999999999999999:" + row.substr(c1 + 1) + t.substr(end);
        })(),
        // 末块偏移越过文件尾（offset+block_size > file_size）
        ([&] {
            const std::string& t = valid;
            const auto start = t.find("\n4096:");
            EXPECT_NE(start, std::string::npos);
            const auto end = t.find('\n', start + 1);
            std::string row = t.substr(start + 1, end - start - 1);
            row[0] = '0';  // 把 4096 改成 0096（仍十进制，但 offset 不再连续）
            return t.substr(0, start + 1) + row + t.substr(end);
        })(),
    };

    for (const auto& text : corrupt) {
        EXPECT_FALSE(IncrementalDownloader::parseRsyncBlockList(text, out))
            << "corrupt variant accepted:\n"
            << text;
    }

    // 第二块偏移非连续（错位一格同样拒绝）
    RsyncBlockList shifted;
    EXPECT_FALSE(IncrementalDownloader::parseRsyncBlockList(
        replace_first(valid, "\n1024:", "\n1025:"), shifted));
}

// 完全相同的本地与远端：单一 LocalCopy 段覆盖整块区 + 尾巴下载段
TEST_F(IncrementalDownloadTest, PlanRsyncIdenticalFilesSingleCopy) {
    const std::string content = make_rsync_remote(4);
    const std::string remote_path = testDir_ + "/rsync_same_remote.bin";
    const std::string local_path = testDir_ + "/rsync_same_local.bin";
    for (const auto& p : {remote_path, local_path}) {
        std::ofstream file(p, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(remote_path, 1024, list));

    const RsyncPlan plan = downloader.planRsync(local_path, list);
    ASSERT_EQ(plan.segments.size(), std::size_t{2});
    EXPECT_EQ(plan.segments[0].kind, RsyncSegment::Kind::LocalCopy);
    EXPECT_EQ(plan.segments[0].outputOffset, 0u);
    EXPECT_EQ(plan.segments[0].localOffset, 0u);
    EXPECT_EQ(plan.segments[0].length, 5u * 1024);
    EXPECT_EQ(plan.segments[1].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments[1].outputOffset, 5u * 1024);
    EXPECT_EQ(plan.segments[1].length, 100u);
    EXPECT_EQ(plan.copyBytes, 5u * 1024);
    EXPECT_EQ(plan.downloadBytes, 100u);
}

// 中间块变化：只重下该块与尾巴，其余按原位拷贝（v1 布局 localOffset==offset）
TEST_F(IncrementalDownloadTest, PlanRsyncChangedMiddleBlockDownloadsOnlyIt) {
    std::string remote_content = make_rsync_remote(5);
    std::string local_content = remote_content;
    for (std::size_t i = 2048; i < 3072; ++i) {
        local_content[i] = static_cast<char>((local_content[i] + 17) % 251);
    }
    const std::string remote_path = testDir_ + "/rsync_mid_remote.bin";
    const std::string local_path = testDir_ + "/rsync_mid_local.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(remote_content.data(),
                   static_cast<std::streamsize>(remote_content.size()));
        std::ofstream local(local_path, std::ios::binary);
        local.write(local_content.data(),
                    static_cast<std::streamsize>(local_content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(remote_path, 1024, list));

    const RsyncPlan plan = downloader.planRsync(local_path, list);
    ASSERT_EQ(plan.segments.size(), std::size_t{4});
    EXPECT_EQ(plan.segments[0].kind, RsyncSegment::Kind::LocalCopy);
    EXPECT_EQ(plan.segments[0].outputOffset, 0u);
    EXPECT_EQ(plan.segments[0].localOffset, 0u);
    EXPECT_EQ(plan.segments[0].length, 2048u);
    EXPECT_EQ(plan.segments[1].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments[1].outputOffset, 2048u);
    EXPECT_EQ(plan.segments[1].length, 1024u);
    EXPECT_EQ(plan.segments[2].kind, RsyncSegment::Kind::LocalCopy);
    EXPECT_EQ(plan.segments[2].outputOffset, 3072u);
    EXPECT_EQ(plan.segments[2].localOffset, 3072u);
    EXPECT_EQ(plan.segments[2].length, 2048u);
    EXPECT_EQ(plan.segments[3].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments[3].outputOffset, 5u * 1024);
    EXPECT_EQ(plan.segments[3].length, 100u);
    EXPECT_EQ(plan.downloadBytes, 1124u);
    EXPECT_EQ(plan.copyBytes, 4096u);
}

// 远端中部插入 3 字节：块错位后滚动扫描仍能恢复（LocalCopy 的本地源
// 偏移与输出位置分叉），只有插入处与尾巴需要下载
TEST_F(IncrementalDownloadTest, PlanRsyncShiftedMatchRecoversWithDivergentOffsets) {
    const std::string local_content = make_rsync_remote(6);
    // 在块 0 之后插入 "PRE"：块 0 原位匹配，块 1 错位丢失，块 2-4 在
    // local 位置 2045 起连续命中（1024k-3）
    std::string remote_content = local_content.substr(0, 1024) + "PRE" +
                                 local_content.substr(1024);
    const std::string remote_path = testDir_ + "/rsync_shift_remote.bin";
    const std::string local_path = testDir_ + "/rsync_shift_local.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(remote_content.data(),
                   static_cast<std::streamsize>(remote_content.size()));
        std::ofstream local(local_path, std::ios::binary);
        local.write(local_content.data(),
                    static_cast<std::streamsize>(local_content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(remote_path, 1024, list));

    const RsyncPlan plan = downloader.planRsync(local_path, list);
    ASSERT_EQ(plan.segments.size(), std::size_t{4});
    EXPECT_EQ(plan.segments[0].kind, RsyncSegment::Kind::LocalCopy);
    EXPECT_EQ(plan.segments[0].outputOffset, 0u);
    EXPECT_EQ(plan.segments[0].localOffset, 0u);
    EXPECT_EQ(plan.segments[0].length, 1024u);
    EXPECT_EQ(plan.segments[1].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments[1].outputOffset, 1024u);
    EXPECT_EQ(plan.segments[1].length, 1024u);
    // 错位恢复：本地源 2045 → 输出 2048（divergent）
    EXPECT_EQ(plan.segments[2].kind, RsyncSegment::Kind::LocalCopy);
    EXPECT_EQ(plan.segments[2].outputOffset, 2048u);
    EXPECT_EQ(plan.segments[2].localOffset, 2045u);
    EXPECT_EQ(plan.segments[2].length, 3072u);
    EXPECT_EQ(plan.segments[3].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments[3].outputOffset, 5u * 1024);
    EXPECT_EQ(plan.segments[3].length, 103u);
    // 下载字节 = 插入区 1024 + 尾巴 103
    EXPECT_EQ(plan.downloadBytes, 1127u);
    EXPECT_EQ(plan.copyBytes, 4096u);
}

TEST_F(IncrementalDownloadTest, PlanRsyncUnreadableLocalFallsBackToFullDownload) {
    const std::string content = make_rsync_remote(7);
    const std::string remote_path = testDir_ + "/rsync_dead_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(remote_path, 1024, list));

    const RsyncPlan plan =
        downloader.planRsync(testDir_ + "/no_such_local.bin", list);
    ASSERT_EQ(plan.segments.size(), std::size_t{1});
    EXPECT_EQ(plan.segments[0].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments[0].outputOffset, 0u);
    EXPECT_EQ(plan.segments[0].length, content.size());
    EXPECT_EQ(plan.downloadBytes, content.size());
    EXPECT_EQ(plan.copyBytes, 0u);
}

TEST_F(IncrementalDownloadTest, PlanRsyncEmptyLocalDownloadsEverything) {
    const std::string content = make_rsync_remote(8);
    const std::string remote_path = testDir_ + "/rsync_empty_local_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    const std::string local_path = testDir_ + "/rsync_local_empty.bin";
    {
        std::ofstream file(local_path, std::ios::binary);
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(remote_path, 1024, list));

    const RsyncPlan plan = downloader.planRsync(local_path, list);
    ASSERT_EQ(plan.segments.size(), std::size_t{2});
    EXPECT_EQ(plan.segments[0].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments[0].length, 5u * 1024);
    EXPECT_EQ(plan.segments[1].kind, RsyncSegment::Kind::Download);
    EXPECT_EQ(plan.segments[1].length, 100u);
    EXPECT_EQ(plan.downloadBytes, content.size());
    EXPECT_EQ(plan.copyBytes, 0u);
}

// 端到端：v2 块列表经 compareRsync → downloadChanged 逐字节重建错位远端
// （错位 LocalCopy 走独立输出缓冲路径；尾巴与插入处走 Range 下载；最后
// 整文件哈希校验放行）
TEST_F(IncrementalDownloadTest, CompareRsyncEndToEndRebuildsShiftedRemote) {
    const std::string local_content = make_rsync_remote(9);
    std::string remote_content =
        local_content.substr(0, 1024) + "PRE" + local_content.substr(1024);
    const std::string local_path = testDir_ + "/rsync_e2e_local.bin";
    {
        std::ofstream file(local_path, std::ios::binary);
        file.write(local_content.data(),
                   static_cast<std::streamsize>(local_content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    // 生成块列表必须在 writer 作用域关闭之后：ofstream 未 flush 时数据仍在
    // 用户态缓冲，生成器 seekg-end 读到 0 会产出「合法的空列表」（fileHash
    // 为 sha256("")，parse 照常通过），e2e 即被远端 0 字节假象污染
    {
        const std::string remote_path = testDir_ + "/rsync_e2e_remote.bin";
        std::ofstream file(remote_path, std::ios::binary);
        file.write(remote_content.data(),
                   static_cast<std::streamsize>(remote_content.size()));
    }
    ASSERT_TRUE(downloader.generateRsyncBlockList(
        testDir_ + "/rsync_e2e_remote.bin", 1024, list));

    IncrementalTestServer server;
    ASSERT_TRUE(server.start(
        remote_content, IncrementalDownloader::serializeRsyncBlockList(list)));

    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";
    FileDiff diff;
    ASSERT_TRUE(downloader.compareRsync(
        local_path,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin",
        options, diff));
    EXPECT_EQ(diff.remoteSize, remote_content.size());
    EXPECT_EQ(diff.fileHash, list.fileHash);
    // 插入处 + 尾巴 = 下载字节；其余全部本地拷贝
    EXPECT_EQ(diff.totalChanged, 1127u);
    ASSERT_EQ(diff.chunks.size(), std::size_t{4});
    EXPECT_FALSE(diff.chunks[0].changed);
    EXPECT_EQ(diff.chunks[0].localOffset, 0u);
    EXPECT_TRUE(diff.chunks[1].changed);
    EXPECT_FALSE(diff.chunks[2].changed);
    EXPECT_EQ(diff.chunks[2].offset, 2048u);
    EXPECT_EQ(diff.chunks[2].localOffset, 2045u);  // divergent copy
    EXPECT_TRUE(diff.chunks[3].changed);

    const std::string out_path = testDir_ + "/rsync_e2e_out.bin";
    EXPECT_TRUE(downloader.downloadChanged(diff, out_path));
    EXPECT_EQ(read_file(out_path), remote_content);
}

TEST_F(IncrementalDownloadTest, CompareRsyncFailsWithoutValidBlockList) {
    const std::string local_path = createTestFile("rsync_fail_local.bin", 2048);
    const std::string content = make_rsync_remote(10);
    const std::string remote_path = testDir_ + "/rsync_fail_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader downloader;
    RsyncBlockList list;
    ASSERT_TRUE(downloader.generateRsyncBlockList(remote_path, 1024, list));

    IncrementalDownloader::Options options;
    // 服务器在位但 .falconhash 为空 → 解析失败
    IncrementalTestServer empty_list_server;
    ASSERT_TRUE(empty_list_server.start(content, ""));
    FileDiff diff;
    EXPECT_FALSE(downloader.compareRsync(
        local_path,
        "http://127.0.0.1:" + std::to_string(empty_list_server.port()) +
            "/file.bin",
        options, diff));
    // 无服务器（连接拒绝）同样干净失败
    FileDiff diff2;
    EXPECT_FALSE(downloader.compareRsync(
        local_path, "http://127.0.0.1:1/remote.bin", options, diff2));
}

// 独立输出缓冲回归钉子：两段 LocalCopy 交换顺序——先执行的段把目标
// 压在另一段的源上。就地写会先毁掉尚未读取的数据；独立缓冲下无论
// 顺序都逐字节正确（成品 = 本地缓冲的两段轮换）
TEST_F(IncrementalDownloadTest, DownloadChangedDivergentCopySurvivesSwappedOrder) {
    const std::string local_content = make_content(4096, 3);
    const std::string local_path = testDir_ + "/rsync_swap_local.bin";
    {
        std::ofstream file(local_path, std::ios::binary);
        file.write(local_content.data(),
                   static_cast<std::streamsize>(local_content.size()));
    }

    FileDiff diff;
    diff.localPath = local_path;
    diff.remotePath = "unused://swap";
    diff.localSize = local_content.size();
    diff.remoteSize = local_content.size();
    diff.verifyDownload = false;  // 手工构造，无整文件哈希可校验

    // 段 A：读 local[0,2048) 写 out[2048,4096)（覆盖段 B 的源）
    ChunkInfo first;
    first.offset = 2048;
    first.size = 2048;
    first.changed = false;
    first.localOffset = 0;
    // 段 B：读 local[2048,4096) 写 out[0,2048)
    ChunkInfo second;
    second.offset = 0;
    second.size = 2048;
    second.changed = false;
    second.localOffset = 2048;
    diff.chunks.push_back(first);   // 先执行段 A（clobber 段 B 的源）
    diff.chunks.push_back(second);

    const std::string out_path = testDir_ + "/rsync_swap_out.bin";
    IncrementalDownloader downloader;
    EXPECT_TRUE(downloader.downloadChanged(diff, out_path));
    EXPECT_EQ(read_file(out_path),
              local_content.substr(2048) + local_content.substr(0, 2048));
}

// ============================================================================
// rsync 边界收口：不可读文件 / 哈希失败 / 计划降级 / 越界段
// ============================================================================

// generateRsyncBlockList 对不可读文件（目录）干净失败。
TEST_F(IncrementalDownloadTest, GenerateRsyncBlockListUnreadableFileFails) {
    IncrementalDownloader downloader;
    RsyncBlockList out;
    EXPECT_FALSE(downloader.generateRsyncBlockList(
        testDir_ + "/no_such_rsync.bin", 1024, out));
}


#if defined(FALCON_FAILURE_INJECTION)
// 哈希计算失败（init 注入）→ generateRsyncBlockList 干净失败。
TEST_F(IncrementalDownloadTest, GenerateRsyncBlockListHashInitFailFails) {
    const std::string path = createTestFile("rsync_hashinit.bin", 2048);
    IncrementalDownloader downloader;
    RsyncBlockList out;
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::IncrementalHashInit);
    EXPECT_FALSE(downloader.generateRsyncBlockList(path, 1024, out));
}
#endif

// planRsync 退化：blockSize 越界 / 空块 / 零文件尺寸 → 全量下载单段。
