/**
 * @file bittorrent_plugin.cpp
 * @brief BitTorrent/Magnet 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "bittorrent_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <random>
#include <chrono>
#include <regex>
#include <openssl/sha.h>
#include <algorithm>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>

namespace falcon {
namespace protocols {

// ============================================================================
// BitTorrentPlugin 实现
// ============================================================================

BitTorrentPlugin::BitTorrentPlugin() {
    FALCON_LOG_INFO("BitTorrent plugin initialized");
}

BitTorrentPlugin::~BitTorrentPlugin() {
    FALCON_LOG_DEBUG("BitTorrent plugin shutdown");
}

std::vector<std::string> BitTorrentPlugin::getSupportedSchemes() const {
    return {"magnet", "bittorrent"};
}

bool BitTorrentPlugin::canHandle(const std::string& url) const {
    // 检查 magnet 链接
    if (url.find("magnet:") == 0) {
        return true;
    }

    // 检查 .torrent 文件
    if (url.find(".torrent") != std::string::npos) {
        return true;
    }

    // 检查 bittorrent:// 链接（自定义协议）
    if (url.find("bittorrent://") == 0) {
        return true;
    }

    return false;
}

std::unique_ptr<IDownloadTask> BitTorrentPlugin::createTask(const std::string& url,
                                                           const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating BitTorrent task for: {}", url);

    return std::make_unique<BitTorrentDownloadTask>(url, options);
}

// ============================================================================
// BitTorrentDownloadTask 实现
// ============================================================================

BitTorrentPlugin::BitTorrentDownloadTask::BitTorrentDownloadTask(const std::string& url,
                                                                 const DownloadOptions& options)
    : url_(url)
    , options_(options)
    , status_(TaskStatus::Pending)
    , totalSize_(0)
    , downloadedBytes_(0)
    , uploadBytes_(0)
    , downloadSpeed_(0)
    , uploadSpeed_(0) {

#ifdef FALCON_USE_LIBTORRENT
    // 配置 session
    libtorrent::settings_pack settings;
    settings.set_str(libtorrent::settings_pack::listen_interfaces, "0.0.0.0:6881");
    settings.set_bool(libtorrent::settings_pack::enable_dht, true);
    settings.set_bool(libtorrent::settings_pack::enable_lsd, true);
    settings.set_bool(libtorrent::settings_pack::enable_upnp, true);
    settings.set_bool(libtorrent::settings_pack::enable_natpmp, true);

    session_ = libtorrent::session(settings);
#endif
}

BitTorrentPlugin::BitTorrentDownloadTask::~BitTorrentDownloadTask() {
    if (status_ == TaskStatus::Downloading) {
        cancel();
    }
}

void BitTorrentPlugin::BitTorrentDownloadTask::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (status_ != TaskStatus::Pending) {
        return;
    }

    status_ = TaskStatus::Downloading;

    // 解析输入 URL
    bool success = false;
    if (url_.find("magnet:") == 0) {
        success = parseMagnetUri(url_);
    } else if (url_.find(".torrent") != std::string::npos || url_.find("file://") == 0) {
        // 处理本地 torrent 文件
        std::string filePath = url_;
        if (url_.find("file://") == 0) {
            filePath = url_.substr(7);
        }
        success = parseTorrentFile(filePath);
    } else if (url_.find("bittorrent://") == 0) {
        // 自定义协议，转换为 magnet 或下载 torrent 文件
        std::string extractedUrl = url_.substr(13);
        if (extractedUrl.find("magnet:") == 0) {
            success = parseMagnetUri(extractedUrl);
        } else {
            // 假设是 torrent 文件 URL
            // 这里需要先下载 torrent 文件
            FALCON_LOG_WARN("Bittorrent URL requires torrent file download");
            success = false;
        }
    }

    if (!success) {
        status_ = TaskStatus::Failed;
        return;
    }

#ifdef FALCON_USE_LIBTORRENT
    // 添加 torrent 到 session
    libtorrent::torrent_handle handle = session_.add_torrent(params_);

    // 设置保存路径
    if (!options_.output_path.empty()) {
        handle.set_save_path(options_.output_path);
    }

    // 开始下载
    handle.resume();

    handle_ = handle;

    FALCON_LOG_INFO("BitTorrent download started: {}", params_.name);
#endif
}

void BitTorrentPlugin::BitTorrentDownloadTask::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == TaskStatus::Downloading) {
        status_ = TaskStatus::Paused;
#ifdef FALCON_USE_LIBTORRENT
        if (handle_.is_valid()) {
            handle_.pause();
        }
#endif
    }
}

void BitTorrentPlugin::BitTorrentDownloadTask::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == TaskStatus::Paused) {
        status_ = TaskStatus::Downloading;
#ifdef FALCON_USE_LIBTORRENT
        if (handle_.is_valid()) {
            handle_.resume();
        }
#endif
    }
}

void BitTorrentPlugin::BitTorrentDownloadTask::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = TaskStatus::Cancelled;

#ifdef FALCON_USE_LIBTORRENT
    if (handle_.is_valid()) {
        session_.remove_torrent(handle_);
    }
#endif
}

TaskStatus BitTorrentPlugin::BitTorrentDownloadTask::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

float BitTorrentPlugin::BitTorrentDownloadTask::getProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (totalSize_ == 0) return 0.0f;
    return static_cast<float>(downloadedBytes_) / totalSize_;
}

uint64_t BitTorrentPlugin::BitTorrentDownloadTask::getTotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalSize_;
}

uint64_t BitTorrentPlugin::BitTorrentDownloadTask::getDownloadedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 更新统计信息
    const_cast<BitTorrentDownloadTask*>(this)->updateStats();

    return downloadedBytes_;
}

uint64_t BitTorrentPlugin::BitTorrentDownloadTask::getSpeed() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 更新统计信息
    const_cast<BitTorrentDownloadTask*>(this)->updateStats();

    return downloadSpeed_;
}

std::string BitTorrentPlugin::BitTorrentDownloadTask::getErrorMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errorMessage_;
}

bool BitTorrentPlugin::BitTorrentDownloadTask::parseTorrentFile(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        errorMessage_ = "Failed to open torrent file: " + filePath;
        return false;
    }

    std::string data((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());

    size_t pos = 0;
    BValue torrent = parseBencode(data, pos);

    if (!validateTorrent(torrent)) {
        errorMessage_ = "Invalid torrent file";
        return false;
    }

#ifdef FALCON_USE_LIBTORRENT
    try {
        libtorrent::add_torrent_params p;
        libtorrent::error_code ec;

        // 使用 libtorrent 解析
        libtorrent::torrent_info ti(data, ec);
        if (ec) {
            errorMessage_ = "Failed to parse torrent: " + ec.message();
            return false;
        }

        p.ti = std::make_shared<libtorrent::torrent_info>(ti);
        p.save_path = options_.output_path.empty() ? "./downloads" : options_.output_path;

        params_ = p;

        // 更新文件信息
        totalSize_ = ti.total_size();
        files_.clear();

        for (int i = 0; i < ti.num_files(); ++i) {
            FileInfo info;
            info.path = ti.files().file_path(i);
            info.size = ti.files().file_size(i);
            info.name = info.path.substr(info.path.find_last_of("/\\") + 1);
            files_.push_back(info);
        }

        return true;
    } catch (const std::exception& e) {
        errorMessage_ = "Exception parsing torrent: " + std::string(e.what());
        return false;
    }
#else
    // 简化的 B 编码解析
    errorMessage_ = "BitTorrent support not compiled with libtorrent";
    return false;
#endif
}

bool BitTorrentPlugin::BitTorrentDownloadTask::parseMagnetUri(const std::string& magnetUri) {
#ifdef FALCON_USE_LIBTORRENT
    try {
        libtorrent::add_torrent_params p = libtorrent::parse_magnet_uri(magnetUri);
        p.save_path = options_.output_path.empty() ? "./downloads" : options_.output_path;

        params_ = p;

        FALCON_LOG_INFO("Parsed magnet URI: {}", p.name);
        return true;
    } catch (const std::exception& e) {
        errorMessage_ = "Failed to parse magnet URI: " + std::string(e.what());
        return false;
    }
#else
    // 手动解析 magnet URI
    std::regex magnetRegex(R"(magnet:\?xt=urn:btih:([a-fA-F0-9]{40}|[a-zA-Z2-7]{32})(&dn=([^&]*))?(&tr=([^&]*))*)");
    std::smatch match;

    if (!std::regex_match(magnetUri, match, magnetRegex)) {
        errorMessage_ = "Invalid magnet URI format";
        return false;
    }

    std::string infoHash = match[1].str();
    std::string name = match[3].str();

    FALCON_LOG_DEBUG("Magnet info_hash: {}, name: {}", infoHash, name);

    // 这里需要实现自定义的 BitTorrent 下载逻辑
    errorMessage_ = "Magnet URI support requires libtorrent";
    return false;
#endif
}

void BitTorrentPlugin::BitTorrentDownloadTask::updateStats() {
#ifdef FALCON_USE_LIBTORRENT
    if (handle_.is_valid()) {
        libtorrent::torrent_status status = handle_.status();

        totalSize_ = status.total_wanted;
        downloadedBytes_ = status.total_wanted_done;
        downloadSpeed_ = status.download_rate;
        uploadSpeed_ = status.upload_rate;

        // 检查状态
        if (status.paused) {
            status_ = TaskStatus::Paused;
        } else if (status.finished || status.seeding) {
            status_ = TaskStatus::Completed;
        } else if (status.error) {
            status_ = TaskStatus::Failed;
            errorMessage_ = status.error.message();
        }
    }
#endif
}

void BitTorrentPlugin::BitTorrentDownloadTask::selectFiles() {
#ifdef FALCON_USE_LIBTORRENT
    if (handle_.is_valid() && params_.ti) {
        // 这里可以选择要下载的文件
        // 默认下载所有文件
        std::vector<int> filePriorities(params_.ti->num_files(), 7);
        handle_.prioritize_files(filePriorities);
    }
#endif
}

void BitTorrentPlugin::BitTorrentDownloadTask::handleAlerts() {
#ifdef FALCON_USE_LIBTORRENT
    std::vector<libtorrent::alert*> alerts;
    session_.pop_alerts(&alerts);

    for (auto alert : alerts) {
        if (auto ta = libtorrent::alert_cast<libtorrent::torrent_error_alert>(alert)) {
            FALCON_LOG_ERROR("Torrent error: {}", ta->error.message());
            errorMessage_ = ta->error.message();
            status_ = TaskStatus::Failed;
        }
        // 处理其他类型的警报...
    }
#endif
}

// ============================================================================
// B 编码解析（简化实现）
// ============================================================================

BitTorrentPlugin::BValue BitTorrentPlugin::parseBencode(const std::string& data, size_t& pos) {
    if (pos >= data.length()) {
        throw std::runtime_error("Invalid bencode data");
    }

    char c = data[pos];

    if (c == 'i') {
        // 整数
        ++pos;
        std::string num;
        while (pos < data.length() && data[pos] != 'e') {
            num += data[pos++];
        }
        ++pos;  // 跳过 'e'

        BValue value;
        value.type = BValue::Integer;
        value.intValue = std::stoll(num);
        return value;
    } else if (c == 'l') {
        // 列表
        ++pos;
        BValue value;
        value.type = BValue::List;
        while (pos < data.length() && data[pos] != 'e') {
            value.listValue.push_back(parseBencode(data, pos));
        }
        ++pos;  // 跳过 'e'
        return value;
    } else if (c == 'd') {
        // 字典
        ++pos;
        BValue value;
        value.type = BValue::Dict;
        while (pos < data.length() && data[pos] != 'e') {
            BValue key = parseBencode(data, pos);
            if (key.type != BValue::String) {
                throw std::runtime_error("Dictionary key must be string");
            }
            BValue val = parseBencode(data, pos);
            value.dictValue[key.strValue] = val;
        }
        ++pos;  // 跳过 'e'
        return value;
    } else if (std::isdigit(c)) {
        // 字符串
        std::string lenStr;
        while (pos < data.length() && std::isdigit(data[pos])) {
            lenStr += data[pos++];
        }
        if (data[pos] != ':') {
            throw std::runtime_error("Invalid string format");
        }
        ++pos;

        size_t len = std::stoul(lenStr);
        if (pos + len > data.length()) {
            throw std::runtime_error("String length exceeds data");
        }

        BValue value;
        value.type = BValue::String;
        value.strValue = data.substr(pos, len);
        pos += len;
        return value;
    }

    throw std::runtime_error("Invalid bencode data");
}

bool BitTorrentPlugin::validateTorrent(const BValue& torrent) {
    if (torrent.type != BValue::Dict) {
        return false;
    }

    auto& dict = torrent.dictValue;

    // 检查必需的字段
    if (dict.find("info") == dict.end() || dict.at("info").type != BValue::Dict) {
        return false;
    }

    auto& info = dict.at("info").dictValue;

    // 必须有 name、pieces、length 或 files
    if (info.find("name") == info.end() || info.find("pieces") == info.end()) {
        return false;
    }

    if (info.find("length") == info.end() && info.find("files") == info.end()) {
        return false;
    }

    return true;
}

std::string BitTorrentPlugin::sha1(const std::string& data) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }

    return ss.str();
}

std::string BitTorrentPlugin::generateNodeId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    std::string nodeId(20, 0);
    for (int i = 0; i < 20; ++i) {
        nodeId[i] = static_cast<char>(dis(gen()));
    }

    return nodeId;
}

// ============================================================================
// 工具函数实现
// ============================================================================

std::string BitTorrentPlugin::bencodeToString(const BValue& value) {
    std::ostringstream ss;

    switch (value.type) {
        case BValue::String:
            ss << value.strValue.length() << ":" << value.strValue;
            break;
        case BValue::Integer:
            ss << "i" << value.intValue << "e";
            break;
        case BValue::List:
            ss << "l";
            for (const auto& item : value.listValue) {
                ss << bencodeToString(item);
            }
            ss << "e";
            break;
        case BValue::Dict:
            ss << "d";
            for (const auto& pair : value.dictValue) {
                ss << bencodeToString(BValue{BValue::String, pair.first, 0, {}, {}}); // key
                ss << bencodeToString(pair.second); // value
            }
            ss << "e";
            break;
    }

    return ss.str();
}

std::string BitTorrentPlugin::base32Decode(const std::string& input) {
    // Base32 字母表
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    static int decode_table[256] = {};

    // 初始化解码表（仅一次）
    static bool initialized = []() {
        std::fill(std::begin(decode_table), std::end(decode_table), -1);
        for (int i = 0; i < 32; ++i) {
            decode_table[static_cast<unsigned char>(alphabet[i])] = i;
        }
        return true;
    }();
    (void)initialized;

    // 移除填充字符
    std::string cleaned;
    for (char c : input) {
        if (c != '=') {
            cleaned += c;
        }
    }

    std::string result;
    result.reserve((cleaned.length() * 5) / 8);

    uint32_t buffer = 0;
    int bits = 0;

    for (char c : cleaned) {
        int value = decode_table[static_cast<unsigned char>(c)];
        if (value < 0) continue;

        buffer = (buffer << 5) | static_cast<uint32_t>(value);
        bits += 5;

        while (bits >= 8) {
            bits -= 8;
            result += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }

    return result;
}

std::string BitTorrentPlugin::urlDecode(const std::string& url) {
    std::ostringstream decoded;
    for (size_t i = 0; i < url.length(); ++i) {
        if (url[i] == '%' && i + 2 < url.length()) {
            char hex[3] = {url[i + 1], url[i + 2], '\0'};
            int value = std::stoi(hex, nullptr, 16);
            decoded << static_cast<char>(value);
            i += 2;
        } else if (url[i] == '+') {
            decoded << ' ';
        } else {
            decoded << url[i];
        }
    }
    return decoded.str();
}

std::vector<std::string> BitTorrentPlugin::getTrackers(const BValue& torrent) {
    std::vector<std::string> trackers;

    if (torrent.type != BValue::Dict) {
        return trackers;
    }

    auto& dict = torrent.dictValue;

    // 检查单 tracker
    if (dict.find("announce") != dict.end()) {
        const auto& announce = dict.at("announce");
        if (announce.type == BValue::String) {
            trackers.push_back(announce.strValue);
        }
    }

    // 检查 tracker 列表
    if (dict.find("announce-list") != dict.end()) {
        const auto& announceList = dict.at("announce-list");
        if (announceList.type == BValue::List) {
            for (const auto& tier : announceList.listValue) {
                if (tier.type == BValue::List) {
                    for (const auto& tracker : tier.listValue) {
                        if (tracker.type == BValue::String) {
                            trackers.push_back(tracker.strValue);
                        }
                    }
                } else if (tier.type == BValue::String) {
                    trackers.push_back(tier.strValue);
                }
            }
        }
    }

    return trackers;
}

// ============================================================================
// 纯 C++ BitTorrent 实现
// ============================================================================

#ifndef FALCON_USE_LIBTORRENT

namespace {
    // HTTP GET 请求函数（用于连接 tracker）
    std::string http_get(const std::string& url, const std::map<std::string, std::string>& params) {
        // 简化实现：仅支持 HTTP tracker
        // 实际实现需要完整的 HTTP 客户端
        // 这里返回空的 bencode 响应
        FALCON_LOG_WARN("HTTP tracker request not implemented in pure C++ mode");
        return "d14:failure reason27:Tracker not implementede";
    }

    // UDP tracker 相关函数
    struct UdpTrackerMessage {
        uint32_t action;      // 0=connect, 1=announce, 2=scrape, 3=error
        uint32_t transaction_id;
    };

    uint64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void BitTorrentPlugin::BitTorrentDownloadTask::startDownloadThread() {
    running_.store(true);
    paused_.store(false);

    downloadThread_ = std::thread([this]() {
        FALCON_LOG_INFO("BitTorrent download thread started");

        // 初始化 piece 状态
        pieceState_.havePiece.resize(torrentInfo_.pieceCount, false);
        pieceState_.requestedPiece.resize(torrentInfo_.pieceCount, false);
        pieceState_.downloadingPiece.resize(torrentInfo_.pieceCount, false);
        pieceState_.pieceData.resize(torrentInfo_.pieceCount);

        // 主下载循环
        while (running_.load() && !cancelled_.load()) {
            if (paused_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 1. 连接到 tracker 获取 peers
            if (peers_.empty()) {
                for (const auto& tracker : torrentInfo_.trackers) {
                    if (connectToTracker(tracker)) {
                        break;
                    }
                }
            }

            // 2. 尝试 DHT 查找
            if (peers_.empty()) {
                findPeersViaDHT();
            }

            // 3. 下载 piece
            for (int i = 0; i < torrentInfo_.pieceCount; ++i) {
                if (!running_.load() || cancelled_.load()) break;
                if (paused_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    --i;
                    continue;
                }

                if (!pieceState_.havePiece[i] && !pieceState_.downloadingPiece[i]) {
                    if (downloadPiece(i)) {
                        downloadedBytes_ += torrentInfo_.pieceLength;
                        if (downloadedBytes_ > totalSize_) {
                            downloadedBytes_ = totalSize_;
                        }
                    }
                }
            }

            // 检查是否完成
            bool allComplete = true;
            for (int i = 0; i < torrentInfo_.pieceCount; ++i) {
                if (!pieceState_.havePiece[i]) {
                    allComplete = false;
                    break;
                }
            }

            if (allComplete) {
                status_ = TaskStatus::Completed;
                FALCON_LOG_INFO("BitTorrent download completed");
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        running_.store(false);
        FALCON_LOG_INFO("BitTorrent download thread ended");
    });
}

bool BitTorrentPlugin::BitTorrentDownloadTask::connectToTracker(const std::string& trackerUrl) {
    FALCON_LOG_INFO("Connecting to tracker: {}", trackerUrl);

    // 解析 tracker URL
    if (trackerUrl.find("http://") == 0 || trackerUrl.find("https://") == 0) {
        // HTTP tracker
        std::map<std::string, std::string> params;
        params["info_hash"] = torrentInfo_.infoHash;
        params["peer_id"] = generateNodeId();
        params["port"] = "6881";
        params["uploaded"] = std::to_string(uploadBytes_);
        params["downloaded"] = std::to_string(downloadedBytes_);
        params["left"] = std::to_string(totalSize_ - downloadedBytes_);
        params["compact"] = "1";
        params["event"] = "started";

        std::string response = http_get(trackerUrl, params);

        // 解析响应
        try {
            size_t pos = 0;
            BValue result = parseBencode(response, pos);

            if (result.dictValue.find("peers") != result.dictValue.end()) {
                const auto& peersValue = result.dictValue.at("peers");
                // 解析 peers 列表（紧凑格式或字典格式）
                FALCON_LOG_INFO("Received peers from tracker");
                return true;
            }

            if (result.dictValue.find("failure reason") != result.dictValue.end()) {
                FALCON_LOG_ERROR("Tracker failure: {}",
                    result.dictValue.at("failure reason").strValue);
                return false;
            }
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR("Failed to parse tracker response: {}", e.what());
        }
    } else if (trackerUrl.find("udp://") == 0) {
        // UDP tracker
        FALCON_LOG_WARN("UDP tracker not implemented yet");
        return false;
    }

    return false;
}

bool BitTorrentPlugin::BitTorrentDownloadTask::findPeersViaDHT() {
    FALCON_LOG_DEBUG("DHT peer search started");

    // 简化 DHT 实现：使用已知路由节点
    // 实际实现需要完整的 DHT 协议支持

    return false;
}

bool BitTorrentPlugin::BitTorrentDownloadTask::connectToPeer(const PeerInfo& peer) {
    FALCON_LOG_DEBUG("Connecting to peer: {}:{}", peer.ip, peer.port);

    // 简化实现：假设连接成功
    return true;
}

bool BitTorrentPlugin::BitTorrentDownloadTask::downloadPiece(int pieceIndex) {
    if (pieceIndex < 0 || pieceIndex >= torrentInfo_.pieceCount) {
        return false;
    }

    pieceState_.downloadingPiece[pieceIndex] = true;

    // 尝试从已有的 peers 下载
    for (const auto& peer : peers_) {
        if (!running_.load() || cancelled_.load() || paused_.load()) {
            pieceState_.downloadingPiece[pieceIndex] = false;
            return false;
        }

        if (requestPiece(peer, pieceIndex, 0, torrentInfo_.pieceLength)) {
            // 等待 piece 数据（简化实现）
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // 实际实现需要处理 peer 消息并验证 piece
            pieceState_.havePiece[pieceIndex] = true;
            pieceState_.downloadingPiece[pieceIndex] = false;
            return true;
        }
    }

    pieceState_.downloadingPiece[pieceIndex] = false;
    return false;
}

bool BitTorrentPlugin::BitTorrentDownloadTask::verifyPiece(int pieceIndex, const std::vector<uint8_t>& data) {
    if (pieceIndex < 0 || pieceIndex >= torrentInfo_.pieceCount) {
        return false;
    }

    // 计算 piece 的 SHA1 哈希
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(data.data(), data.size(), hash);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }

    // 与 torrent 中的哈希比较
    std::string computedHash = ss.str();
    std::string expectedHash = torrentInfo_.pieces[pieceIndex];

    return computedHash == expectedHash;
}

bool BitTorrentPlugin::BitTorrentDownloadTask::writePiece(int pieceIndex, const std::vector<uint8_t>& data) {
    // 创建输出目录
    std::filesystem::path outputPath(options_.output_path);
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path());
    }

    // 打开文件并写入 piece 数据
    std::ofstream file(outputPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        FALCON_LOG_ERROR("Failed to open output file: {}", outputPath.string());
        return false;
    }

    file.seekp(static_cast<std::streampos>(pieceIndex) * torrentInfo_.pieceLength);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());

    return file.good();
}

std::vector<uint8_t> BitTorrentPlugin::BitTorrentDownloadTask::readPiece(int pieceIndex) {
    std::vector<uint8_t> result;

    std::filesystem::path outputPath(options_.output_path);
    std::ifstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        return result;
    }

    file.seekg(static_cast<std::streampos>(pieceIndex) * torrentInfo_.pieceLength);

    int pieceSize = torrentInfo_.pieceLength;
    if (pieceIndex == torrentInfo_.pieceCount - 1) {
        // 最后一个 piece 可能较小
        pieceSize = static_cast<int>(totalSize_ % torrentInfo_.pieceLength);
        if (pieceSize == 0) {
            pieceSize = torrentInfo_.pieceLength;
        }
    }

    result.resize(pieceSize);
    file.read(reinterpret_cast<char*>(result.data()), pieceSize);

    return result;
}

bool BitTorrentPlugin::BitTorrentDownloadTask::requestPiece(const PeerInfo& peer, int pieceIndex, int begin, int length) {
    // 构建 BitTorrent 协议消息
    // 实际实现需要与 peer 建立 TCP 连接并发送请求

    FALCON_LOG_DEBUG("Requesting piece {} from {}:{} (offset: {}, size: {})",
        pieceIndex, peer.ip, peer.port, begin, length);

    return true;  // 简化实现：假设请求成功
}

void BitTorrentPlugin::BitTorrentDownloadTask::handlePeerMessages(const PeerInfo& peer) {
    // 处理来自 peer 的消息
    // 包括: piece, reject, request, cancel, keep-alive 等
}

#endif

} // namespace protocols
} // namespace falcon