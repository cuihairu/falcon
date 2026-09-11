/**
 * @file incremental_download_test.cpp
 * @brief 增量下载功能测试
 * @author Falcon Team
 * @date 2025-12-31
 */

#include <gtest/gtest.h>
#include <falcon/protocols/incremental_download.hpp>
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
using ssize_t = std::ptrdiff_t;
#define SHUT_WR SD_SEND
#define CLOSE_SOCKET(fd) closesocket(fd)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET(fd) close(fd)
#endif

#include <atomic>
#include <thread>

using namespace falcon;

class IncrementalDownloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto base = std::filesystem::temp_directory_path();
        auto unique = std::to_string(static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        testDir_ = (base / ("falcon_test_" + unique)).string();
        std::filesystem::create_directories(testDir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir_, ec);
    }

    // 创建测试文件
    std::string createTestFile(const std::string& name, size_t size) {
        std::string path = testDir_ + "/" + name;
        std::ofstream file(path, std::ios::binary);

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(0, 255);

        std::vector<uint8_t> data(size);
        for (auto& byte : data) {
            byte = static_cast<uint8_t>(dis(gen));
        }

        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        file.close();

        return path;
    }

    std::string testDir_;
};

// ============================================================================
// calculateHash 测试 (私有方法 - 暂时注释)
// ============================================================================

