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

namespace falcon {
namespace plugins {

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
        nodeId[i] = static_cast<char>(dis(gen));
    }

    return nodeId;
}

} // namespace plugins
} // namespace falcon