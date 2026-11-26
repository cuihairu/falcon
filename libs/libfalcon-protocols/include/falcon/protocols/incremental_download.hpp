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
    /// 未变化分块「本地源偏移与输出位置相同」的标记（v1 分块布局）
    static constexpr uint64_t kLocalOffsetAuto = UINT64_MAX;

    uint64_t offset;        // 偏移量（输出位置；变化分块同时是远程抓取起点）
    uint64_t size;          // 大小
    std::string hash;       // 哈希值
    bool changed;           // 是否变化
    /// 未变化分块的本地源偏移。kLocalOffsetAuto 表示与 offset 一致（v1 分块
    /// 布局下本地位置与输出位置相同）；rsync 计划中未变化段的本地源与输出
    /// 位置可以不同（远端插入/删除造成错位），必须显式给出
    uint64_t localOffset = kLocalOffsetAuto;
};

/**
 * @struct RsyncBlock
 * @brief rsync 块签名（滚动弱校验和 + 强哈希）
 */
struct RsyncBlock {
    uint64_t offset = 0;    // 块在远程文件中的偏移
    uint32_t weak = 0;      // Adler 风格滚动弱校验和
    std::string strong;     // 强哈希（sha256，小写 hex）
};

/**
 * @struct RsyncBlockList
 * @brief 远程文件的 rsync 块列表（v2 哈希列表）
 */
struct RsyncBlockList {
    uint64_t blockSize = 0;
    uint64_t fileSize = 0;
    std::string fileHash;               // 整文件哈希（v2 必填）
    std::vector<RsyncBlock> blocks;     // 整块（按偏移升序）
};

/**
 * @struct RsyncSegment
 * @brief rsync 重建计划段（按成品位置升序且完整铺满成品）
 */
struct RsyncSegment {
    enum class Kind { LocalCopy, Download };

    Kind kind;                   // LocalCopy = 从本地文件拷贝；Download = 远程 Range
    uint64_t outputOffset = 0;   // 段在成品中的位置
    uint64_t localOffset = 0;    // LocalCopy：本地源偏移；Download 未用（0）
    uint64_t remoteOffset = 0;   // Download：远程抓取偏移（== outputOffset）
    uint64_t length = 0;
};

/**
 * @struct RsyncPlan
 * @brief rsync 重建计划
 */
struct RsyncPlan {
    std::vector<RsyncSegment> segments;
    uint64_t downloadBytes = 0;   // Download 段总字节（增量传输量）
    uint64_t copyBytes = 0;       // LocalCopy 段总字节
};

/**
 * @class RollingChecksum
 * @brief Adler 风格滚动弱校验和（rsync 块匹配第一关）
 *
 * a = 窗口内字节和，b = 逐字节前缀和之和；窗口右滑一格 O(1)：
 * a' = a - 出界字节 + 新进字节，b' = b - 窗口长度 × 出界字节 + a'。
 * 校验和取 a/b 低 16 位拼接。碰撞由下游强哈希逐个确认，这里只负责
 * 把候选集缩小到几乎为一。
 */
class RollingChecksum {
public:
    RollingChecksum() = default;

    /// 用 len 字节窗口重算（此后窗口大小固定为 len）
    void init(const uint8_t* data, uint32_t len);

    /// 窗口向右滑一格：移除最旧字节 out，纳入新字节 in
    void roll(uint8_t out, uint8_t in);

    uint32_t value() const;

private:
    uint32_t a_ = 0;
    uint32_t b_ = 0;
    uint32_t window_ = 0;
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
    std::string fileHash;            // 远程整文件哈希（哈希列表提供时）
    std::string hashAlgorithm;       // 哈希算法名
    bool verifyDownload = true;      // 下载完成后整文件校验
};

