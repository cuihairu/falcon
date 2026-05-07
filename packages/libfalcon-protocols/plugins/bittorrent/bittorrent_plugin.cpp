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
// BitTorrentHandler 实现
// ============================================================================

BitTorrentHandler::BitTorrentHandler() {
    FALCON_LOG_INFO("BitTorrent handler initialized");
}

BitTorrentHandler::~BitTorrentHandler() {
    FALCON_LOG_DEBUG("BitTorrent handler shutdown");
}

std::vector<std::string> BitTorrentHandler::supported_schemes() const {
    return {"magnet", "bittorrent"};
}

bool BitTorrentHandler::can_handle(const std::string& url) const {
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

FileInfo BitTorrentHandler::get_file_info(const std::string& url,
                                          const DownloadOptions& options) {
    FileInfo info;
    info.url = url;
    info.supports_resume = true;

#ifdef FALCON_USE_LIBTORRENT
    try {
        if (url.find("magnet:") == 0) {
            // 解析 magnet URI
            auto params = libtorrent::parse_magnet_uri(url);
            info.filename = params.name;
            info.total_size = 0;  // magnet 链接不提供文件大小
        } else if (url.find(".torrent") != std::string::npos) {
            // 读取 torrent 文件
            std::string filePath = url;
            if (url.find("file://") == 0) {
                filePath = url.substr(7);
            }

            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open()) {
                throw FileNotFoundException("Failed to open torrent file: " + filePath);
            }

            std::string data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

            libtorrent::error_code ec;
            libtorrent::torrent_info ti(data, ec);
            if (ec) {
                throw ParseException("Failed to parse torrent: " + ec.message());
            }

            info.filename = ti.name();
            info.total_size = ti.total_size();

            // 获取最后修改时间（如果有）
            std::filesystem::path fsPath(filePath);
            if (std::filesystem::exists(fsPath)) {
                info.last_modified = std::filesystem::last_write_time(fsPath);
            }
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to get torrent file info: {}", e.what());
        throw;
    }
#else
    // 纯 C++ 模式：仅支持基本解析
    if (url.find(".torrent") != std::string::npos) {
        std::string filePath = url;
        if (url.find("file://") == 0) {
            filePath = url.substr(7);
        }

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            throw FileNotFoundException("Failed to open torrent file: " + filePath);
        }

        std::string data((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

        size_t pos = 0;
        BValue torrent = parseBencode(data, pos);

        if (validateTorrent(torrent)) {
            auto& infoDict = torrent.dictValue.at("info").dictValue;
            info.filename = infoDict.at("name").strValue;

            if (infoDict.find("length") != infoDict.end()) {
                info.total_size = infoDict.at("length").intValue;
            } else {
                // 多文件 torrent
                uint64_t total = 0;
                for (const auto& file : infoDict.at("files").listValue) {
                    total += file.dictValue.at("length").intValue;
                }
                info.total_size = total;
            }
        }
    }
#endif

    return info;
}

void BitTorrentHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
#ifdef FALCON_USE_LIBTORRENT
    // 使用 libtorrent 实现
    FALCON_LOG_INFO("Starting BitTorrent download: {}", task->url());

    try {
        libtorrent::add_torrent_params params;

        if (task->url().find("magnet:") == 0) {
            params = libtorrent::parse_magnet_uri(task->url());
        } else {
            // 处理 torrent 文件
            std::string filePath = task->url();
            if (task->url().find("file://") == 0) {
                filePath = task->url().substr(7);
            }

            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open()) {
                throw FileNotFoundException("Failed to open torrent file");
            }

            std::string data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

            libtorrent::error_code ec;
            params.ti = std::make_shared<libtorrent::torrent_info>(data, ec);
            if (ec) {
                throw ParseException("Failed to parse torrent: " + ec.message());
            }
        }

        // 设置保存路径
        std::string outputPath = task->options().output_directory;
        if (outputPath.empty()) {
            outputPath = "./downloads";
        }
        params.save_path = outputPath;

        // 添加 torrent 到 session
        libtorrent::torrent_handle handle = session_.add_torrent(params);

        // 设置任务为下载中状态
        task->mark_started();

        // 监控下载进度（简化实现）
        // 实际实现应该使用独立的监控线程
        FALCON_LOG_INFO("BitTorrent download started: {}", params.name);

    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to start BitTorrent download: {}", e.what());
        task->set_error(e.what());
        throw;
    }
#else
    FALCON_LOG_ERROR("BitTorrent download requires libtorrent support");
    task->set_error("BitTorrent support not compiled with libtorrent");
    throw NotSupportedException("BitTorrent download requires FALCON_USE_LIBTORRENT");
#endif
}

