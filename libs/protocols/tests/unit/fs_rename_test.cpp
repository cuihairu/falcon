/// @file fs_rename_test.cpp
/// 发布改名重试 helper 的单元测试。
///
/// POSIX 的 rename 不检查打开句柄（无法在本地构造 Windows 杀毒/索引的
/// sharing violation），重试循环的尝试计数与放弃判定经 rename_with_retry_
/// fn 注入脚本化错误码验证；真实文件系统路径只验证透传语义。

#include <falcon/protocols/fs_rename.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr int kTransientErr = 32;   // ERROR_SHARING_VIOLATION
constexpr int kTransientErr2 = 33;  // ERROR_LOCK_VIOLATION
constexpr int kPermanentErr = 5;    // ERROR_ACCESS_DENIED
#else
constexpr int kTransientErr = EBUSY;
constexpr int kTransientErr2 = ETXTBSY;
constexpr int kPermanentErr = EACCES;
#endif

std::error_code make_ec(int value) {
    return std::error_code(value, std::generic_category());
}

/// 脚本化改名：按序返回错误码，空串表示成功
struct ScriptedRename {
    std::vector<int> errors;  // 每次调用的错误码；序列耗尽后成功
    int calls = 0;

    void operator()(const fs::path&, const fs::path&, std::error_code& ec) {
        ++calls;  // 无条件计数：脚本耗尽后的成功调用也要计入
        const int value = calls <= static_cast<int>(errors.size())
                              ? errors[static_cast<std::size_t>(calls - 1)]
                              : 0;
        if (value != 0) {
            ec = make_ec(value);
        }
    }
};

TEST(RenameRetryFn, RetriesTransientThenSucceeds) {
    ScriptedRename fn;
    fn.errors = {kTransientErr, kTransientErr2};  // 两族瞬时错误各一
    const auto rr = falcon::protocols::detail::rename_with_retry_fn(
        fn, "a.tmp", "a.bin", {1, 1, 1});
    EXPECT_TRUE(rr.ok);
    EXPECT_TRUE(rr.error.empty());
    EXPECT_EQ(fn.calls, 3);
}

TEST(RenameRetryFn, GivesUpAfterExhaustedTransientRetries) {
    ScriptedRename fn;
    fn.errors = {kTransientErr, kTransientErr, kTransientErr,
                 kTransientErr};  // 恒占用：4 次尝试全灭
    const auto rr = falcon::protocols::detail::rename_with_retry_fn(
        fn, "a.tmp", "a.bin", {1, 1, 1});
    EXPECT_FALSE(rr.ok);
    EXPECT_FALSE(rr.error.empty());
    EXPECT_EQ(fn.calls, 4);  // 1 + delays.size()
}

TEST(RenameRetryFn, PermanentErrorSkipsRetry) {
    ScriptedRename fn;
    fn.errors = {kPermanentErr, kPermanentErr};
    const auto rr = falcon::protocols::detail::rename_with_retry_fn(
        fn, "a.tmp", "a.bin", {50, 50});
    EXPECT_FALSE(rr.ok);
    EXPECT_FALSE(rr.error.empty());
    EXPECT_EQ(fn.calls, 1);  // 目录占用/权限等永久错误第一次就收口
}

TEST(RenameRetryFn, FirstAttemptSuccessDoesNotSleep) {
    ScriptedRename fn;  // 空脚本 = 一次成功
    const auto start = std::chrono::steady_clock::now();
    const auto rr = falcon::protocols::detail::rename_with_retry_fn(
        fn, "a.tmp", "a.bin", {10000});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(rr.ok);
    EXPECT_EQ(fn.calls, 1);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST(RenameWithRetry, RealRenameMovesFile) {
    const fs::path dir = fs::temp_directory_path() /
                         ("falcon_fs_rename_test_" +
                          std::to_string(
                              std::hash<std::uint64_t>{}(
                                  static_cast<std::uint64_t>(
                                      std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()))));
    fs::create_directories(dir);
    const fs::path from = dir / "published.tmp";
    const fs::path to = dir / "published.bin";
    { std::ofstream f(from, std::ios::binary); f << "payload"; }

    const auto rr = falcon::protocols::detail::rename_with_retry(from, to);
    EXPECT_TRUE(rr.ok);
    EXPECT_FALSE(fs::exists(from));
    EXPECT_TRUE(fs::exists(to));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(RenameWithRetry, PermanentRealErrorFailsFast) {
    // 目标父目录不存在 = 永久性错误（ENOENT），重试不可能改变结果——
    // 用 3s 档退避证明门禁跳过了重试（错误立即返回）
    const fs::path dir = fs::temp_directory_path() /
                         ("falcon_fs_rename_test_" +
                          std::to_string(
                              std::hash<std::uint64_t>{}(
                                  static_cast<std::uint64_t>(
                                      std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()))));
    fs::create_directories(dir);
    const fs::path from = dir / "x.tmp";
    { std::ofstream f(from, std::ios::binary); f << "x"; }
    const fs::path to = dir / "missing_parent" / "out.bin";

    const auto start = std::chrono::steady_clock::now();
    const auto rr = falcon::protocols::detail::rename_with_retry(
        from, to, {3000, 3000});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(rr.ok);
    EXPECT_FALSE(rr.error.empty());
    EXPECT_LT(elapsed, std::chrono::seconds(1));
    EXPECT_TRUE(fs::exists(from));  // 源未被消费

    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace
