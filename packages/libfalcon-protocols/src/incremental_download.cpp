/**
 * @file incremental_download.cpp
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

// ============================================================================
// IncrementalDownloader 实现
// ============================================================================

IncrementalDownloader::IncrementalDownloader() {
}

IncrementalDownloader::~IncrementalDownloader() {
}

// RollingChecksum：Adler 风格滚动弱校验和（rsync 块匹配第一关）。a 是窗口内
// 字节和，b 是逐字节前缀和之和，两者按 uint32 累加；窗口右滑一格只需用出界
// 字节与新进字节重算，无需重扫整个窗口。
void RollingChecksum::init(const uint8_t* data, uint32_t len) {
    a_ = 0;
    b_ = 0;
    window_ = len;
    for (uint32_t i = 0; i < len; ++i) {
        a_ += data[i];
        b_ += a_;
    }
}

void RollingChecksum::roll(uint8_t out, uint8_t in) {
    a_ = a_ - static_cast<uint32_t>(out) + static_cast<uint32_t>(in);
    b_ = b_ - static_cast<uint32_t>(window_) * static_cast<uint32_t>(out) + a_;
}

uint32_t RollingChecksum::value() const {
    return (a_ & 0xFFFFu) | ((b_ & 0xFFFFu) << 16);
}

//==============================================================================
// 哈希列表序列化 / 解析
//==============================================================================

std::string IncrementalDownloader::serializeHashList(
    const std::vector<ChunkInfo>& chunks,
    uint64_t chunkSize,
    const std::string& algorithm,
    uint64_t fileSize,
    const std::string& fileHash) {
    std::ostringstream ss;
    ss << "# falcon-hash-list v1\n";
    ss << "# chunkSize: " << chunkSize << "\n";
    ss << "# algorithm: " << algorithm << "\n";
    ss << "# fileSize: " << fileSize << "\n";
    ss << "# chunks: " << chunks.size() << "\n";
    if (!fileHash.empty()) {
        ss << "# fileHash: " << fileHash << "\n";
    }
    for (const auto& chunk : chunks) {
        ss << chunk.hash << "\n";
    }
    return ss.str();
}

bool IncrementalDownloader::parseHashListEx(
    const std::string& text,
    uint64_t defaultChunkSize,
    const std::string& defaultAlgorithm,
    HashListData& out) {
    out = HashListData{};

    uint64_t chunk_size = defaultChunkSize;
    uint64_t file_size = 0;
    bool has_file_size = false;
    uint64_t expected_chunks = 0;
    std::string file_hash;
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
                        return false;
                    }
                } else if (key == "filesize") {
                    file_size = std::stoull(value);
                    has_file_size = true;
                } else if (key == "chunks") {
                    expected_chunks = std::stoull(value);
                } else if (key == "filehash") {
                    // 整文件哈希用于下载后的端到端校验，格式非法则列表无效
                    if (!is_hex_string(value)) {
                        return false;
                    }
                    file_hash = to_lower_copy(value);
                }
            } catch (const std::exception&) {
                return false;  // 元数据数值非法 → 列表无效
            }
            continue;
        }

        // 哈希行：严格校验，任何非法行都使整个列表无效
        if (!is_hex_string(trimmed)) {
            return false;
        }
        hashes.push_back(trimmed);
    }

    if (hashes.empty()) {
        return false;
    }
    if (expected_chunks > 0 && hashes.size() != expected_chunks) {
        return false;
    }
    if (chunk_size == 0) {
        return false;
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

    out.chunks = std::move(chunks);
    out.fileHash = std::move(file_hash);
    return true;
}

std::vector<ChunkInfo> IncrementalDownloader::parseHashList(
    const std::string& text,
    uint64_t defaultChunkSize,
    const std::string& defaultAlgorithm) {
    HashListData data;
    if (!parseHashListEx(text, defaultChunkSize, defaultAlgorithm, data)) {
        return {};
    }
    return std::move(data.chunks);
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

    if ((::falcon::detail::inject_failure(
             ::falcon::detail::InjectPoint::IncrementalHashInit) ||
         EVP_DigestInit_ex(ctx, md, nullptr) != 1)) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    if ((::falcon::detail::inject_failure(
             ::falcon::detail::InjectPoint::IncrementalHashUpdate) ||
         EVP_DigestUpdate(ctx, data.c_str(), data.size()) != 1)) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;

    if ((::falcon::detail::inject_failure(
             ::falcon::detail::InjectPoint::IncrementalHashFinal) ||
         EVP_DigestFinal_ex(ctx, hash, &hashLen) != 1)) {
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

IncrementalDownloader::HashListData IncrementalDownloader::downloadRemoteHashList(
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

    HashListData list;
    if (!parseHashListEx(text, chunkSize, algorithm, list)) {
        FALCON_LOG_WARN("Invalid or mismatching hash list: {}", list_url);
        return {};
    }
    return list;
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
    // 注入命中时短路真实创建，避免已创建句柄在失败路径泄漏
    CURL* curl = ::falcon::detail::inject_failure(
                     ::falcon::detail::InjectPoint::CurlEasyInit)
                     ? nullptr
                     : curl_easy_init();
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
    diff.hashAlgorithm = options.hashAlgorithm;
    diff.verifyDownload = options.verifyDownload;

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
    auto remoteList = downloadRemoteHashList(remoteUrl, options.chunkSize,
                                             options.hashAlgorithm);

    if (remoteList.chunks.empty()) {
        FALCON_LOG_WARN("Failed to get remote hash list, falling back to full download");
        diff.chunks = std::move(remoteList.chunks);
        return diff;
    }

    // 计算远程文件大小
    for (const auto& chunk : remoteList.chunks) {
        diff.remoteSize += chunk.size;
    }

    // 比较哈希列表
    diff.chunks = compareHashLists(localChunks, remoteList.chunks);
    diff.fileHash = std::move(remoteList.fileHash);

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

    // 注入命中时短路真实创建，避免已创建句柄在失败路径泄漏
    CURL* curl = ::falcon::detail::inject_failure(
                     ::falcon::detail::InjectPoint::CurlEasyInit)
                     ? nullptr
                     : curl_easy_init();
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

    // 成品写入独立缓冲，绝不就地改写本地读入的缓冲：rsync 计划中未变化段的
    // 本地源偏移可能与输出位置不同（远端插入/删除造成错位），交换顺序的匹配
    // 会让「段 A 的搬运目标 == 段 B 的搬运源」，就地写会先毁掉尚未读取的数据
    std::vector<uint8_t> output(static_cast<size_t>(diff.remoteSize), 0);

    // 是否存在「本地源与输出位置不同」的未变化段（rsync 计划专有形态）
    bool divergent_copy = false;
    for (const auto& chunk : diff.chunks) {
        if (!chunk.changed && chunk.localOffset != ChunkInfo::kLocalOffsetAuto &&
            chunk.localOffset != chunk.offset) {
            divergent_copy = true;
            break;
        }
    }

    // 读取本地文件：v1 分块布局下本地位置与输出位置相同，只读前 remoteSize
    // 字节即可；rsync 计划中未变化段的本地源可能越出 remoteSize，需读到末尾
    std::vector<uint8_t> localData;
    std::ifstream inFile(diff.localPath, std::ios::binary);
    if (inFile.is_open()) {
        inFile.seekg(0, std::ios::end);
        // tellg 对不可定位流（如目录）返回 -1——按 0 处理，与 planRsync
        // 同一守卫，避免负值回绕成天文数字的缓冲分配
        const auto end_pos = inFile.tellg();
        const uint64_t localSize = end_pos >= 0 ? static_cast<uint64_t>(end_pos) : 0;
        inFile.seekg(0, std::ios::beg);
        const uint64_t readLimit =
            divergent_copy ? localSize : std::min(localSize, diff.remoteSize);
        if (readLimit > 0) {
            localData.resize(static_cast<size_t>(readLimit));
            inFile.read(reinterpret_cast<char*>(localData.data()),
                        static_cast<std::streamsize>(readLimit));
            const uint64_t got = static_cast<uint64_t>(inFile.gcount());
            if (got < readLimit) {
                localData.resize(static_cast<size_t>(got));
            }
        }
    }
    inFile.close();

    if (divergent_copy) {
        // 逐段显式搬运：源与目标位置无关，天然别名安全
        for (const auto& chunk : diff.chunks) {
            if (chunk.changed || chunk.size == 0) {
                continue;
            }
            const uint64_t src = (chunk.localOffset == ChunkInfo::kLocalOffsetAuto)
                                     ? chunk.offset
                                     : chunk.localOffset;
            if (src + chunk.size > localData.size()) {
                FALCON_LOG_ERROR("Local copy range out of bounds: "
                                 "offset {} + size {} > local {} bytes",
                                 src, chunk.size, localData.size());
                return false;
            }
            std::memcpy(output.data() + chunk.offset, localData.data() + src,
                        static_cast<size_t>(chunk.size));
        }
    } else if (!localData.empty()) {
        // v1：本地区域与输出区域逐字节对齐，前缀直填
        std::memcpy(output.data(), localData.data(), localData.size());
    }

    // 下载变化的部分
    uint64_t downloaded = 0;

    for (const auto& chunk : diff.chunks) {
        // 零尺寸分块无可下载内容，跳过（也避免 data.data()==nullptr 的
        // 零长度 memcpy——UBSan nonnull 检查下的未定义行为）
        if (chunk.changed && chunk.size > 0) {
            auto data = downloadRange(diff.remotePath, chunk.offset, chunk.size);
            if (data.size() != chunk.size) {
                FALCON_LOG_ERROR("Failed to download chunk at offset {}", chunk.offset);
                return false;
            }

            // 分块哈希校验：哈希可用即启用（保护增量机制自身的完整性；
            // verifyDownload 只门控最后的整文件校验）
            if (!diff.hashAlgorithm.empty() && !chunk.hash.empty()) {
                const std::string chunk_data(reinterpret_cast<const char*>(data.data()),
                                             data.size());
                if (calculateHash(chunk_data, diff.hashAlgorithm) != chunk.hash) {
                    FALCON_LOG_ERROR("Chunk hash mismatch at offset {}", chunk.offset);
                    return false;
                }
            }

            // 写入到对应位置
            std::memcpy(output.data() + chunk.offset, data.data(), data.size());

            downloaded += data.size();

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

    outFile.write(reinterpret_cast<const char*>(output.data()),
                  static_cast<std::streamsize>(diff.remoteSize));
    outFile.close();

    // 端到端整文件校验：分块校验只覆盖变化部分，未变化的本地区域
    // 在比较与下载之间可能被修改（成品 = 本地区域 + 下载块，任何一侧
    // 损坏都应拦在发布前）；不匹配绝不把损坏成品当成功留下
    if (diff.verifyDownload && !diff.fileHash.empty()) {
        if (!verifyFile(outputPath, diff.fileHash)) {
            FALCON_LOG_ERROR("Incremental output failed verification, removing: {}",
                             outputPath);
            std::error_code ec;
            std::filesystem::remove(outputPath, ec);
            return false;
        }
    }

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
