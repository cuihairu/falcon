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
#include <cstdint>

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
 * @brief 哈希列表文本格式（.falconhash 文件）：
 *
 *   # falcon-hash-list v1
 *   # chunkSize: 1048576
 *   # algorithm: sha256
 *   # fileSize: 12345678
 *   # chunks: 12
 *   <hex hash line 1>
 *   <hex hash line 2>
 *   ...
 *
 * 以 '#' 开头的行是元数据；其余非空行为按分块顺序排列的十六进制哈希。
 * compare() 使用的哈希列表 URL 约定为 <file url> + ".falconhash"。
 */

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
     *
     * 远程哈希列表从 <remoteUrl> + ".falconhash" 获取；获取失败或列表
     * 无效时回退为全量下载建议（diff.chunks 为空）。
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

    /**
     * @brief 将哈希列表序列化为文本格式
     *
     * 输出可直接部署为服务端的 .falconhash 文件，供
     * downloadRemoteHashList 消费。
     *
     * @param chunks 分块哈希（来自 generateHashList）
     * @param chunkSize 分块大小
     * @param algorithm 哈希算法名
     * @param fileSize 文件总大小（用于计算最后一块的实际大小）
     * @return 文本内容
     */
    static std::string serializeHashList(const std::vector<ChunkInfo>& chunks,
                                         uint64_t chunkSize,
                                         const std::string& algorithm,
                                         uint64_t fileSize);

    /**
     * @brief 解析哈希列表文本
     *
     * 元数据行可覆盖默认参数；哈希行必须是偶数长度的十六进制串。
     * 校验失败（无哈希行 / chunks 计数不符 / 哈希格式非法 /
     * 算法与 defaultAlgorithm 不一致）返回空列表。
     *
     * @param text 哈希列表文本
     * @param defaultChunkSize 元数据缺失时的分块大小
     * @param defaultAlgorithm 期望的哈希算法（不一致视为列表无效）
     * @return 分块信息列表（changed 均为 false）
     */
    static std::vector<ChunkInfo> parseHashList(const std::string& text,
                                                uint64_t defaultChunkSize,
                                                const std::string& defaultAlgorithm);

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
     *
     * 实际请求 <url> + ".falconhash"；解析结果与传入的 chunkSize /
     * algorithm 约束不匹配时返回空。
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
     * @brief 下载指定范围的数据（HTTP Range 请求）
     */
    std::vector<uint8_t> downloadRange(const std::string& url,
                                      uint64_t offset,
                                      uint64_t size);

    /**
     * @brief 阻塞式 HTTP GET（libcurl 可用时）
     * @return true 成功且 out 非空
     */
    bool http_get(const std::string& url, std::string& out);

};

} // namespace falcon
