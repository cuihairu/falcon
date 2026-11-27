/**
 * @file incremental_download_core.cpp
 * @brief 增量下载功能实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include <falcon/protocols/incremental_download.hpp>
#include <falcon/detail/injection.hpp>
#include <falcon/logger.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unordered_map>

#ifdef FALCON_USE_CURL
#include <curl/curl.h>
#endif

#if defined(FALCON_USE_OPENSSL) || defined(FALCON_HAS_OPENSSL)
#include <openssl/sha.h>
#include <openssl/evp.h>
#endif

namespace falcon {

namespace {

#ifdef FALCON_USE_CURL
std::size_t curl_write_string_cb(char* ptr, std::size_t size, std::size_t nmemb,
                                 void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}
#endif

/// 十六进制串校验（偶数长度 + 全部为十六进制字符）
bool is_hex_string(const std::string& s) {
    if (s.empty() || (s.size() % 2) != 0) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

/// 去除行尾 \r 与首尾空白
std::string trim_line(const std::string& line) {
    std::size_t begin = 0;
    std::size_t end = line.size();
    while (end > begin && (line[end - 1] == '\r' || line[end - 1] == ' ' ||
                           line[end - 1] == '\t')) {
        --end;
    }
    while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) {
        ++begin;
    }
    return line.substr(begin, end - begin);
}

/// ASCII 小写化
std::string to_lower_copy(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// 解析元数据行 "# key: value"（key 小写化；'#' 前缀与空白被剥离）
bool parse_metadata_line(const std::string& line, std::string& key, std::string& value) {
    std::size_t begin = 0;
    if (begin < line.size() && line[begin] == '#') {
        ++begin;
    }
    while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) {
        ++begin;
    }

    const auto colon = line.find(':', begin);
    if (colon == std::string::npos) {
        return false;
    }
    key = to_lower_copy(line.substr(begin, colon - begin));
    value = trim_line(line.substr(colon + 1));
    return !key.empty();
}

} // namespace

bool IncrementalDownloader::applyPatch(const std::string& localPath,
                                      const std::string& patchData,
                                      const FileDiff& diff) {
    // 应用补丁数据
    FALCON_LOG_INFO("Applying patch to {}", localPath);

    // 补丁格式解析待实现；当前仅回写本地内容
    (void)patchData;

    // 读取本地文件
    std::vector<uint8_t> fileData(diff.remoteSize);

    std::ifstream inFile(localPath, std::ios::binary);
    if (inFile.is_open()) {
        inFile.read(reinterpret_cast<char*>(fileData.data()),
                    static_cast<std::streamsize>(diff.localSize));
        inFile.close();
    }

    // 解析补丁数据并应用
    // 这里需要根据补丁格式进行解析

    // 写回文件
    std::ofstream outFile(localPath, std::ios::binary);
    if (!outFile.is_open()) {
        FALCON_LOG_ERROR("Failed to open file for writing: {}", localPath);
        return false;
    }

    outFile.write(reinterpret_cast<const char*>(fileData.data()),
                  static_cast<std::streamsize>(fileData.size()));
    outFile.close();

    FALCON_LOG_INFO("Patch applied successfully");
    return true;
}

std::vector<ChunkInfo> IncrementalDownloader::generateHashList(
    const std::string& filePath,
    uint64_t chunkSize) {

    return calculateChunkHashes(filePath, chunkSize, "sha256");
}

std::string IncrementalDownloader::calculateFileHash(const std::string& filePath,
                                                    const std::string& algorithm) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        FALCON_LOG_ERROR("Failed to open file for hashing: {}", filePath);
        return "";
    }

    file.seekg(0, std::ios::end);
    const auto end_pos = file.tellg();
    const uint64_t fileSize = end_pos >= 0 ? static_cast<uint64_t>(end_pos) : 0;
    file.seekg(0, std::ios::beg);

    std::string fileData(fileSize, '\0');
    if (fileSize > 0) {
        file.read(&fileData[0], static_cast<std::streamsize>(fileSize));
    }
    file.close();

    return calculateHash(fileData, algorithm);
}

bool IncrementalDownloader::verifyFile(const std::string& filePath,
                                      const std::string& expectedHash) {
    const std::string actualHash = calculateFileHash(filePath, "sha256");
    if (actualHash.empty()) {
        // 文件不可读或哈希计算失败（含注入）→ 无法判定为有效
        return false;
    }

    const bool valid = (actualHash == expectedHash);
    if (!valid) {
        FALCON_LOG_ERROR("File verification failed: expected {}, got {}",
                        expectedHash, actualHash);
    } else {
        FALCON_LOG_INFO("File verification passed: {}", filePath);
    }

    return valid;
}

// ============================================================================
// rsync 滚动哈希（v2 块列表：生成 / 序列化 / 解析 / 计划 / 端到端比较）
// ============================================================================

bool IncrementalDownloader::generateRsyncBlockList(const std::string& filePath,
                                                   uint64_t blockSize,
                                                   RsyncBlockList& out) {
    // 块尺寸门禁：0 无意义；过大块削弱滚动匹配粒度且弱校验和精度下降
    if (blockSize == 0 || blockSize > 16ull * 1024 * 1024) {
        FALCON_LOG_ERROR("Invalid rsync block size: {}", blockSize);
        return false;
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        FALCON_LOG_ERROR("Failed to open file for rsync hashing: {}", filePath);
        return false;
    }

    file.seekg(0, std::ios::end);
    const auto end_pos = file.tellg();
    const uint64_t fileSize = end_pos >= 0 ? static_cast<uint64_t>(end_pos) : 0;
    file.seekg(0, std::ios::beg);

    out = RsyncBlockList{};
    out.blockSize = blockSize;
    out.fileSize = fileSize;

    // v2 格式整文件哈希必填（downloadChanged() 端到端校验依赖它）
    out.fileHash = calculateFileHash(filePath, "sha256");
    if (out.fileHash.empty()) {
        FALCON_LOG_ERROR("Failed to calculate file hash for rsync: {}", filePath);
        return false;
    }

    if (fileSize == 0) {
        // 空文件无整块（尾部不足 blockSize 的部分不进块列表）
        return true;
    }

    std::vector<RsyncBlock> blocks;
    blocks.reserve(static_cast<size_t>(fileSize / blockSize + 1));

    std::vector<uint8_t> buffer(static_cast<size_t>(blockSize));
    uint64_t offset = 0;
    while (offset + blockSize <= fileSize) {
        file.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(blockSize));
        if (static_cast<uint64_t>(file.gcount()) < blockSize) {
            FALCON_LOG_ERROR("Short read during rsync block generation at {}",
                            offset);
            return false;
        }

        RollingChecksum rc;
        rc.init(buffer.data(), static_cast<uint32_t>(blockSize));

        const std::string strong = calculateHash(
            std::string(reinterpret_cast<const char*>(buffer.data()),
                        static_cast<size_t>(blockSize)),
            "sha256");
        if (strong.empty()) {
            FALCON_LOG_ERROR("Failed to calculate block hash at offset {}", offset);
            return false;
        }

        RsyncBlock block;
        block.offset = offset;
        block.weak = rc.value();
        block.strong = strong;
        blocks.push_back(std::move(block));

        offset += blockSize;
    }

    out.blocks = std::move(blocks);
    return true;
}

std::string IncrementalDownloader::serializeRsyncBlockList(
    const RsyncBlockList& list) {
    std::ostringstream ss;
    ss << "# falcon-hash-list v2\n";
    ss << "# mode: rsync\n";
    ss << "# blockSize: " << list.blockSize << "\n";
    ss << "# algorithm: sha256\n";
    ss << "# fileSize: " << list.fileSize << "\n";
    ss << "# fileHash: " << list.fileHash << "\n";
    ss << "# blocks: " << list.blocks.size() << "\n";
    for (const auto& block : list.blocks) {
        ss << block.offset << ":";
        ss << std::hex << std::setfill('0') << std::setw(8) << block.weak;
        ss << std::dec << ":" << block.strong << "\n";
    }
    return ss.str();
}

bool IncrementalDownloader::parseRsyncBlockList(const std::string& text,
                                                RsyncBlockList& out) {
    out = RsyncBlockList{};

    bool saw_magic = false;
    bool saw_mode = false;
    bool saw_block_size = false;
    bool saw_algorithm = false;
    bool saw_file_size = false;
    bool saw_file_hash = false;
    bool saw_blocks_count = false;

    uint64_t block_size = 0;
    uint64_t file_size = 0;
    uint64_t expected_blocks = 0;
    std::string file_hash;
    std::vector<RsyncBlock> blocks;

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string trimmed = trim_line(line);
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed[0] == '#') {
            // 版本魔数行无冒号，parse_metadata_line 解析不了——直接串比较；
            // 其余注释行（无冒号）跳过
            if (trimmed == "# falcon-hash-list v2") {
                saw_magic = true;
                continue;
            }
            std::string key;
            std::string value;
            if (!parse_metadata_line(trimmed, key, value)) {
                continue;  // 注释行
            }

            try {
                if (key == "falcon-hash-list") {
                    // 版本魔数：只接受 v2，v1 分块哈希列表在此被拒绝
                    if (value != "v2") {
                        return false;
                    }
                    saw_magic = true;
                } else if (key == "mode") {
                    if (value != "rsync") {
                        return false;
                    }
                    saw_mode = true;
                } else if (key == "blocksize") {
                    block_size = std::stoull(value);
                    saw_block_size = true;
                } else if (key == "algorithm") {
                    if (to_lower_copy(value) != "sha256") {
                        return false;
                    }
                    saw_algorithm = true;
                } else if (key == "filesize") {
                    file_size = std::stoull(value);
                    saw_file_size = true;
                } else if (key == "filehash") {
                    if (!is_hex_string(value)) {
                        return false;
                    }
                    file_hash = to_lower_copy(value);
                    saw_file_hash = true;
                } else if (key == "blocks") {
                    expected_blocks = std::stoull(value);
                    saw_blocks_count = true;
                }
            } catch (const std::exception&) {
                return false;  // 元数据数值非法 → 列表无效
            }
            continue;
        }

        // 数据行必须出现在全部必需元数据之后（元数据齐备性前置校验）
        if (!saw_magic || !saw_mode || !saw_block_size || !saw_algorithm ||
            !saw_file_size || !saw_file_hash || !saw_blocks_count) {
            return false;
        }

        // 行格式 <offset 十进制>:<weak 8 位 hex>:<strong hex>
        const auto c1 = trimmed.find(':');
        const auto c2 = (c1 == std::string::npos) ? std::string::npos
                                                  : trimmed.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) {
            return false;
        }
        const std::string offset_str = trimmed.substr(0, c1);
        const std::string weak_str = trimmed.substr(c1 + 1, c2 - c1 - 1);
        const std::string strong_str = trimmed.substr(c2 + 1);

        if (offset_str.empty() ||
            !std::all_of(offset_str.begin(), offset_str.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return false;
        }
        if (weak_str.size() != 8 || !is_hex_string(weak_str)) {
            return false;
        }
        if (!is_hex_string(strong_str)) {
            return false;
        }

        try {
            RsyncBlock block;
            block.offset = std::stoull(offset_str);
            block.weak = static_cast<uint32_t>(std::stoul(weak_str, nullptr, 16));
            block.strong = strong_str;
            blocks.push_back(std::move(block));
        } catch (const std::exception&) {
            return false;
        }
    }

    // 必需元数据齐备
    if (!saw_magic || !saw_mode || !saw_block_size || !saw_algorithm ||
        !saw_file_size || !saw_file_hash || !saw_blocks_count) {
        return false;
    }

    if (block_size == 0 || block_size > 16ull * 1024 * 1024) {
        return false;
    }
    if (blocks.size() != expected_blocks) {
        return false;
    }

    // 块按偏移从 0 起连续升序，且不越过文件尾（尾巴不进块列表）
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].offset != static_cast<uint64_t>(i) * block_size) {
            return false;
        }
        if (blocks[i].offset + block_size > file_size) {
            return false;
        }
    }

    out.blockSize = block_size;
    out.fileSize = file_size;
    out.fileHash = file_hash;
    out.blocks = std::move(blocks);
    return true;
}

RsyncPlan IncrementalDownloader::planRsync(const std::string& localPath,
                                           const RsyncBlockList& remote) {
    // 全量下载兜底：单个 Download 段覆盖整个远端文件
    const auto full_download = [&remote]() {
        RsyncPlan plan;
        plan.segments.push_back(RsyncSegment{
            RsyncSegment::Kind::Download, 0, 0, 0, remote.fileSize});
        plan.downloadBytes = remote.fileSize;
        return plan;
    };

    if (remote.blockSize == 0 || remote.blockSize > 16ull * 1024 * 1024 ||
        remote.blocks.empty() || remote.fileSize == 0) {
        return full_download();
    }

    std::ifstream inFile(localPath, std::ios::binary);
    if (!inFile.is_open()) {
        // 本地不可读：无源数据可用，退化为全量下载
        return full_download();
    }
    inFile.seekg(0, std::ios::end);
    const auto end_pos = inFile.tellg();
    const uint64_t localSize = end_pos >= 0 ? static_cast<uint64_t>(end_pos) : 0;
    inFile.seekg(0, std::ios::beg);

    std::vector<uint8_t> local(static_cast<size_t>(localSize));
    if (localSize > 0) {
        inFile.read(reinterpret_cast<char*>(local.data()),
                    static_cast<std::streamsize>(localSize));
        if (static_cast<uint64_t>(inFile.gcount()) < localSize) {
            FALCON_LOG_ERROR("Short read reading local file for rsync plan: {}",
                            localPath);
            return full_download();
        }
    }
    inFile.close();

    const uint32_t block_size = static_cast<uint32_t>(remote.blockSize);

    // 弱校验和 → 候选块索引表（同一弱值可能对应多个块）
    std::unordered_map<uint32_t, std::vector<uint32_t>> weak_table;
    for (std::size_t i = 0; i < remote.blocks.size(); ++i) {
        weak_table[remote.blocks[i].weak].push_back(
            static_cast<uint32_t>(i));
    }

    std::vector<bool> matched(remote.blocks.size(), false);
    std::vector<uint64_t> matched_local_offset(remote.blocks.size(), 0);

    // 1 字节滑动扫描：弱校验和缩小候选，强哈希逐个确认；命中后窗口整体
    // 跳过一个块长。扫描只覆盖本地整块区域（不足一窗的尾部没有远端签名
    // 可对，无法作为拷贝源）。
    RollingChecksum rc;
    bool window_valid = false;
    uint64_t pos = 0;
    while (pos + remote.blockSize <= localSize) {
        if (!window_valid) {
            rc.init(local.data() + pos, block_size);
            window_valid = true;
        }

        bool hit = false;
        const auto it = weak_table.find(rc.value());
        if (it != weak_table.end()) {
            for (const uint32_t idx : it->second) {
                if (matched[idx]) {
                    continue;
                }
                const std::string strong = calculateHash(
                    std::string(reinterpret_cast<const char*>(local.data() + pos),
                                static_cast<size_t>(remote.blockSize)),
                    "sha256");
                if (strong == remote.blocks[idx].strong) {
                    matched[idx] = true;
                    matched_local_offset[idx] = pos;
                    hit = true;
                    break;
                }
            }
        }

        if (hit) {
            pos += remote.blockSize;
            window_valid = false;
        } else if (pos + remote.blockSize < localSize) {
            rc.roll(local[pos], local[pos + remote.blockSize]);
            ++pos;
        } else {
            // 窗口已到文件末尾仍无匹配：无字节可再滑入，扫描结束
            break;
        }
    }

    // 段构建：按远端块序（== 成品位置升序）合并连续匹配/未匹配段。
    // 匹配段还要求本地源偏移同步递增（本地连续 → 合并为一次搬运），
    // 源偏移断开的匹配各自成段（downloadChanged 逐段独立搬运，别名安全）。
    RsyncPlan plan;
    const uint64_t full_bytes =
        static_cast<uint64_t>(remote.blocks.size()) * remote.blockSize;

    std::size_t idx = 0;
    while (idx < remote.blocks.size()) {
        if (matched[idx]) {
            const uint64_t run_start = remote.blocks[idx].offset;
            uint64_t expected_local = matched_local_offset[idx];
            std::size_t j = idx;
            while (j < remote.blocks.size() && matched[j] &&
                   matched_local_offset[j] == expected_local) {
                expected_local += remote.blockSize;
                ++j;
            }
            const uint64_t length = (static_cast<uint64_t>(j - idx)) *
                                    remote.blockSize;
            plan.segments.push_back(RsyncSegment{
                RsyncSegment::Kind::LocalCopy, run_start, matched_local_offset[idx],
                0, length});
            plan.copyBytes += length;
            idx = j;
        } else {
            const uint64_t run_start = remote.blocks[idx].offset;
            std::size_t j = idx;
            while (j < remote.blocks.size() && !matched[j]) {
                ++j;
            }
            const uint64_t length = (static_cast<uint64_t>(j - idx)) *
                                    remote.blockSize;
            plan.segments.push_back(RsyncSegment{
                RsyncSegment::Kind::Download, run_start, 0, run_start, length});
            plan.downloadBytes += length;
            idx = j;
        }
    }

    // 远程尾部（不足 blockSize）不在块列表中，恒为下载段
    if (full_bytes < remote.fileSize) {
        plan.segments.push_back(RsyncSegment{
            RsyncSegment::Kind::Download, full_bytes, 0, full_bytes,
            remote.fileSize - full_bytes});
        plan.downloadBytes += remote.fileSize - full_bytes;
    }

    return plan;
}

bool IncrementalDownloader::compareRsync(const std::string& localPath,
                                         const std::string& remoteUrl,
                                         const Options& options,
                                         FileDiff& diff) {
    // v2 块列表沿用 <url> + ".falconhash" 约定
    std::string text;
    if (!http_get(remoteUrl + ".falconhash", text)) {
        FALCON_LOG_WARN("Failed to download rsync block list: {}", remoteUrl);
        return false;
    }

    RsyncBlockList remote;
    if (!parseRsyncBlockList(text, remote)) {
        FALCON_LOG_WARN("Invalid rsync block list: {}", remoteUrl);
        return false;
    }

    const RsyncPlan plan = planRsync(localPath, remote);

    std::error_code ec;
    const uint64_t localSize = std::filesystem::file_size(localPath, ec);

    FileDiff result;
    result.localPath = localPath;
    result.remotePath = remoteUrl;
    result.localSize = ec ? 0 : localSize;
    result.remoteSize = remote.fileSize;
    result.hashAlgorithm = "sha256";
    result.fileHash = remote.fileHash;
    result.verifyDownload = options.verifyDownload;
    result.totalChanged = plan.downloadBytes;
    result.ratio =
        remote.fileSize > 0
            ? static_cast<double>(plan.downloadBytes) /
                  static_cast<double>(remote.fileSize)
            : 0.0;

    for (const auto& seg : plan.segments) {
        ChunkInfo info;
        info.offset = seg.outputOffset;
        info.size = seg.length;
        info.changed = (seg.kind == RsyncSegment::Kind::Download);
        // 未变化段带显式本地源偏移（rsync 错位搬运；v1 分块布局下本地
        // 位置与输出位置相同，无需设置）
        if (seg.kind == RsyncSegment::Kind::LocalCopy) {
            info.localOffset = seg.localOffset;
        }
        result.chunks.push_back(std::move(info));
    }

    FALCON_LOG_INFO("Rsync comparison complete: {} local, {} remote, "
                    "{} changed ({}%)",
                    result.localSize, result.remoteSize, result.totalChanged,
                    static_cast<uint64_t>(result.ratio * 100));

    diff = std::move(result);
    return true;
}


} // namespace falcon
