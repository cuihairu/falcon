/**
 * @file file_hash.cpp
 * @brief 文件哈希校验实现
 * @author Falcon Team
 * @date 2025-12-25
 */

#include <falcon/file_hash.hpp>
#include <falcon/logger.hpp>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <functional>
#include <algorithm>

// OpenSSL 3.0 使用 EVP API 替代弃用的低级函数
#include <openssl/evp.h>

namespace falcon {

//==============================================================================
// FileHasher 实现
//==============================================================================

std::string FileHasher::calculate(const std::string& file_path,
                                   HashAlgorithm algorithm) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        FALCON_LOG_ERROR("无法打开文件: " << file_path);
        return "";
    }

    constexpr std::size_t BUFFER_SIZE = 8192;
    char buffer[BUFFER_SIZE];
    std::vector<unsigned char> hash_result;
    std::vector<unsigned char> data;

    // 首先读取整个文件到内存（对于大文件应该使用流式处理）
    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size > 100 * 1024 * 1024) {  // > 100MB 警告
        FALCON_LOG_WARN("大文件哈希计算可能较慢: " << file_path);
    }

    while (file.read(buffer, BUFFER_SIZE)) {
        data.insert(data.end(), buffer, buffer + file.gcount());
    }
    if (file.gcount() > 0) {
        data.insert(data.end(), buffer, buffer + file.gcount());
    }

    return calculate(reinterpret_cast<const char*>(data.data()), data.size(), algorithm);
}

std::string FileHasher::calculate(const char* data, std::size_t size,
                                   HashAlgorithm algorithm) {
    // 选择哈希算法
    const char* md_type = nullptr;
    switch (algorithm) {
        case HashAlgorithm::MD5:    md_type = "MD5"; break;
        case HashAlgorithm::SHA1:   md_type = "SHA1"; break;
        case HashAlgorithm::SHA256: md_type = "SHA256"; break;
        case HashAlgorithm::SHA512: md_type = "SHA512"; break;
    }

    // 使用 OpenSSL 3.0 EVP API
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        FALCON_LOG_ERROR("创建 EVP_MD_CTX 失败");
        return "";
    }

    const EVP_MD* md = EVP_get_digestbyname(md_type);
    if (!md) {
        FALCON_LOG_ERROR("获取哈希算法失败: " << md_type);
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    if (EVP_DigestInit_ex(mdctx, md, nullptr) != 1) {
        FALCON_LOG_ERROR("初始化哈希失败");
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    if (EVP_DigestUpdate(mdctx, data, size) != 1) {
        FALCON_LOG_ERROR("更新哈希失败");
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    unsigned char hash_value[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(mdctx, hash_value, &hash_len) != 1) {
        FALCON_LOG_ERROR("完成哈希失败");
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    EVP_MD_CTX_free(mdctx);

    // 转换为十六进制字符串
    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash_value[i]);
    }

    return oss.str();
}

HashResult FileHasher::verify(const std::string& file_path,
                               const std::string& expected_hash,
                               HashAlgorithm algorithm) {
    HashResult result;
    result.algorithm = algorithm;
    result.expected = expected_hash;

    // 计算实际哈希
    result.calculated = calculate(file_path, algorithm);

    // 比较哈希值（不区分大小写）
    if (result.calculated.length() == result.expected.length()) {
        result.valid = std::equal(
            result.calculated.begin(), result.calculated.end(),
            result.expected.begin(),
            [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            }
        );
    }

    return result;
}

std::vector<HashResult> FileHasher::verify_multiple(
    const std::string& file_path,
    const std::vector<std::pair<std::string, HashAlgorithm>>& expected_hashes) {
    std::vector<HashResult> results;

    for (const auto& [hash, algo] : expected_hashes) {
        results.push_back(verify(file_path, hash, algo));
    }

    return results;
}

HashAlgorithm FileHasher::detect_algorithm(const std::string& hash) {
    switch (hash.length()) {
        case 32: return HashAlgorithm::MD5;
        case 40: return HashAlgorithm::SHA1;
        case 64: return HashAlgorithm::SHA256;
        case 128: return HashAlgorithm::SHA512;
        default: return HashAlgorithm::SHA256;  // 默认 SHA256
    }
}

std::size_t FileHasher::get_hash_length(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::MD5:    return 32;
        case HashAlgorithm::SHA1:   return 40;
        case HashAlgorithm::SHA256: return 64;
        case HashAlgorithm::SHA512: return 128;
        default: return 64;
    }
}

//==============================================================================
// HashVerifyCommand 实现
//==============================================================================

HashVerifyCommand::HashVerifyCommand(const std::string& file_path,
                                     const std::string& expected_hash,
                                     HashAlgorithm algorithm)
    : file_path_(file_path)
    , expected_hash_(expected_hash)
    , algorithm_(algorithm)
{
}

bool HashVerifyCommand::execute() const {
    result_ = FileHasher::verify(file_path_, expected_hash_, algorithm_);

    if (result_.valid) {
        FALCON_LOG_INFO("哈希验证通过: " << file_path_);
    } else {
        FALCON_LOG_WARN("哈希验证失败: " << file_path_);
        FALCON_LOG_WARN("  期望: " << result_.expected);
        FALCON_LOG_WARN("  实际: " << result_.calculated);
    }

    return result_.valid;
}

//==============================================================================
// PieceHashVerifier 实现
//==============================================================================

PieceHashVerifier::PieceHashVerifier(std::size_t piece_size,
                                     std::vector<std::string> piece_hashes)
    : piece_size_(piece_size)
    , piece_hashes_(std::move(piece_hashes))
{
}

std::vector<bool> PieceHashVerifier::verify(const std::string& file_path) const {
    std::vector<bool> results(piece_hashes_.size(), false);

    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        FALCON_LOG_ERROR("无法打开文件: " << file_path);
        return results;
    }

    std::vector<char> buffer(piece_size_);

    for (std::size_t i = 0; i < piece_hashes_.size(); ++i) {
        file.read(buffer.data(), static_cast<std::streamsize>(piece_size_));
        std::size_t bytes_read = static_cast<std::size_t>(file.gcount());

        if (bytes_read == 0) {
            break;  // 文件结束
        }

        // 计算当前块的 SHA1 哈希
        std::string calculated = FileHasher::calculate(buffer.data(), bytes_read, HashAlgorithm::SHA1);

        // 比较（不区分大小写）
        results[i] = (calculated.length() == piece_hashes_[i].length() &&
            std::equal(calculated.begin(), calculated.end(),
                      piece_hashes_[i].begin(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      }));
    }

    std::size_t valid_count = std::count(results.begin(), results.end(), true);
    FALCON_LOG_INFO("分块哈希验证完成: " << valid_count << "/" << results.size() << " 通过");

    return results;
}

} // namespace falcon