/*
TEST_F(IncrementalDownloadTest, CalculateHash_Sha256_Valid) {
    IncrementalDownloader downloader;

    std::string testData = "Hello, World!";
    std::string hash = downloader.calculateHash(testData, "sha256");

    // 验证哈希不为空且长度正确（SHA256 = 64 hex chars）
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(64, hash.length());

    // 验证相同输入产生相同哈希
    std::string hash2 = downloader.calculateHash(testData, "sha256");
    EXPECT_EQ(hash, hash2);

    // 验证不同输入产生不同哈希
    std::string differentData = "Hello, World?";
    std::string hash3 = downloader.calculateHash(differentData, "sha256");
    EXPECT_NE(hash, hash3);
}

TEST_F(IncrementalDownloadTest, CalculateHash_DifferentAlgorithms) {
    IncrementalDownloader downloader;

    std::string testData = "Test data";

    // 测试多种哈希算法
    std::vector<std::string> algorithms = {"sha256", "sha512", "md5", "sha1"};

    for (const auto& algo : algorithms) {
        std::string hash = downloader.calculateHash(testData, algo);
        EXPECT_FALSE(hash.empty()) << "Algorithm: " << algo;
        EXPECT_GT(hash.length(), 0) << "Algorithm: " << algo;
    }
}

TEST_F(IncrementalDownloadTest, CalculateHash_EmptyData) {
    IncrementalDownloader downloader;

    std::string emptyData = "";
    std::string hash = downloader.calculateHash(emptyData, "sha256");

    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(64, hash.length());
}

TEST_F(IncrementalDownloadTest, CalculateHash_LargeData) {
    IncrementalDownloader downloader;

    // 测试大数据（1MB）
    std::string largeData(1024 * 1024, 'A');
    std::string hash = downloader.calculateHash(largeData, "sha256");

    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(64, hash.length());
}

// ============================================================================
// calculateChunkHashes 测试
// ============================================================================

TEST_F(IncrementalDownloadTest, CalculateChunkHashes_SingleChunk) {
    IncrementalDownloader downloader;

    std::string filePath = createTestFile("test_single.bin", 1024);
    std::vector<ChunkInfo> chunks = downloader.calculateChunkHashes(filePath, 2048, "sha256");

    EXPECT_EQ(1, chunks.size());
    EXPECT_EQ(0, chunks[0].offset);
    EXPECT_EQ(1024, chunks[0].size);
    EXPECT_FALSE(chunks[0].hash.empty());
    EXPECT_FALSE(chunks[0].changed);
}

TEST_F(IncrementalDownloadTest, CalculateChunkHashes_MultipleChunks) {
    IncrementalDownloader downloader;

    // 创建 2.5MB 的文件，分块大小 1MB
    std::string filePath = createTestFile("test_multi.bin", 2560 * 1024);
    std::vector<ChunkInfo> chunks = downloader.calculateChunkHashes(filePath, 1024 * 1024, "sha256");

    EXPECT_EQ(3, chunks.size());

    // 验证第一个分块
    EXPECT_EQ(0, chunks[0].offset);
    EXPECT_EQ(1024 * 1024, chunks[0].size);

    // 验证第二个分块
    EXPECT_EQ(1024 * 1024, chunks[1].offset);
    EXPECT_EQ(1024 * 1024, chunks[1].size);

    // 验证第三个分块（部分）
    EXPECT_EQ(2048 * 1024, chunks[2].offset);
    EXPECT_EQ(512 * 1024, chunks[2].size);

    // 验证所有哈希值不同
    EXPECT_NE(chunks[0].hash, chunks[1].hash);
    EXPECT_NE(chunks[1].hash, chunks[2].hash);
}

TEST_F(IncrementalDownloadTest, CalculateChunkHashes_NonExistentFile) {
    IncrementalDownloader downloader;

    std::string nonExistentPath = testDir_ + "/non_existent.bin";
    std::vector<ChunkInfo> chunks = downloader.calculateChunkHashes(nonExistentPath, 1024, "sha256");

    EXPECT_TRUE(chunks.empty());
}

TEST_F(IncrementalDownloadTest, CalculateChunkHashes_EmptyFile) {
    IncrementalDownloader downloader;

    std::string emptyFilePath = testDir_ + "/empty.bin";
    std::ofstream(emptyFilePath).close();

    std::vector<ChunkInfo> chunks = downloader.calculateChunkHashes(emptyFilePath, 1024, "sha256");
    EXPECT_TRUE(chunks.empty());
}

// ============================================================================
// compareHashLists 测试
// ============================================================================

TEST_F(IncrementalDownloadTest, CompareHashLists_Identical) {
    IncrementalDownloader downloader;

    std::vector<ChunkInfo> local = {
        {0, 1024, "hash1", false},
        {1024, 1024, "hash2", false},
        {2048, 512, "hash3", false}
    };

    std::vector<ChunkInfo> remote = {
        {0, 1024, "hash1", false},
        {1024, 1024, "hash2", false},
        {2048, 512, "hash3", false}
    };

    std::vector<ChunkInfo> diff = downloader.compareHashLists(local, remote);

    EXPECT_EQ(3, diff.size());
    EXPECT_FALSE(diff[0].changed);
    EXPECT_FALSE(diff[1].changed);
    EXPECT_FALSE(diff[2].changed);
}

TEST_F(IncrementalDownloadTest, CompareHashLists_Different) {
    IncrementalDownloader downloader;

    std::vector<ChunkInfo> local = {
        {0, 1024, "hash1", false},
        {1024, 1024, "hash2_old", false},
        {2048, 512, "hash3", false}
    };

    std::vector<ChunkInfo> remote = {
        {0, 1024, "hash1", false},
        {1024, 1024, "hash2_new", false},
        {2048, 512, "hash3", false}
    };

    std::vector<ChunkInfo> diff = downloader.compareHashLists(local, remote);

    EXPECT_EQ(3, diff.size());
    EXPECT_FALSE(diff[0].changed);
    EXPECT_TRUE(diff[1].changed);
    EXPECT_FALSE(diff[2].changed);
}

TEST_F(IncrementalDownloadTest, CompareHashLists_RemoteLarger) {
    IncrementalDownloader downloader;

    std::vector<ChunkInfo> local = {
        {0, 1024, "hash1", false},
        {1024, 1024, "hash2", false}
    };

    std::vector<ChunkInfo> remote = {
        {0, 1024, "hash1", false},
        {1024, 1024, "hash2", false},
        {2048, 512, "hash3", false}
    };

    std::vector<ChunkInfo> diff = downloader.compareHashLists(local, remote);

    EXPECT_EQ(3, diff.size());
    EXPECT_FALSE(diff[0].changed);
    EXPECT_FALSE(diff[1].changed);
    EXPECT_TRUE(diff[2].changed);  // 新分块标记为变化
}

TEST_F(IncrementalDownloadTest, CompareHashLists_RemoteSmaller) {
    IncrementalDownloader downloader;

    std::vector<ChunkInfo> local = {
        {0, 1024, "hash1", false},
        {1024, 1024, "hash2", false},
        {2048, 512, "hash3", false}
    };

    std::vector<ChunkInfo> remote = {
        {0, 1024, "hash1", false},
        {1024, 1024, "hash2", false}
    };

    std::vector<ChunkInfo> diff = downloader.compareHashLists(local, remote);

    EXPECT_EQ(2, diff.size());
    EXPECT_FALSE(diff[0].changed);
    EXPECT_FALSE(diff[1].changed);
}
*/

// ============================================================================
// generateHashList 测试
// ============================================================================

TEST_F(IncrementalDownloadTest, GenerateHashList_ValidFile) {
#if !defined(FALCON_HAS_OPENSSL) && !defined(FALCON_USE_OPENSSL)
    GTEST_SKIP() << "OpenSSL not available";
#endif
    IncrementalDownloader downloader;

    std::string filePath = createTestFile("test_hashlist.bin", 2048);
    std::vector<ChunkInfo> chunks = downloader.generateHashList(filePath, 1024);

    EXPECT_EQ(2, chunks.size());

    // 验证哈希格式（64个十六进制字符）
    for (const auto& chunk : chunks) {
        EXPECT_EQ(64, chunk.hash.length());
        EXPECT_FALSE(chunk.changed);
    }
}

TEST_F(IncrementalDownloadTest, GenerateHashList_DifferentChunkSizes) {
    IncrementalDownloader downloader;

    std::string filePath = createTestFile("test_chunksizes.bin", 4096);

    // 测试不同分块大小
    std::vector<size_t> chunkSizes = {512, 1024, 2048, 8192};

    for (size_t chunkSize : chunkSizes) {
        std::vector<ChunkInfo> chunks = downloader.generateHashList(filePath, chunkSize);
        EXPECT_FALSE(chunks.empty()) << "Chunk size: " << chunkSize;

        // 验证分块大小正确
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (i < chunks.size() - 1) {
                EXPECT_EQ(chunkSize, chunks[i].size);
            } else {
                // 最后一个分块可能小于完整分块大小
                EXPECT_LE(chunks[i].size, chunkSize);
            }
        }
    }
}

// ============================================================================
// verifyFile 测试
// ============================================================================

TEST_F(IncrementalDownloadTest, VerifyFile_Valid) {
    IncrementalDownloader downloader;

    std::string filePath = createTestFile("test_verify.bin", 1024);

    // 计算正确哈希
    std::vector<ChunkInfo> chunks = downloader.generateHashList(filePath, 2048);
    ASSERT_FALSE(chunks.empty());
    std::string correctHash = chunks[0].hash;

    // 验证应该成功
    EXPECT_TRUE(downloader.verifyFile(filePath, correctHash));
}

TEST_F(IncrementalDownloadTest, VerifyFile_Invalid) {
    IncrementalDownloader downloader;

    std::string filePath = createTestFile("test_verify_invalid.bin", 1024);

    // 使用错误的哈希
    std::string wrongHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    EXPECT_FALSE(downloader.verifyFile(filePath, wrongHash));
}

TEST_F(IncrementalDownloadTest, VerifyFile_NonExistent) {
    IncrementalDownloader downloader;

    std::string nonExistentPath = testDir_ + "/non_existent.bin";
    std::string anyHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    EXPECT_FALSE(downloader.verifyFile(nonExistentPath, anyHash));
}

// ============================================================================
// compare 测试（集成测试）
// ============================================================================