void BitTorrentHandler::pause(DownloadTask::Ptr task) {
#ifdef FALCON_USE_LIBTORRENT
    FALCON_LOG_DEBUG("Pausing BitTorrent download: {}", task->id());
    // libtorrent 实现需要维护任务句柄映射
    // 这里简化处理
#endif
}

void BitTorrentHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
#ifdef FALCON_USE_LIBTORRENT
    FALCON_LOG_DEBUG("Resuming BitTorrent download: {}", task->id());
    // libtorrent 实现需要维护任务句柄映射
    // 这里简化处理
#endif
}

void BitTorrentHandler::cancel(DownloadTask::Ptr task) {
#ifdef FALCON_USE_LIBTORRENT
    FALCON_LOG_DEBUG("Canceling BitTorrent download: {}", task->id());
    // libtorrent 实现需要维护任务句柄映射
    // 这里简化处理
#endif
}

// ============================================================================
// B 编码解析（简化实现）
// ============================================================================

BitTorrentHandler::BValue BitTorrentHandler::parseBencode(const std::string& data, size_t& pos) {
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
        if (pos >= data.length() || data[pos] != ':') {
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

std::string BitTorrentHandler::bencodeToString(const BValue& value) {
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
                BValue key;
                key.type = BValue::String;
                key.strValue = pair.first;
                ss << bencodeToString(key);
                ss << bencodeToString(pair.second);
            }
            ss << "e";
            break;
    }

    return ss.str();
}

std::string BitTorrentHandler::sha1(const std::string& data) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    ::SHA1(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }

    return ss.str();
}

std::string BitTorrentHandler::base32Decode(const std::string& input) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    static int decode_table[256] = {};

    static bool initialized = []() {
        std::fill(std::begin(decode_table), std::end(decode_table), -1);
        for (int i = 0; i < 32; ++i) {
            decode_table[static_cast<unsigned char>(alphabet[i])] = i;
        }
        return true;
    }();
    (void)initialized;

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

bool BitTorrentHandler::validateTorrent(const BValue& torrent) {
    if (torrent.type != BValue::Dict) {
        return false;
    }

    const auto& dict = torrent.dictValue;

    if (dict.find("info") == dict.end() || dict.at("info").type != BValue::Dict) {
        return false;
    }

    const auto& info = dict.at("info").dictValue;

    if (info.find("name") == info.end() || info.find("pieces") == info.end()) {
        return false;
    }

    if (info.find("length") == info.end() && info.find("files") == info.end()) {
        return false;
    }

    return true;
}

std::vector<std::string> BitTorrentHandler::getTrackers(const BValue& torrent) {
    std::vector<std::string> trackers;

    if (torrent.type != BValue::Dict) {
        return trackers;
    }

    const auto& dict = torrent.dictValue;

    if (dict.find("announce") != dict.end()) {
        const auto& announce = dict.at("announce");
        if (announce.type == BValue::String) {
            trackers.push_back(announce.strValue);
        }
    }

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

std::string BitTorrentHandler::generateNodeId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 255);

    std::string nodeId(20, 0);
    for (int i = 0; i < 20; ++i) {
        nodeId[i] = static_cast<char>(dis(gen));
    }

    return nodeId;
}

std::string BitTorrentHandler::urlDecode(const std::string& url) {
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

// ============================================================================
// Factory function
// ============================================================================

std::unique_ptr<IProtocolHandler> create_bittorrent_handler() {
    return std::make_unique<BitTorrentHandler>();
}

} // namespace protocols
} // namespace falcon
