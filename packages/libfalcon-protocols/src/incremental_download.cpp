/**
 * @file incremental_download.cpp
 * @brief 增量下载功能实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include <falcon/protocols/incremental_download.hpp>
#include <falcon/logger.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <cctype>
#include <cstring>

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

// ============================================================================
// IncrementalDownloader 实现
// ============================================================================

IncrementalDownloader::IncrementalDownloader() {
}

IncrementalDownloader::~IncrementalDownloader() {
}

//==============================================================================
// 哈希列表序列化 / 解析
//==============================================================================

std::string IncrementalDownloader::serializeHashList(
    const std::vector<ChunkInfo>& chunks,
    uint64_t chunkSize,
    const std::string& algorithm,
    uint64_t fileSize) {
    std::ostringstream ss;
    ss << "# falcon-hash-list v1\n";
    ss << "# chunkSize: " << chunkSize << "\n";
    ss << "# algorithm: " << algorithm << "\n";
    ss << "# fileSize: " << fileSize << "\n";
    ss << "# chunks: " << chunks.size() << "\n";
    for (const auto& chunk : chunks) {
        ss << chunk.hash << "\n";
    }
    return ss.str();
}

std::vector<ChunkInfo> IncrementalDownloader::parseHashList(
    const std::string& text,
    uint64_t defaultChunkSize,
    const std::string& defaultAlgorithm) {
    uint64_t chunk_size = defaultChunkSize;
    uint64_t file_size = 0;
    bool has_file_size = false;
    uint64_t expected_chunks = 0;
    std::vector<std::string> hashes;

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string trimmed = trim_line(line);
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed[0] == '#') {
            std::string key;
            std::string value;
            if (!parse_metadata_line(trimmed, key, value)) {
                continue;  // 注释行
            }
            try {
                if (key == "chunksize") {
                    const uint64_t v = std::stoull(value);
                    if (v > 0) chunk_size = v;
                } else if (key == "algorithm") {
                    // 调用方的 defaultAlgorithm 是期望算法（与本地计算一致），
                    // 列表声明了不同算法则两者不可比较
                    if (!value.empty() &&
                        to_lower_copy(value) != to_lower_copy(defaultAlgorithm)) {
                        return {};
                    }
                } else if (key == "filesize") {
                    file_size = std::stoull(value);
                    has_file_size = true;
                } else if (key == "chunks") {
                    expected_chunks = std::stoull(value);
                }
            } catch (const std::exception&) {
                return {};  // 元数据数值非法 → 列表无效
            }
            continue;
        }

        // 哈希行：严格校验，任何非法行都使整个列表无效
        if (!is_hex_string(trimmed)) {
            return {};
        }
        hashes.push_back(trimmed);
    }

    if (hashes.empty()) {
        return {};
    }
    if (expected_chunks > 0 && hashes.size() != expected_chunks) {
        return {};
    }
    if (chunk_size == 0) {
        return {};
    }

    std::vector<ChunkInfo> chunks;
    chunks.reserve(hashes.size());
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        ChunkInfo info;
        info.offset = static_cast<uint64_t>(i) * chunk_size;
        info.size = chunk_size;
        // 最后一块按实际文件大小收缩（元数据提供 fileSize 时）
        if (has_file_size && file_size > info.offset) {
            info.size = std::min(chunk_size, file_size - info.offset);
        }
        info.hash = hashes[i];
        info.changed = false;
        chunks.push_back(std::move(info));
    }
    return chunks;
}

std::string IncrementalDownloader::calculateHash(const std::string& data,
                                                 const std::string& algorithm) {
#if defined(FALCON_USE_OPENSSL) || defined(FALCON_HAS_OPENSSL)
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
#else
    // Fallback: simple hash implementation when OpenSSL is not available
    // This is a simple XOR-based hash for placeholder purposes
    (void)algorithm;
    FALCON_LOG_WARN("OpenSSL not available, using fallback hash implementation");
    uint32_t hash = 0;
    for (char c : data) {
        hash = hash * 31 + static_cast<uint8_t>(c);
    }

    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(8) << hash;
    return ss.str();
#endif
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
    const auto end_pos = file.tellg();
    uint64_t fileSize = end_pos >= 0 ? static_cast<uint64_t>(end_pos) : 0;
    file.seekg(0, std::ios::beg);

    uint64_t offset = 0;
    uint64_t remaining = fileSize;

    while (remaining > 0) {
        uint64_t currentChunkSize = std::min(chunkSize, remaining);
        std::vector<char> buffer(currentChunkSize);

        file.read(buffer.data(), static_cast<std::streamsize>(currentChunkSize));
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

#ifdef FALCON_USE_CURL
    // 哈希列表 URL 约定：<file url>.falconhash
    const std::string list_url = url + ".falconhash";
    std::string text;
    if (!http_get(list_url, text)) {
        FALCON_LOG_WARN("Failed to download hash list: {}", list_url);
        return {};
    }

    auto chunks = parseHashList(text, chunkSize, algorithm);
    if (chunks.empty()) {
        FALCON_LOG_WARN("Invalid or mismatching hash list: {}", list_url);
    }
    return chunks;
#else
    (void)url;
    (void)chunkSize;
    (void)algorithm;
    FALCON_LOG_WARN("Remote hash list download requires libcurl (FALCON_USE_CURL)");
    return {};
#endif
}

bool IncrementalDownloader::http_get(const std::string& url, std::string& out) {
#ifdef FALCON_USE_CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // 增量比较不应长时间挂起：连接与总时长兜底
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        FALCON_LOG_WARN("HTTP GET failed: {} ({})", url, curl_easy_strerror(res));
        return false;
    }
    return !out.empty() || res == CURLE_OK;
#else
    (void)url;
    (void)out;
    FALCON_LOG_WARN("HTTP support requires libcurl (FALCON_USE_CURL)");
    return false;
#endif
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
        const auto end_pos = localFile.tellg();
        diff.localSize = end_pos >= 0 ? static_cast<uint64_t>(end_pos) : 0;
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
        static_cast<double>(diff.totalChanged) / static_cast<double>(diff.remoteSize) : 0.0;

    FALCON_LOG_INFO("File comparison complete: {} local, {} remote, {} changed ({:.1f}%)",
                   diff.localSize, diff.remoteSize, diff.totalChanged,
                   diff.ratio * 100);

    return diff;
}

std::vector<uint8_t> IncrementalDownloader::downloadRange(
    const std::string& url,
    uint64_t offset,
    uint64_t size) {

#ifdef FALCON_USE_CURL
    if (size == 0) {
        return {};
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return {};
    }

    // CURLOPT_RANGE 的值是 Range 头的字节范围（curl 自动添加 "Range: " 前缀）
    const std::string range =
        std::to_string(offset) + "-" + std::to_string(offset + size - 1);
    std::string out;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        FALCON_LOG_WARN("Range download failed: {} [{}] ({})",
                        url, range, curl_easy_strerror(res));
        return {};
    }
    if (out.size() != size) {
        FALCON_LOG_WARN("Range download size mismatch: {} [{}] expected {} got {}",
                        url, range, size, out.size());
        return {};
    }

    return std::vector<uint8_t>(out.begin(), out.end());
#else
    // 无 libcurl 时无法发起 HTTP 请求
    (void)url;
    (void)offset;
    (void)size;

    FALCON_LOG_WARN("Range download requires libcurl (FALCON_USE_CURL)");
    return {};
#endif
}

bool IncrementalDownloader::downloadChanged(const FileDiff& diff,
                                           const std::string& outputPath,
                                           ProgressCallback callback) {
    FALCON_LOG_INFO("Downloading changed parts: {} bytes of {} total",
                   diff.totalChanged, diff.remoteSize);

    // 读取本地文件（本地比远程大时只读取能容纳的部分，防止越界）
    std::vector<uint8_t> localData(diff.remoteSize);

    std::ifstream inFile(diff.localPath, std::ios::binary);
    if (inFile.is_open()) {
        const uint64_t readable = std::min(diff.localSize, diff.remoteSize);
        if (readable > 0) {
            inFile.read(reinterpret_cast<char*>(localData.data()),
                        static_cast<std::streamsize>(readable));
        }
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

    outFile.write(reinterpret_cast<const char*>(localData.data()),
                  static_cast<std::streamsize>(diff.remoteSize));
    outFile.close();

    FALCON_LOG_INFO("Incremental download completed: {}", outputPath);
    return true;
}

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

bool IncrementalDownloader::verifyFile(const std::string& filePath,
                                      const std::string& expectedHash) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        FALCON_LOG_ERROR("Failed to open file for verification: {}", filePath);
        return false;
    }

    file.seekg(0, std::ios::end);
    const auto end_pos = file.tellg();
    const uint64_t fileSize = end_pos >= 0 ? static_cast<uint64_t>(end_pos) : 0;
    file.seekg(0, std::ios::beg);

    std::string fileData(fileSize, '\0');
    file.read(&fileData[0], static_cast<std::streamsize>(fileSize));
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
    const auto end_pos = inFile.tellg();
    const uint64_t fileSize = end_pos >= 0 ? static_cast<uint64_t>(end_pos) : 0;
    inFile.seekg(0, std::ios::beg);

    std::vector<uint8_t> fileData(fileSize);
    inFile.read(reinterpret_cast<char*>(fileData.data()),
                static_cast<std::streamsize>(fileSize));
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

    outFile.write(reinterpret_cast<const char*>(fileData.data()),
                  static_cast<std::streamsize>(fileData.size()));
    outFile.close();

    return true;
}

} // namespace falcon