TEST_F(IncrementalDownloadTest, Compare_Integration) {
    IncrementalDownloader downloader;

    // 创建本地文件
    std::string localPath = createTestFile("local.bin", 2048);

    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";

    // 立即拒绝的回环端口：哈希列表获取失败，应安全回退（chunks 为空）而不崩溃
    FileDiff diff = downloader.compare(localPath, "http://127.0.0.1:1/remote.bin", options);

    // 验证基本字段
    EXPECT_EQ(localPath, diff.localPath);
    EXPECT_EQ("http://127.0.0.1:1/remote.bin", diff.remotePath);
    EXPECT_GT(diff.localSize, 0);
}

// ============================================================================
// mergeFile 测试 (私有方法 - 暂时注释)
// ============================================================================

/*
TEST_F(IncrementalDownloadTest, MergeFile_Valid) {
    IncrementalDownloader downloader;

    // 创建本地文件
    std::string filePath = createTestFile("test_merge.bin", 2048);

    // 模拟变化的分块
    std::vector<std::vector<uint8_t>> changedChunks = {
        std::vector<uint8_t>(1024, 0xFF),  // 第一个分块数据
        std::vector<uint8_t>(1024, 0xAA)   // 第二个分块数据
    };

    std::vector<ChunkInfo> chunkInfo = {
        {0, 1024, "hash1", true},
        {1024, 1024, "hash2", true}
    };

    EXPECT_TRUE(downloader.mergeFile(filePath, changedChunks, chunkInfo));

    // 验证文件已被修改
    std::ifstream inFile(filePath, std::ios::binary);
    std::vector<uint8_t> fileData(2048);
    inFile.read(reinterpret_cast<char*>(fileData.data()), 2048);
    inFile.close();

    // 验证第一个分块被修改
    EXPECT_EQ(0xFF, fileData[0]);
    EXPECT_EQ(0xFF, fileData[1023]);
}

TEST_F(IncrementalDownloadTest, MergeFile_NonExistent) {
    IncrementalDownloader downloader;

    std::string nonExistentPath = testDir_ + "/non_existent.bin";

    std::vector<std::vector<uint8_t>> changedChunks = {
        std::vector<uint8_t>(1024, 0xFF)
    };

    std::vector<ChunkInfo> chunkInfo = {
        {0, 1024, "hash1", true}
    };

    EXPECT_FALSE(downloader.mergeFile(nonExistentPath, changedChunks, chunkInfo));
}
*/

// ============================================================================
// 性能测试
// ============================================================================

TEST_F(IncrementalDownloadTest, Performance_LargeFile) {
    IncrementalDownloader downloader;

    // 创建 10MB 文件
    std::string filePath = createTestFile("test_large.bin", 10 * 1024 * 1024);

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<ChunkInfo> chunks = downloader.generateHashList(filePath, 1024 * 1024);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 验证结果
    EXPECT_EQ(10, chunks.size());

    // 性能：应该在合理时间内完成（< 5秒）
    EXPECT_LT(duration.count(), 5000) << "Large file processing took too long: "
                                       << duration.count() << "ms";
}

TEST_F(IncrementalDownloadTest, Performance_ManySmallFiles) {
    IncrementalDownloader downloader;

    constexpr int fileCount = 100;
    constexpr int fileSize = 10 * 1024;  // 10KB

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < fileCount; ++i) {
        std::string filePath = createTestFile("test_small_" + std::to_string(i) + ".bin", fileSize);
        std::vector<ChunkInfo> chunks = downloader.generateHashList(filePath, fileSize);
        EXPECT_EQ(1, chunks.size());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 性能：100个10KB文件应该在合理时间内完成（< 3秒）
    EXPECT_LT(duration.count(), 3000) << "Many small files processing took too long: "
                                      << duration.count() << "ms";
}

// ============================================================================
// 哈希列表序列化 / 解析
// ============================================================================

TEST_F(IncrementalDownloadTest, HashListRoundTrip) {
    IncrementalDownloader downloader;

    const std::string path = createTestFile("roundtrip.bin", 3000);
    const auto chunks = downloader.generateHashList(path, 1024);
    ASSERT_EQ(chunks.size(), std::size_t{3});

    const std::string text =
        IncrementalDownloader::serializeHashList(chunks, 1024, "sha256", 3000);

    // 使用与元数据不同的默认值：元数据应覆盖
    const auto parsed = IncrementalDownloader::parseHashList(text, 512, "sha256");
    ASSERT_EQ(parsed.size(), chunks.size());
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(parsed[i].hash, chunks[i].hash) << "chunk " << i;
        EXPECT_EQ(parsed[i].offset, chunks[i].offset) << "chunk " << i;
        EXPECT_EQ(parsed[i].size, chunks[i].size) << "chunk " << i;
        EXPECT_FALSE(parsed[i].changed);
    }
    // 最后一块按 fileSize 收缩：3000 = 1024 + 1024 + 952
    EXPECT_EQ(parsed.back().size, 952u);
}

TEST_F(IncrementalDownloadTest, ParseHashListMetadataOverride) {
    const std::string text =
        "# falcon-hash-list v1\n"
        "# chunkSize: 10\n"
        "# algorithm: sha256\n"
        "# fileSize: 25\n"
        "# chunks: 3\n"
        "aabbccddeeff0011\n"
        "\n"
        "1122334455667788\n"
        "99aabbccddeeff00\n";

    const auto parsed = IncrementalDownloader::parseHashList(text, 999, "sha256");
    ASSERT_EQ(parsed.size(), std::size_t{3});
    EXPECT_EQ(parsed[0].offset, 0u);
    EXPECT_EQ(parsed[0].size, 10u);
    EXPECT_EQ(parsed[1].offset, 10u);
    EXPECT_EQ(parsed[1].size, 10u);
    EXPECT_EQ(parsed[2].offset, 20u);
    EXPECT_EQ(parsed[2].size, 5u);  // fileSize 收缩最后一块
}

TEST_F(IncrementalDownloadTest, ParseHashListRejectsCorruptInput) {
    // 非法哈希行
    const std::string bad_hex =
        "# chunkSize: 10\n# algorithm: sha256\nnot-hex!\n";
    EXPECT_TRUE(IncrementalDownloader::parseHashList(bad_hex, 10, "sha256").empty());

    // 奇数长度哈希
    const std::string odd_len =
        "# chunkSize: 10\n# algorithm: sha256\nabc\n";
    EXPECT_TRUE(IncrementalDownloader::parseHashList(odd_len, 10, "sha256").empty());

    // chunks 计数与哈希行数不符
    const std::string count_mismatch =
        "# chunkSize: 10\n# algorithm: sha256\n# chunks: 3\naabb\n";
    EXPECT_TRUE(IncrementalDownloader::parseHashList(count_mismatch, 10, "sha256").empty());

    // 算法不匹配（调用方期望 md5，列表是 sha256）
    const std::string algo_mismatch =
        "# chunkSize: 10\n# algorithm: sha256\naabb\n";
    EXPECT_TRUE(IncrementalDownloader::parseHashList(algo_mismatch, 10, "md5").empty());

    // 空输入 / 无哈希行
    EXPECT_TRUE(IncrementalDownloader::parseHashList("", 10, "sha256").empty());
    EXPECT_TRUE(IncrementalDownloader::parseHashList("# only comments\n", 10, "sha256").empty());
}

// ============================================================================
// 端到端：本地服务器 + compare + downloadChanged
// ============================================================================

namespace {

/// 支持文件 + 哈希列表两条路由的极简本地服务器
class IncrementalTestServer {
public:
    bool start(const std::string& file_body, const std::string& hash_list) {
        body_ = file_body;
        hash_list_ = hash_list;

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
            stop();
            return false;
        }
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this] { serve_loop(); });
        return true;
    }

    uint16_t port() const { return port_; }

    ~IncrementalTestServer() { stop(); }

