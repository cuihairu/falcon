/**
 * @file incremental_download.hpp
 * @brief 增量下载功能
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace falcon {

/**
 * @enum DiffAlgorithm
 * @brief 差异比较算法
 */
enum class DiffAlgorithm {
    Rsync,      // 使用 rsync 算法
    HashChunk,  // 基于哈希的分块比较
    BinaryDiff  // 二进制差异
};

/**
 * @struct ChunkInfo
 * @brief 分块信息
 */
struct ChunkInfo {
    uint64_t offset;        // 偏移量
    uint64_t size;          // 大小
    std::string hash;       // 哈希值
    bool changed;           // 是否变化
};

/**
 * @struct FileDiff
 * @brief 文件差异信息
 */
struct FileDiff {
    std::string localPath;           // 本地文件路径
    std::string remotePath;          // 远程文件路径
    uint64_t localSize;              // 本地文件大小
    uint64_t remoteSize;             // 远程文件大小
    std::vector<ChunkInfo> chunks;   // 分块差异
    uint64_t totalChanged;           // 变化的总字节数
    double ratio;                    // 变化比例 (0.0 - 1.0)
};

/**
 * @class IncrementalDownloader
 * @brief 增量下载器
 *
 * 支持以下特性：
 * - 只下载文件变化的部分
 * - 基于哈希的分块比较
 * - rsync 算法支持
 * - 节省带宽和下载时间
 */
class IncrementalDownloader {
public:
    /**
     * @brief 配置选项
     */
    struct Options {
        DiffAlgorithm algorithm;          // 差异算法
        uint64_t chunkSize;               // 分块大小
        std::string hashAlgorithm;        // 哈希算法
        bool verifyDownload;              // 验证下载
        int maxRetries;                   // 最大重试次数

        Options()
            : algorithm(DiffAlgorithm::HashChunk)
            , chunkSize(1024 * 1024)      // 1MB
            , hashAlgorithm("sha256")
            , verifyDownload(true)
            , maxRetries(3) {}
    };

    /**
     * @brief 进度回调
     */
    using ProgressCallback = std::function<void(uint64_t downloaded, uint64_t total)>;

    /**
     * @brief 构造函数
     */
    IncrementalDownloader();

    /**
     * @brief 析构函数
     */
    ~IncrementalDownloader();

    /**
     * @brief 比较本地和远程文件
     */
    FileDiff compare(const std::string& localPath,
                    const std::string& remoteUrl,
                    const Options& options = Options());

    /**
     * @brief 下载变化的部分
     */
    bool downloadChanged(const FileDiff& diff,
                        const std::string& outputPath,
                        ProgressCallback callback = nullptr);

    /**
     * @brief 应用增量更新
     */
    bool applyPatch(const std::string& localPath,
                   const std::string& patchData,
                   const FileDiff& diff);

    /**
     * @brief 生成本地文件的哈希列表
     */
    std::vector<ChunkInfo> generateHashList(const std::string& filePath,
                                           uint64_t chunkSize);

    /**
     * @brief 验证文件哈希
     */
    bool verifyFile(const std::string& filePath,
                   const std::string& expectedHash);

private:
    /**
     * @brief 计算文件哈希
     */
    std::string calculateHash(const std::string& data,
                             const std::string& algorithm = "sha256");

    /**
     * @brief 计算文件分块哈希
     */
    std::vector<ChunkInfo> calculateChunkHashes(const std::string& filePath,
                                               uint64_t chunkSize,
                                               const std::string& algorithm);

    /**
     * @brief 下载远程文件的哈希列表
     */
    std::vector<ChunkInfo> downloadRemoteHashList(const std::string& url,
                                                  uint64_t chunkSize,
                                                  const std::string& algorithm);

    /**
     * @brief 比较哈希列表
     */
    std::vector<ChunkInfo> compareHashLists(
        const std::vector<ChunkInfo>& local,
        const std::vector<ChunkInfo>& remote);

    /**
     * @brief 下载指定范围的数据
     */
    std::vector<uint8_t> downloadRange(const std::string& url,
                                      uint64_t offset,
                                      uint64_t size);

    /**
     * @brief 合并文件
     */
    bool mergeFile(const std::string& localPath,
                  const std::vector<std::vector<uint8_t>>& changedChunks,
                  const std::vector<ChunkInfo>& chunkInfo);
};

} // namespace falcon
