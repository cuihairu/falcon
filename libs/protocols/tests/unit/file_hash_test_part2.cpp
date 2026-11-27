/**
 * @file file_hash_test_part2.cpp
 * @brief 文件哈希验证单元测试
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <gtest/gtest.h>
#include <falcon/protocols/file_hash.hpp>
#include <falcon/detail/injection.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace falcon;

// Cryptographic hash tests require OpenSSL; skip when unavailable
#if !defined(FALCON_HAS_OPENSSL) && !defined(FALCON_USE_OPENSSL)
#define SKIP_NO_OPENSSL() GTEST_SKIP() << "OpenSSL not available"
#else
#define SKIP_NO_OPENSSL() (void)0
#endif

//==============================================================================
// 测试辅助函数
//==============================================================================

/**
 * @brief 创建测试文件
 */

TEST(PieceHashVerifier, LargePieceCount) {
    std::string path = make_unique_temp_path("test_pieces_many.bin");

    // 创建包含多个分块的文件
    constexpr std::size_t piece_size = 1024;  // 1 KB
    constexpr std::size_t num_pieces = 1000;
    std::vector<char> data(piece_size * num_pieces, 'X');

    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();

    // 创建假哈希列表
    std::vector<std::string> piece_hashes(num_pieces,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    PieceHashVerifier verifier(piece_size, std::move(piece_hashes));
    auto results = verifier.verify(path);

    EXPECT_EQ(results.size(), num_pieces);

    remove_test_file(path);
}

//==============================================================================
// HashVerifyCommand 测试
//==============================================================================

TEST(HashVerifyCommand, BasicExecution) {
    SKIP_NO_OPENSSL();
    std::string path = make_unique_temp_path("test_verify_cmd.txt");
    create_test_file(path, "Hello, World!");

    std::string expected_hash = get_md5_hash("Hello, World!");
    HashVerifyCommand cmd(path, expected_hash, HashAlgorithm::MD5);

    bool result = cmd.execute();

    EXPECT_TRUE(result);
    EXPECT_TRUE(cmd.get_result().valid);

    remove_test_file(path);
}

TEST(HashVerifyCommand, FailedExecution) {
    std::string path = make_unique_temp_path("test_verify_cmd_fail.txt");
    create_test_file(path, "Hello, World!");

    std::string wrong_hash = "00000000000000000000000000000000";
    HashVerifyCommand cmd(path, wrong_hash, HashAlgorithm::MD5);

    bool result = cmd.execute();

    EXPECT_FALSE(result);
    EXPECT_FALSE(cmd.get_result().valid);

    remove_test_file(path);
}

//==============================================================================
// 压力测试
//==============================================================================

TEST(FileHashStress, RapidHashCalculations) {
    SKIP_NO_OPENSSL();
    std::string path = make_unique_temp_path("test_stress.txt");
    create_test_file(path, "Stress test data");

    constexpr int num_iterations = 1000;
    std::string last_hash;

    for (int i = 0; i < num_iterations; ++i) {
        auto hash = FileHasher::calculate(path, HashAlgorithm::MD5);

        if (i == 0) {
            last_hash = hash;
        } else {
            EXPECT_EQ(hash, last_hash);  // 所有哈希应该相同
        }

        EXPECT_FALSE(hash.empty());
    }

    remove_test_file(path);
}

TEST(FileHashStress, MultipleAlgorithmSwitching) {
    SKIP_NO_OPENSSL();
    std::string path = make_unique_temp_path("test_algo_switch.txt");
    std::string content(1024 * 100, 'S');  // 100 KB
    create_test_file(path, content);

    std::vector<HashAlgorithm> algorithms = {
        HashAlgorithm::MD5,
        HashAlgorithm::SHA1,
        HashAlgorithm::SHA256,
        HashAlgorithm::SHA512
    };

    constexpr int num_iterations = 100;

    for (int i = 0; i < num_iterations; ++i) {
        for (auto algo : algorithms) {
            auto hash = FileHasher::calculate(path, algo);
            EXPECT_FALSE(hash.empty());
        }
    }

    remove_test_file(path);
}

// 覆盖率批次 O：未知算法枚举的防御路径
TEST(FileHashTest, CalculateUnknownAlgorithmFailsGracefully) {
    SKIP_NO_OPENSSL();
    // switch 无 default：未知枚举 → md_type 为空 → 获取哈希算法失败防御
    const std::string path = create_test_file("unknown_alg.bin", "falcon");
    EXPECT_TRUE(
        FileHasher::calculate(path, static_cast<HashAlgorithm>(99)).empty());
    remove_test_file(path);
}

TEST(FileHashTest, GetHashLengthUnknownAlgorithmDefaultsTo64) {
    EXPECT_EQ(FileHasher::get_hash_length(static_cast<HashAlgorithm>(99)),
              std::size_t{64});
}

// 覆盖率批次 X:calculate_streaming 的算法矩阵与文件层防御
TEST(FileHashTest, StreamingFileMissingFailsGracefully) {
    SKIP_NO_OPENSSL();
    EXPECT_TRUE(FileHasher::calculate_streaming(
                    "/nonexistent/falcon-no-such-file.bin", HashAlgorithm::MD5)
                    .empty());
}

TEST(FileHashTest, StreamingMatchesCalculateForAllAlgorithms) {
    SKIP_NO_OPENSSL();
    const std::string path =
        create_test_file("stream_alg_matrix.bin", "falcon streaming matrix");
    for (auto algo : {HashAlgorithm::MD5, HashAlgorithm::SHA1,
                      HashAlgorithm::SHA256, HashAlgorithm::SHA512}) {
        EXPECT_EQ(FileHasher::calculate_streaming(path, algo),
                  FileHasher::calculate(path, algo))
            << "algorithm mismatch: " << static_cast<int>(algo);
    }
    remove_test_file(path);
}

TEST(FileHashTest, StreamingUnknownAlgorithmFailsGracefully) {
    SKIP_NO_OPENSSL();
    // md_type 置空 → EVP 获取失败防御(与非流式 default 互相独立)
    const std::string path =
        create_test_file("stream_unknown_alg.bin", "falcon");
    EXPECT_TRUE(FileHasher::calculate_streaming(
                    path, static_cast<HashAlgorithm>(99))
                    .empty());
    remove_test_file(path);
}

TEST(FileHashTest, CalculateLargeFileLogsWarning) {
    SKIP_NO_OPENSSL();
    // >100MB 触发慢速告警日志分支;稀疏文件不占磁盘
    const std::string path = make_unique_temp_path("large_sparse.bin");
    {
        std::ofstream file(path, std::ios::binary);
        file.seekp(100 * 1024 * 1024 + 1);
        file.put('\0');
    }
    EXPECT_FALSE(FileHasher::calculate(path, HashAlgorithm::MD5).empty());
    remove_test_file(path);
}

//==============================================================================
// 故障注入:EVP 失败防御(仅测试构建编入注入点)
//==============================================================================

#if defined(FALCON_FAILURE_INJECTION) && \
    (defined(FALCON_HAS_OPENSSL) || defined(FALCON_USE_OPENSSL))

TEST(FileHashInjection, CalculateEvpDefenseChain) {
    const std::string content = "falcon injection";
    // 基准:未注入时的正常结果
    const std::string good =
        FileHasher::calculate(content.data(), content.size(), HashAlgorithm::MD5);
    ASSERT_FALSE(good.empty());
    // 逐个置位,验证四层 EVP 防御各自返回空串
    {
        detail::ScopedInjection guard(detail::InjectPoint::FileHashCalcCtxNew);
        EXPECT_TRUE(FileHasher::calculate(content.data(), content.size(),
                                          HashAlgorithm::MD5)
                        .empty());
    }
    {
        detail::ScopedInjection guard(
            detail::InjectPoint::FileHashCalcDigestInit);
        EXPECT_TRUE(FileHasher::calculate(content.data(), content.size(),
                                          HashAlgorithm::MD5)
                        .empty());
    }
    {
        detail::ScopedInjection guard(
            detail::InjectPoint::FileHashCalcDigestUpdate);
        EXPECT_TRUE(FileHasher::calculate(content.data(), content.size(),
                                          HashAlgorithm::MD5)
                        .empty());
    }
    {
        detail::ScopedInjection guard(
            detail::InjectPoint::FileHashCalcDigestFinal);
        EXPECT_TRUE(FileHasher::calculate(content.data(), content.size(),
                                          HashAlgorithm::MD5)
                        .empty());
    }
    // 注入清除后恢复正常
    EXPECT_EQ(FileHasher::calculate(content.data(), content.size(),
                                    HashAlgorithm::MD5),
              good);
}

TEST(FileHashInjection, StreamingEvpDefenseChain) {
    const std::string path =
        create_test_file("inj_stream.bin", "falcon injection streaming");
    {
        detail::ScopedInjection guard(detail::InjectPoint::FileHashStreamCtxNew);
        EXPECT_TRUE(
            FileHasher::calculate_streaming(path, HashAlgorithm::MD5).empty());
    }
    {
        detail::ScopedInjection guard(
            detail::InjectPoint::FileHashStreamDigestInit);
        EXPECT_TRUE(
            FileHasher::calculate_streaming(path, HashAlgorithm::MD5).empty());
    }
    {
        detail::ScopedInjection guard(
            detail::InjectPoint::FileHashStreamDigestUpdate);
        EXPECT_TRUE(
            FileHasher::calculate_streaming(path, HashAlgorithm::MD5).empty());
    }
    {
        detail::ScopedInjection guard(
            detail::InjectPoint::FileHashStreamDigestFinal);
        EXPECT_TRUE(
            FileHasher::calculate_streaming(path, HashAlgorithm::MD5).empty());
    }
    // 注入清除后与内存版一致
    EXPECT_FALSE(
        FileHasher::calculate_streaming(path, HashAlgorithm::MD5).empty());
    remove_test_file(path);
}

TEST(FileHashInjection, VerifyPathPropagatesInjectedFailure) {
    // verify → calculate 注入失败:valid=false 且 calculated 为空
    const std::string path = create_test_file("inj_verify.bin", "falcon");
    const std::string good = FileHasher::calculate(path, HashAlgorithm::MD5);
    ASSERT_FALSE(good.empty());
    {
        detail::ScopedInjection guard(
            detail::InjectPoint::FileHashCalcDigestUpdate);
        const HashResult r = FileHasher::verify(path, good, HashAlgorithm::MD5);
        EXPECT_FALSE(r.valid);
        EXPECT_TRUE(r.calculated.empty());
    }
    remove_test_file(path);
}

#endif  // FALCON_FAILURE_INJECTION && OpenSSL

//==============================================================================
// 主函数
//==============================================================================
