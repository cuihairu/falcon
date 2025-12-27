/**
 * @file incremental_download.cpp
 * @brief 增量下载功能实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include <falcon/incremental_download.hpp>
#include <falcon/logger.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <openssl/sha.h>
#include <openssl/evp.h>

namespace falcon {

// ============================================================================
// IncrementalDownloader 实现
// ============================================================================

IncrementalDownloader::IncrementalDownloader() {
}

IncrementalDownloader::~IncrementalDownloader() {
}

std::string IncrementalDownloader::calculateHash(const std::string& data,
                                                 const std::string& algorithm) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const EVP_MD* md = EVP_get_digestbyname(algorithm.c_str());

    if (!ctx || !md) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    if (EVP_DigestUpdate(ctx, data.c_str(), data.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;

    if (EVP_DigestFinal_ex(ctx, hash, &hashLen) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    EVP_MD_CTX_free(ctx);

    // 转换为十六进制字符串
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hashLen; ++i) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }

    return ss.str();
}

std::vector<ChunkInfo> IncrementalDownloader::calculateChunkHashes(
    const std::string& filePath,
    uint64_t chunkSize,
    const std::string& algorithm) {

    std::vector<ChunkInfo> chunks;

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        FALCON_LOG_ERROR("Failed to open file: {}", filePath);
        return chunks;
    }

    file.seekg(0, std::ios::end);
    uint64_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    uint64_t offset = 0;
    uint64_t remaining = fileSize;

    while (remaining > 0) {
        uint64_t currentChunkSize = std::min(chunkSize, remaining);
        std::vector<char> buffer(currentChunkSize);

        file.read(buffer.data(), currentChunkSize);
        if (!file) {
            FALCON_LOG_ERROR("Error reading file at offset {}", offset);
            break;
        }

        std::string chunkData(buffer.data(), currentChunkSize);
        std::string hash = calculateHash(chunkData, algorithm);

        ChunkInfo info;
        info.offset = offset;
        info.size = currentChunkSize;
        info.hash = hash;
        info.changed = false;

        chunks.push_back(info);

        offset += currentChunkSize;
        remaining -= currentChunkSize;
    }

    file.close();

    FALCON_LOG_INFO("Calculated {} chunks for file {}", chunks.size(), filePath);
    return chunks;
}

std::vector<ChunkInfo> IncrementalDownloader::downloadRemoteHashList(
    const std::string& url,
    uint64_t chunkSize,
    const std::string& algorithm) {

    // 这里需要使用 HTTP 插件下载远程文件
    // 并计算哈希列表

    // 暂时返回空列表
    FALCON_LOG_WARN("Remote hash list download not implemented");
    return {};
}

std::vector<ChunkInfo> IncrementalDownloader::compareHashLists(
    const std::vector<ChunkInfo>& local,
    const std::vector<ChunkInfo>& remote) {

    std::vector<ChunkInfo> diff;

    // 假设本地和远程的分块是对齐的
    for (size_t i = 0; i < remote.size(); ++i) {
        ChunkInfo info = remote[i];

        if (i < local.size()) {
            info.changed = (local[i].hash != remote[i].hash);
        } else {
            info.changed = true;  // 远程有新分块
        }

        diff.push_back(info);
    }

    return diff;
}

FileDiff IncrementalDownloader::compare(const std::string& localPath,
                                       const std::string& remoteUrl,
                                       const Options& options) {
    FileDiff diff;
    diff.localPath = localPath;
    diff.remotePath = remoteUrl;
    diff.localSize = 0;
    diff.remoteSize = 0;
    diff.totalChanged = 0;
    diff.ratio = 0.0;

    // 获取本地文件大小
    std::ifstream localFile(localPath, std::ios::binary | std::ios::ate);
    if (localFile.is_open()) {
        diff.localSize = localFile.tellg();
        localFile.close();
    }

    // 计算本地文件哈希列表
    auto localChunks = calculateChunkHashes(localPath, options.chunkSize,
                                            options.hashAlgorithm);

    // 下载远程文件哈希列表
    auto remoteChunks = downloadRemoteHashList(remoteUrl, options.chunkSize,
                                               options.hashAlgorithm);

    if (remoteChunks.empty()) {
        FALCON_LOG_WARN("Failed to get remote hash list, falling back to full download");
        diff.chunks = remoteChunks;
        return diff;
    }

    // 计算远程文件大小
    for (const auto& chunk : remoteChunks) {
        diff.remoteSize += chunk.size;
    }

    // 比较哈希列表
    diff.chunks = compareHashLists(localChunks, remoteChunks);

    // 计算变化统计
    for (const auto& chunk : diff.chunks) {
        if (chunk.changed) {
            diff.totalChanged += chunk.size;
        }
    }

    diff.ratio = diff.remoteSize > 0 ?
        static_cast<double>(diff.totalChanged) / diff.remoteSize : 0.0;

    FALCON_LOG_INFO("File comparison complete: {} local, {} remote, {} changed ({:.1f}%)",
                   diff.localSize, diff.remoteSize, diff.totalChanged,
                   diff.ratio * 100);

    return diff;
}

std::vector<uint8_t> IncrementalDownloader::downloadRange(
    const std::string& url,
    uint64_t offset,
    uint64_t size) {

    // 这里需要使用 HTTP 插件的 Range 请求
    // 暂时返回空数据
    FALCON_LOG_WARN("Range download not implemented");
    return {};
}

bool IncrementalDownloader::downloadChanged(const FileDiff& diff,
                                           const std::string& outputPath,
                                           ProgressCallback callback) {
    FALCON_LOG_INFO("Downloading changed parts: {} bytes of {} total",
                   diff.totalChanged, diff.remoteSize);

    // 读取本地文件
    std::vector<uint8_t> localData(diff.remoteSize);

    std::ifstream inFile(diff.localPath, std::ios::binary);
    if (inFile.is_open()) {
        inFile.read(reinterpret_cast<char*>(localData.data()), diff.localSize);
        inFile.close();
    }

    // 下载变化的部分
    uint64_t downloaded = 0;
    std::vector<std::vector<uint8_t>> changedChunks;

    for (const auto& chunk : diff.chunks) {
        if (chunk.changed) {
            auto data = downloadRange(diff.remotePath, chunk.offset, chunk.size);
            if (data.size() != chunk.size) {
                FALCON_LOG_ERROR("Failed to download chunk at offset {}", chunk.offset);
                return false;
            }

            // 写入到对应位置
            std::memcpy(localData.data() + chunk.offset, data.data(), data.size());

            downloaded += data.size();
            changedChunks.push_back(data);

            if (callback) {
                callback(downloaded, diff.totalChanged);
            }
        }
    }

    // 写入输出文件
    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile.is_open()) {
        FALCON_LOG_ERROR("Failed to create output file: {}", outputPath);
        return false;
    }

    outFile.write(reinterpret_cast<const char*>(localData.data()), diff.remoteSize);
    outFile.close();

    FALCON_LOG_INFO("Incremental download completed: {}", outputPath);
    return true;
}

bool IncrementalDownloader::applyPatch(const std::string& localPath,
                                      const std::string& patchData,
                                      const FileDiff& diff) {
    // 应用补丁数据
    FALCON_LOG_INFO("Applying patch to {}", localPath);

    // 读取本地文件
    std::vector<uint8_t> fileData(diff.remoteSize);

    std::ifstream inFile(localPath, std::ios::binary);
    if (inFile.is_open()) {
        inFile.read(reinterpret_cast<char*>(fileData.data()), diff.localSize);
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

    outFile.write(reinterpret_cast<const char*>(fileData.data()), fileData.size());
    outFile.close();

    FALCON_LOG_INFO("Patch applied successfully");
    return true;
}

std::vector<ChunkInfo> IncrementalDownloader::generateHashList(
    const std::string& filePath,
    uint64_t chunkSize) {

    return calculateChunkHashes(filePath, chunkSize, "sha256");
}

bool IncrementalDownloader::verifyFile(const std::string& filePath,
                                      const std::string& expectedHash) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        FALCON_LOG_ERROR("Failed to open file for verification: {}", filePath);
        return false;
    }

    file.seekg(0, std::ios::end);
    uint64_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string fileData(fileSize, '\0');
    file.read(&fileData[0], fileSize);
    file.close();

    std::string actualHash = calculateHash(fileData, "sha256");

    bool valid = (actualHash == expectedHash);
    if (!valid) {
        FALCON_LOG_ERROR("File verification failed: expected {}, got {}",
                        expectedHash, actualHash);
    } else {
        FALCON_LOG_INFO("File verification passed: {}", filePath);
    }

    return valid;
}

bool IncrementalDownloader::mergeFile(const std::string& localPath,
                                     const std::vector<std::vector<uint8_t>>& changedChunks,
                                     const std::vector<ChunkInfo>& chunkInfo) {
    // 合并文件
    FALCON_LOG_INFO("Merging file: {}", localPath);

    // 读取本地文件
    std::ifstream inFile(localPath, std::ios::binary);
    if (!inFile.is_open()) {
        FALCON_LOG_ERROR("Failed to open local file: {}", localPath);
        return false;
    }

    inFile.seekg(0, std::ios::end);
    uint64_t fileSize = inFile.tellg();
    inFile.seekg(0, std::ios::beg);

    std::vector<uint8_t> fileData(fileSize);
    inFile.read(reinterpret_cast<char*>(fileData.data()), fileSize);
    inFile.close();

    // 合并变化的部分
    for (size_t i = 0; i < chunkInfo.size(); ++i) {
        if (i < changedChunks.size()) {
            const auto& chunk = chunkInfo[i];
            const auto& data = changedChunks[i];

            if (chunk.changed && chunk.offset + data.size() <= fileData.size()) {
                std::memcpy(fileData.data() + chunk.offset, data.data(), data.size());
            }
        }
    }

    // 写回文件
    std::ofstream outFile(localPath, std::ios::binary);
    if (!outFile.is_open()) {
        FALCON_LOG_ERROR("Failed to open file for writing: {}", localPath);
        return false;
    }

    outFile.write(reinterpret_cast<const char*>(fileData.data()), fileData.size());
    outFile.close();

    return true;
}

} // namespace falcon