private:
    void serve_loop() {
        while (listen_fd_ >= 0) {
#ifdef _WIN32
            WSAPOLLFD pfd{};
            pfd.fd = static_cast<SOCKET>(listen_fd_);
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (WSAPoll(&pfd, 1, 5000) <= 0) return;
#else
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (::poll(&pfd, 1, 5000) <= 0) return;
#endif
            const int conn = static_cast<int>(::accept(listen_fd_, nullptr, nullptr));
            if (conn < 0) return;
            handle_connection(conn);
        }
    }

    static void send_all(int conn, const std::string& data) {
        std::size_t off = 0;
        while (off < data.size()) {
            const ssize_t n = ::send(conn, data.data() + off, data.size() - off, 0);
            if (n <= 0) return;
            off += static_cast<std::size_t>(n);
        }
    }

    void handle_connection(int conn) {
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                CLOSE_SOCKET(conn);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }

        const auto path_begin = request.find(' ') + 1;
        const auto path_end = request.find(' ', path_begin);
        const std::string path = request.substr(path_begin, path_end - path_begin);

        if (path == "/file.bin.falconhash") {
            send_all(conn,
                     "HTTP/1.1 200 OK\r\nContent-Length: " +
                         std::to_string(hash_list_.size()) +
                         "\r\nConnection: close\r\n\r\n" + hash_list_);
        } else if (path == "/file.bin") {
            // 解析 Range: bytes=A-B
            Bytes range_start = 0;
            Bytes range_end = 0;
            bool has_range = false;
            const std::string marker = "Range: bytes=";
            const auto pos = request.find(marker);
            if (pos != std::string::npos) {
                const std::string value = request.substr(
                    pos + marker.size(),
                    request.find("\r\n", pos) - (pos + marker.size()));
                const auto dash = value.find('-');
                range_start = std::stoull(value.substr(0, dash));
                range_end = std::stoull(value.substr(dash + 1));
                has_range = true;
            }

            if (has_range) {
                const Bytes slice_len = range_end - range_start + 1;
                send_all(conn,
                         "HTTP/1.1 206 Partial Content\r\n"
                         "Content-Range: bytes " + std::to_string(range_start) + "-" +
                             std::to_string(range_end) + "/" +
                             std::to_string(body_.size()) + "\r\n"
                         "Content-Length: " + std::to_string(slice_len) +
                         "\r\nConnection: close\r\n\r\n" +
                         body_.substr(static_cast<std::size_t>(range_start),
                                      static_cast<std::size_t>(slice_len)));
            } else {
                send_all(conn,
                         "HTTP/1.1 200 OK\r\n"
                         "Accept-Ranges: bytes\r\n"
                         "Content-Length: " + std::to_string(body_.size()) +
                         "\r\nConnection: close\r\n\r\n" + body_);
            }
        } else {
            send_all(conn, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                           "Connection: close\r\n\r\n");
        }

        ::shutdown(conn, SHUT_WR);
        while (::recv(conn, buf, sizeof(buf), 0) > 0) {}
        CLOSE_SOCKET(conn);
    }

    std::string body_;
    std::string hash_list_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;

    void stop() {
        if (listen_fd_ >= 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }
};

