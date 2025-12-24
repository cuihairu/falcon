/**
 * @file file_hash.hpp
 * @brief 文件哈希校验 - aria2 风格
 * @author Falcon Team
 * @date 2025-12-25
 *
 * 设计参考: aria2 的文件校验机制
 */

#pragma once

#include <falcon/types.hpp>
#include <string>
#include <vector>
#include <memory>

namespace falcon {

/**
 * @brief 哈希算法类型
 */
enum class HashAlgorithm {
    MD5,
    SHA1,
    SHA256,
    SHA512
};

/**
 * @brief 哈希校验结果
 */
struct HashResult {
    HashAlgorithm algorithm;
    std::string expected;      // 期望的哈希值
    std::string calculated;    // 计算的哈希值
    bool valid = false;        // 是否匹配
};

/**
 * @brief 文件哈希计算器
 *
 * 提供文件完整性校验功能
 */
class FileHasher {
public:
    /**
     * @brief 计算文件的哈希值
     *
     * @param file_path 文件路径
     * @param algorithm 哈希算法
     * @return 计算的哈希值（十六进制字符串）
     */
    static std::string calculate(const std::string& file_path,
                                  HashAlgorithm algorithm);

    /**
     * @brief 计算文件块的哈希值
     *
     * @param data 数据指针
     * @param size 数据大小
     * @param algorithm 哈希算法
     * @return 计算的哈希值（十六进制字符串）
     */
    static std::string calculate(const char* data, std::size_t size,
                                  HashAlgorithm algorithm);

    /**
     * @brief 验证文件哈希
     *
     * @param file_path 文件路径
     * @param expected_hash 期望的哈希值
     * @param algorithm 哈希算法
     * @return 验证结果
     */
    static HashResult verify(const std::string& file_path,
                             const std::string& expected_hash,
                             HashAlgorithm algorithm);

    /**
     * @brief 批量验证文件哈希
     *
     * @param file_path 文件路径
     * @param expected_hashes 期望的哈希值列表
     * @return 验证结果列表
     */
    static std::vector<HashResult> verify_multiple(
        const std::string& file_path,
        const std::vector<std::pair<std::string, HashAlgorithm>>& expected_hashes);

    /**
     * @brief 从哈希字符串自动检测算法
     *
     * @param hash 哈希字符串
     * @return 检测到的算法
     */
    static HashAlgorithm detect_algorithm(const std::string& hash);

    /**
     * @brief 获取算法对应的哈希长度
     */
    static std::size_t get_hash_length(HashAlgorithm algorithm);
};

/**
 * @brief 哈希校验命令（用于下载后验证）
 */
class HashVerifyCommand {
public:
    /**
     * @brief 构造函数
     *
     * @param file_path 要验证的文件路径
     * @param expected_hash 期望的哈希值
     * @param algorithm 哈希算法
     */
    HashVerifyCommand(const std::string& file_path,
                      const std::string& expected_hash,
                      HashAlgorithm algorithm = HashAlgorithm::SHA256);

    /**
     * @brief 执行验证
     *
     * @return true 验证通过
     */
    bool execute() const;

    /**
     * @brief 获取验证结果
     */
    const HashResult& result() const { return result_; }

private:
    std::string file_path_;
    std::string expected_hash_;
    HashAlgorithm algorithm_;
    mutable HashResult result_;
};

/**
 * @brief BitTorrent 样式的哈希列表校验
 *
 * 用于分段校验大文件
 */
class PieceHashVerifier {
public:
    /**
     * @brief 构造函数
     *
     * @param piece_size 分块大小
     * @param piece_hashes 分块哈希列表（SHA1）
     */
    PieceHashVerifier(std::size_t piece_size,
                      std::vector<std::string> piece_hashes);

    /**
     * @brief 验证文件分块
     *
     * @param file_path 文件路径
     * @return 验证结果（每块的验证状态）
     */
    std::vector<bool> verify(const std::string& file_path) const;

    /**
     * @brief 获取分块数量
     */
    std::size_t piece_count() const { return piece_hashes_.size(); }

private:
    std::size_t piece_size_;
    std::vector<std::string> piece_hashes_;
};

} // namespace falcon