/**
 * @brief 哈希列表文本格式（.falconhash 文件）：
 *
 *   # falcon-hash-list v1
 *   # chunkSize: 1048576
 *   # algorithm: sha256
 *   # fileSize: 12345678
 *   # chunks: 12
 *   # fileHash: <整文件 hex 哈希（可选行）>
 *   <hex hash line 1>
 *   <hex hash line 2>
 *   ...
 *
 * 以 '#' 开头的行是元数据；其余非空行为按分块顺序排列的十六进制哈希。
 * fileHash 提供时 downloadChanged() 在写出成品后做整文件校验，不匹配
 * 删除输出并报告失败（verifyDownload 选项可关闭该校验）。
 * compare() 使用的哈希列表 URL 约定为 <file url> + ".falconhash"。
 *
 * v2（rsync 块列表，DiffAlgorithm::Rsync）：
 *
 *   # falcon-hash-list v2
 *   # mode: rsync
 *   # blockSize: 2048
 *   # algorithm: sha256
 *   # fileSize: 12345
 *   # fileHash: <整文件 hex 哈希（v2 必填）>
 *   # blocks: 6
 *   <offset>:<weak 8 位 hex>:<strong hex>
 *   ...
 *
 * v2 只发布整块签名（末尾不足 blockSize 的尾巴不进列表，客户端按
 * 下载段处理）；fileHash 必填，供 downloadChanged() 做成品校验。
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
     * @brief 完整哈希列表（分块 + 可选整文件哈希）
     */
    struct HashListData {
        std::vector<ChunkInfo> chunks;  // 分块信息
        std::string fileHash;           // 整文件哈希（列表未提供时为空）
    };

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
     * @param fileHash 整文件哈希（可选，非空时写出 fileHash 元数据行）
     * @return 文本内容
     */
    static std::string serializeHashList(const std::vector<ChunkInfo>& chunks,
                                         uint64_t chunkSize,
                                         const std::string& algorithm,
                                         uint64_t fileSize,
                                         const std::string& fileHash = std::string());

    /**
     * @brief 解析哈希列表文本（扩展版）
     *
     * 元数据行可覆盖默认参数；哈希行必须是偶数长度的十六进制串；
     * fileHash 行存在时必须是合法十六进制（否则整个列表无效）。
     * 校验失败（无哈希行 / chunks 计数不符 / 哈希格式非法 /
     * 算法与 defaultAlgorithm 不一致）返回 false。
     *
     * @param text 哈希列表文本
     * @param defaultChunkSize 元数据缺失时的分块大小
     * @param defaultAlgorithm 期望的哈希算法（不一致视为列表无效）
     * @param out 解析结果（成功时写入）
     * @return 解析是否成功
     */
    static bool parseHashListEx(const std::string& text,
                                uint64_t defaultChunkSize,
                                const std::string& defaultAlgorithm,
                                HashListData& out);

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

    /**
     * @brief 计算文件整文件哈希
     *
     * @param filePath 文件路径
     * @param algorithm 哈希算法名
     * @return 十六进制哈希；文件打开失败返回空串
     */
    std::string calculateFileHash(const std::string& filePath,
                                  const std::string& algorithm = "sha256");

    /**
     * @brief 生成文件的 rsync 块列表
     *
     * 按 blockSize 顺序切分：每个整块计算滚动弱校验和与 sha256 强哈希，
     * 末尾不足 blockSize 的尾巴不进块列表（客户端按下载段处理）。
     * blockSize 越界（0 或 >16MB）或哈希计算失败返回 false。
     */
    bool generateRsyncBlockList(const std::string& filePath,
                                uint64_t blockSize,
                                RsyncBlockList& out);

    /**
     * @brief 序列化 rsync 块列表（v2 文本格式，见文件头格式说明）
     */
    static std::string serializeRsyncBlockList(const RsyncBlockList& list);

    /**
     * @brief 解析 rsync 块列表文本（v2 格式）
     *
     * 要求 v2 魔数与 mode: rsync 声明、algorithm=sha256、合法 fileHash
     * （必填）、blocks 计数一致、每条 `offset:weak:strong` 格式合法。
     * 任何非法内容返回 false（v1 哈希列表会被拒绝）。
     */
    static bool parseRsyncBlockList(const std::string& text,
                                    RsyncBlockList& out);

    /**
     * @brief 生成本地文件对照远程块列表的重建计划
     *
     * 弱校验和哈希表定位候选块，强哈希逐个确认；未匹配窗口按 1 字节
     * 滑动重试（吸收远端插入/删除造成的错位）。段按成品位置升序且完整
     * 覆盖 [0, remote.fileSize)。本地文件不可读时退化为单个全量下载段。
     */
    RsyncPlan planRsync(const std::string& localPath,
                        const RsyncBlockList& remote);

    /**
     * @brief rsync 端到端比较（拉取 v2 块列表 + 生成重建计划）
     *
     * 成功时 diff.chunks 由重建计划映射而成（未变化段带显式 localOffset，
     * 变化段为远程 Range），diff.fileHash 为 v2 列表声明的整文件哈希，
     * 结果可直接交给 downloadChanged()。任何环节失败返回 false（diff 不
     * 写入），调用方应回退到其他比较方式。
     */
    bool compareRsync(const std::string& localPath,
                      const std::string& remoteUrl,
                      const Options& options,
                      FileDiff& diff);

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
    HashListData downloadRemoteHashList(const std::string& url,
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