/// 确定性内容生成
std::string make_content(std::size_t size, unsigned seed) {
    std::string content;
    content.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        content.push_back(static_cast<char>((i * 13 + seed) % 251));
    }
    return content;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST_F(IncrementalDownloadTest, CompareAndDownloadChangedEndToEnd) {
    const std::string remote_content = make_content(2500, 7);
    std::string local_content = remote_content;
    // 修改第二个分块（1024..2047）
    for (std::size_t i = 1024; i < 2048; ++i) {
        local_content[i] = static_cast<char>((local_content[i] + 11) % 251);
    }

    const std::string local_path = testDir_ + "/local_old.bin";
    {
        std::ofstream file(local_path, std::ios::binary);
        file.write(local_content.data(), static_cast<std::streamsize>(local_content.size()));
    }

    // 远程哈希列表由远程内容生成
    const std::string remote_path = testDir_ + "/remote_new.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(remote_content.data(), static_cast<std::streamsize>(remote_content.size()));
    }
    IncrementalDownloader list_generator;
    const auto remote_chunks = list_generator.generateHashList(remote_path, 1024);
    ASSERT_EQ(remote_chunks.size(), std::size_t{3});

    IncrementalTestServer server;
    ASSERT_TRUE(server.start(
        remote_content,
        IncrementalDownloader::serializeHashList(remote_chunks, 1024, "sha256", 2500)));

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";

    const FileDiff diff = downloader.compare(
        local_path, "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);

    // 只有第二个分块变化
    ASSERT_EQ(diff.chunks.size(), std::size_t{3});
    EXPECT_FALSE(diff.chunks[0].changed);
    EXPECT_TRUE(diff.chunks[1].changed);
    EXPECT_FALSE(diff.chunks[2].changed);
    EXPECT_EQ(diff.totalChanged, 1024u);
    EXPECT_EQ(diff.remoteSize, 2500u);
    EXPECT_NEAR(diff.ratio, 1024.0 / 2500.0, 0.001);

    // 下载变化部分，输出应与远程内容一致
    const std::string out_path = testDir_ + "/updated.bin";
    uint64_t progress_seen = 0;
    ASSERT_TRUE(downloader.downloadChanged(
        diff, out_path,
        [&progress_seen](uint64_t downloaded, uint64_t /*total*/) {
            progress_seen = downloaded;
        }));
    EXPECT_GT(progress_seen, 0u);
    EXPECT_EQ(read_file(out_path), remote_content);
}

TEST_F(IncrementalDownloadTest, CompareNoChangesDownloadsNothing) {
    const std::string content = make_content(2048, 42);

    const std::string local_path = testDir_ + "/same_local.bin";
    const std::string remote_path = testDir_ + "/same_remote.bin";
    for (const auto& path : {local_path, remote_path}) {
        std::ofstream file(path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader generator;
    const auto chunks = generator.generateHashList(remote_path, 1024);

    IncrementalTestServer server;
    ASSERT_TRUE(server.start(
        content, IncrementalDownloader::serializeHashList(chunks, 1024, "sha256", 2048)));

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";

    const FileDiff diff = downloader.compare(
        local_path, "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);

    EXPECT_EQ(diff.totalChanged, 0u);
    EXPECT_NEAR(diff.ratio, 0.0, 1e-9);

    // 无变化时不应发起任何 Range 请求即可产出一致文件
    const std::string out_path = testDir_ + "/same_out.bin";
    ASSERT_TRUE(downloader.downloadChanged(diff, out_path));
    EXPECT_EQ(read_file(out_path), content);
}

TEST_F(IncrementalDownloadTest, CompareMissingLocalDownloadsEverything) {
    const std::string content = make_content(2048, 99);
    const std::string remote_path = testDir_ + "/fresh_remote.bin";
    {
        std::ofstream file(remote_path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    IncrementalDownloader generator;
    const auto chunks = generator.generateHashList(remote_path, 1024);

    IncrementalTestServer server;
    ASSERT_TRUE(server.start(
        content, IncrementalDownloader::serializeHashList(chunks, 1024, "sha256", 2048)));

    IncrementalDownloader downloader;
    IncrementalDownloader::Options options;
    options.chunkSize = 1024;
    options.hashAlgorithm = "sha256";

    // 本地文件不存在：所有分块都视为变化
    const FileDiff diff = downloader.compare(
        testDir_ + "/does_not_exist.bin",
        "http://127.0.0.1:" + std::to_string(server.port()) + "/file.bin", options);

    ASSERT_EQ(diff.chunks.size(), std::size_t{2});
    EXPECT_TRUE(diff.chunks[0].changed);
    EXPECT_TRUE(diff.chunks[1].changed);
    EXPECT_EQ(diff.totalChanged, 2048u);

    const std::string out_path = testDir_ + "/fresh_out.bin";
    ASSERT_TRUE(downloader.downloadChanged(diff, out_path));
    EXPECT_EQ(read_file(out_path), content);
}
