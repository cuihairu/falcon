/**
 * @file bittorrent_plugin.cpp
 * @brief BitTorrent/Magnet 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "bittorrent_plugin.hpp"
#include "seed_policy.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <algorithm>

#ifdef FALCON_USE_LIBTORRENT
#include <libtorrent/bdecode.hpp>
#endif

namespace falcon {
namespace protocols {

// ============================================================================
// BitTorrentHandler 实现
// ============================================================================

BitTorrentHandler::BitTorrentHandler() {
    FALCON_LOG_INFO("BitTorrent handler initialized");

#ifndef FALCON_USE_LIBTORRENT
    // 如果启用了 DHT，自动启动
    if (dhtEnabled_) {
        startDht(dhtPort_);
    }
#endif
}

BitTorrentHandler::~BitTorrentHandler() {
    FALCON_LOG_DEBUG("BitTorrent handler shutdown");

#ifndef FALCON_USE_LIBTORRENT
    // 停止 DHT
    stopDht();

    // 清理 PEX 处理器
    std::lock_guard<std::mutex> lock(pexHandlersMutex_);
    pexHandlers_.clear();
#endif
}

std::vector<std::string> BitTorrentHandler::supported_schemes() const {
    return {"magnet", "bittorrent"};
}

std::string BitTorrentHandler::extract_info_hash(const std::string& url) {
    static const std::string kXtPrefix = "xt=urn:btih:";
    const size_t pos = url.find(kXtPrefix);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t start = pos + kXtPrefix.size();
    const size_t end = url.find_first_of("&#", start);
    return (end == std::string::npos) ? url.substr(start)
                                      : url.substr(start, end - start);
}

std::string BitTorrentHandler::info_hash_to_hex(const std::string& hash) {
    if (hash.size() == 40) {
        // 十六进制（大小写不敏感）：校验并小写归一
        std::string hex;
        hex.reserve(40);
        for (char c : hash) {
            const auto u = static_cast<unsigned char>(
                std::tolower(static_cast<unsigned char>(c)));
            if (!std::isxdigit(u)) {
                return {};
            }
            hex.push_back(static_cast<char>(u));
        }
        return hex;
    }

    if (hash.size() == 32) {
        // Base32（RFC 4648）：解码为原始 20 字节再转小写 hex
        const std::string raw = base32Decode(hash);
        if (raw.size() != 20) {
            return {};
        }
        static const char* kHexDigits = "0123456789abcdef";
        std::string hex;
        hex.reserve(40);
        for (char byte : raw) {
            const auto u = static_cast<unsigned char>(byte);
            hex.push_back(kHexDigits[u >> 4]);
            hex.push_back(kHexDigits[u & 0x0f]);
        }
        return hex;
    }

    return {};
}

namespace {

// 校验 magnet URI 中的 info-hash：40 位十六进制（BT v1）或 32 位 Base32
bool isValidInfoHash(const std::string& url, size_t hashBegin) {
    // hash 在 '&'（下一个参数）或 '#'（片段）或字符串结尾处结束
    size_t end = url.size();
    for (size_t i = hashBegin; i < end; ++i) {
        const char c = url[i];
        if (c == '&' || c == '#') {
            end = i;
            break;
        }
    }

    const size_t len = end - hashBegin;
    if (len == 40) {
        // 十六进制（大小写不敏感）
        for (size_t i = hashBegin; i < end; ++i) {
            if (!std::isxdigit(static_cast<unsigned char>(url[i]))) {
                return false;
            }
        }
        return true;
    }
    if (len == 32) {
        // Base32（RFC 4648：A-Z、2-7，大小写不敏感）
        for (size_t i = hashBegin; i < end; ++i) {
            const char c = static_cast<char>(
                std::toupper(static_cast<unsigned char>(url[i])));
            if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) {
                return false;
            }
        }
        return true;
    }
    return false;
}

} // anonymous namespace

bool BitTorrentHandler::can_handle(const std::string& url) const {
    // 检查 magnet 链接：scheme 大小写不敏感，且必须携带合法的 btih info-hash
    std::string lower;
    lower.reserve(url.size());
    for (char c : url) {
        lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower.rfind("magnet:", 0) == 0) {
        static const std::string kXtPrefix = "xt=urn:btih:";
        const size_t hashBegin = lower.find(kXtPrefix);
        if (hashBegin == std::string::npos) {
            return false;
        }
        return isValidInfoHash(url, hashBegin + kXtPrefix.size());
    }

    // 检查 .torrent 文件
    if (url.find(".torrent") != std::string::npos) {
        return true;
    }

    // 检查 bittorrent:// 链接（自定义协议）
    if (url.rfind("bittorrent://", 0) == 0) {
        return true;
    }

    return false;
}

FileInfo BitTorrentHandler::get_file_info(const std::string& url,
                                          [[maybe_unused]] const DownloadOptions& options) {
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
                throw FileIOException("Failed to open torrent file: " + filePath);
            }

            std::string data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

            libtorrent::error_code ec;
            // torrent_info(std::string, ec) 把参数当文件路径——内存数据
            // 须先 bdecode 再从 bdecode_node 构造
            libtorrent::bdecode_node node;
            libtorrent::bdecode(data.data(), data.data() + data.size(), node, ec);
            if (ec) {
                throw FileIOException("Failed to decode torrent: " + ec.message());
            }
            libtorrent::torrent_info ti(node, ec);
            if (ec) {
                throw FileIOException("Failed to parse torrent: " + ec.message());
            }

            info.filename = ti.name();
            info.total_size = static_cast<Bytes>(ti.total_size());
            // FileInfo::last_modified 是 steady_clock 时间点（HTTP 条件
            // 下载语义），文件 mtime（file_clock）不可转换——BT 路径
            // 不消费该字段，保持默认值
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
            throw FileIOException("Failed to open torrent file: " + filePath);
        }

        std::string data((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

        size_t pos = 0;
        BValue torrent = parseBencode(data, pos);

        if (validateTorrent(torrent)) {
            auto& infoDict = torrent.dictValue.at("info").dictValue;
            info.filename = infoDict.at("name").strValue;

            if (infoDict.find("length") != infoDict.end()) {
                info.total_size = static_cast<Bytes>(infoDict.at("length").intValue);
            } else {
                // 多文件 torrent
                uint64_t total = 0;
                for (const auto& file : infoDict.at("files").listValue) {
                    total += static_cast<uint64_t>(file.dictValue.at("length").intValue);
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
    // 进度通知经 task->update_progress 内部 listener 下发，形参不消费
    (void)listener;
    // libtorrent 数据面：worker 线程内阻塞监控（对齐 metalink 委托形态）。
    // 任务终态完全由本函数负责置位（TaskManager worker 对 download()
    // 返回后不做任何状态改写；异常由 worker catch 置 Failed）
    FALCON_LOG_INFO("Starting BitTorrent download: {}", task->url());

    try {
        // 句柄复用（resume 路径）：TaskManager 的 resume = Paused→Pending
        // 重排队后重新调 download()。PAUSED torrent 句柄保留在 session
        // 内（断点数据在磁盘与 session 缓存），复用续跑而非重新添加
        libtorrent::torrent_handle handle;
        bool reused = false;
        {
            std::lock_guard<std::mutex> handlesLock(handlesMutex_);
            auto it = torrentHandles_.find(task->id());
            if (it != torrentHandles_.end() && it->second.is_valid()) {
                handle = it->second;
                reused = true;
            }
        }

        if (reused) {
            handle.resume();
            FALCON_LOG_INFO("BitTorrent download resumed: {}", task->id());
        } else {
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
                    throw FileIOException("Failed to open torrent file");
                }

                std::string data((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());

                libtorrent::error_code ec;
                // 同 get_file_info：内存 bencode 须先 bdecode（string 构造
                // 重载按文件路径处理）
                libtorrent::bdecode_node node;
                libtorrent::bdecode(data.data(), data.data() + data.size(), node, ec);
                if (ec) {
                    throw FileIOException("Failed to decode torrent: " + ec.message());
                }
                params.ti = std::make_shared<libtorrent::torrent_info>(node, ec);
                if (ec) {
                    throw FileIOException("Failed to parse torrent: " + ec.message());
                }
            }

            // 设置保存路径
            std::string outputPath = task->options().output_directory;
            if (outputPath.empty()) {
                outputPath = "./downloads";
            }
            params.save_path = outputPath;

            handle = session_.add_torrent(params);

            // 保存句柄供 pause/resume/cancel 与监控循环使用
            {
                std::lock_guard<std::mutex> handlesLock(handlesMutex_);
                torrentHandles_[task->id()] = handle;
            }
        }

        task->mark_started();

        // peer 发现由 session 原生 DHT/LSD/PEX/tracker 驱动——自研
        // DhtClient/PexExtensionHandler 是纯 C++ 数据面的基础设施，
        // libtorrent 分支不使用

        // ===== 监控循环（aria2 BT 语义：任务完成 = 下载完成 + 做种策略
        // 满足；pause/cancel 经任务状态观测后在此收口）=====
        using bt::SeedLimits;
        using bt::SeedStats;
        using bt::seeding_complete;
        const auto& opts = task->options();
        const SeedLimits limits{opts.seed_ratio,
                                static_cast<double>(opts.seed_time_minutes)};
        std::chrono::steady_clock::time_point seeding_started{};
        bool seeding_observed = false;

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // 用户控制检查点：pause()/cancel() 只置任务状态，数据面冻结
            // 与清理在循环内收口（DownloadTask::pause 不代置状态）
            const auto status = task->status();
            if (status == TaskStatus::Cancelled) {
                // 清理已由 cancel() 完成，此处只退出循环
                return;
            }
            if (!handle.is_valid()) {
                // cancel() 的 remove_torrent 与状态置位之间的竞态窗口
                return;
            }
            if (status == TaskStatus::Paused) {
                handle.pause();
                FALCON_LOG_INFO("BitTorrent download paused: {}", task->id());
                // 句柄保留，resume 重排队后重新进入本函数
                return;
            }

            libtorrent::torrent_status st = handle.status();

            // torrent 级致命错误（存储错误等）→ throw 经 worker 置 Failed
            if (st.errc) {
                throw std::runtime_error("BitTorrent error: " + st.errc.message());
            }

            // magnet 元数据未到位时 total_wanted==0（0/0 piece，尚无
            // 内容可下）——此阶段不能当下载完成处理（做种策略面对
            // 0 字节任务没有可评估的量），必须等元数据到达后真有
            // 内容；total_wanted>0 同时为该时序提供防御
            const bool finished = st.is_finished && st.total_wanted > 0;
            if (finished && !seeding_observed) {
                seeding_observed = true;
                seeding_started = std::chrono::steady_clock::now();
                FALCON_LOG_INFO("BitTorrent download complete, seeding "
                                "(ratio={}, time={} min): {}",
                                limits.ratio, limits.time_minutes, task->id());
            }

            // 进度上报（update_progress 按 progress_interval_ms 节流下发；
            // magnet 元数据未到时 total_wanted==0，报 0/0 未知总量）
            task->update_progress(
                static_cast<Bytes>(st.total_payload_download),
                static_cast<Bytes>(st.total_wanted),
                static_cast<BytesPerSecond>(st.download_payload_rate));

            if (finished) {
                const auto seeded_seconds = seeding_observed
                    ? std::chrono::duration_cast<std::chrono::duration<double>>(
                          std::chrono::steady_clock::now() - seeding_started).count()
                    : 0.0;
                const SeedStats stats{static_cast<std::uint64_t>(st.total_payload_upload),
                                      static_cast<std::uint64_t>(st.total_payload_download),
                                      static_cast<std::uint64_t>(st.total_wanted),
                                      seeded_seconds};
                if (seeding_complete(limits, stats)) {
                    // 收口：移除 torrent（默认保留磁盘文件），任务 Completed。
                    // 首次观察即满足（ratio/time 均 0）= 下载完成立即停
                    remove_torrent(task->id());
                    // 终态进度穿透（total>0 时 update_progress 不节流）
                    task->update_progress(
                        static_cast<Bytes>(st.total_payload_download),
                        static_cast<Bytes>(st.total_wanted), 0);
                    task->set_status(TaskStatus::Completed);
                    FALCON_LOG_INFO("BitTorrent seeding finished "
                                    "(uploaded={}, target ratio={}, "
                                    "time={} min): {}",
                                    st.total_payload_upload, limits.ratio,
                                    limits.time_minutes, task->id());
                    return;
                }
            }
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("BitTorrent download failed: {}", e.what());
        task->set_error(e.what());
        throw;
    }
#else
    // 纯 C++ 实现 - 使用自定义 DHT 和 PEX
    FALCON_LOG_INFO("Starting BitTorrent download (native C++): {}", task->url());

    // 启动 DHT（如果未启动）
    if (!dhtClient_ && dhtEnabled_) {
        startDht(dhtPort_);
    }

    // 解析 info_hash：hex 原样归一，Base32 解码为原始 20 字节的十六进制
    // （DHT 查询按 40 位 hex 定位——Base32 文本直接下发会查询错误的 info_hash）
    std::string infoHash;
    if (task->url().find("magnet:") == 0) {
        infoHash = info_hash_to_hex(extract_info_hash(task->url()));
    }

    // 使用 DHT 查找 peers
    if (dhtClient_ && !infoHash.empty()) {
        FALCON_LOG_INFO("Starting DHT peer discovery for info_hash: {}", infoHash);

        // 创建任务上下文
        auto ctx = std::make_unique<TaskContext>();
        ctx->url = task->url();
        ctx->task = task;
        ctx->listener = listener;
        ctx->running.store(true);
        ctx->downloadedBytes = 0;
        ctx->totalSize = 0;

        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            activeTasks_[task->id()] = std::move(ctx);
        }

        dhtClient_->findPeers(infoHash,
            [this, task](const std::string& hash,
                        const std::vector<std::pair<std::string, uint16_t>>& peers) {
                FALCON_LOG_INFO("DHT found {} peers for {}", peers.size(), hash);

                // 将发现的 peers 添加到任务上下文
                std::lock_guard<std::mutex> lock(tasksMutex_);
                auto it = activeTasks_.find(task->id());
                if (it != activeTasks_.end()) {
                    for (const auto& peer : peers) {
                        PeerInfo peerInfo;
                        peerInfo.ip = peer.first;
                        peerInfo.port = peer.second;
                        it->second->peers.push_back(peerInfo);
                    }
                    FALCON_LOG_INFO("Added {} DHT peers to task {}", peers.size(), task->id());
                }
            });
    }

    task->mark_started();
    FALCON_LOG_INFO("BitTorrent download started: {}", task->url());
#endif
}

void BitTorrentHandler::pause(DownloadTask::Ptr task) {
    // TaskManager 不代置状态（DownloadTask::pause 也不代置）——handler
    // 首行自置 Paused；数据面冻结由 download 监控循环观测后收口
    task->set_status(TaskStatus::Paused);
#ifdef FALCON_USE_LIBTORRENT
    // 尽早转发冻结：torrent pause 后停止请求与上传分块；
    // 句柄移除与监控循环退出归循环内的 Paused 观测分支
    libtorrent::torrent_handle handle;
    {
        std::lock_guard<std::mutex> handlesLock(handlesMutex_);
        auto it = torrentHandles_.find(task->id());
        if (it != torrentHandles_.end() && it->second.is_valid()) {
            handle = it->second;
        }
    }
    if (handle.is_valid()) {
        handle.pause();
    }
    FALCON_LOG_INFO("BitTorrent download paused: {}", task->id());
#else
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(true);
    }
#endif
}

void BitTorrentHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    // TaskManager 的 resume = Paused→Pending 重排队后重新调 download()
    //（句柄复用续跑）；此处直接 download 是非 TaskManager 调用路径
    //（DownloadTask::resume）的对齐实现（metalink/http 同形态）
    download(std::move(task), listener);
}

void BitTorrentHandler::cancel(DownloadTask::Ptr task) {
    // 首行自置 Cancelled（幂等）——download 监控循环观测后退出；
    // 直接调 handler->cancel 的路径（不经 DownloadTask::cancel）同样收口
    task->set_status(TaskStatus::Cancelled);
#ifdef FALCON_USE_LIBTORRENT
    remove_torrent(task->id());  // 默认保留磁盘文件
    FALCON_LOG_INFO("BitTorrent download cancelled: {}", task->id());
#else
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->cancelled.store(true);
        it->second->running.store(false);

        // 清理 PEX 处理器
        // 需要从 TaskContext 获取 info_hash，这里简化处理
    }

    // 从活动任务中移除
    activeTasks_.erase(task->id());
    FALCON_LOG_INFO("BitTorrent download cancelled: {}", task->id());
#endif
}

#ifdef FALCON_USE_LIBTORRENT
void BitTorrentHandler::remove_torrent(TaskId id) {
    std::lock_guard<std::mutex> handlesLock(handlesMutex_);
    auto it = torrentHandles_.find(id);
    if (it != torrentHandles_.end()) {
        if (it->second.is_valid()) {
            // 默认 flags：保留磁盘文件与 .torrent 状态
            session_.remove_torrent(it->second);
        }
        torrentHandles_.erase(it);
    }
}
#endif

#ifdef FALCON_USE_LIBTORRENT
libtorrent::settings_pack BitTorrentHandler::make_session_settings() {
    libtorrent::settings_pack pack;
    pack.set_bool(libtorrent::settings_pack::enable_dht, true);
    pack.set_str(libtorrent::settings_pack::dht_bootstrap_nodes,
                 "dht.libtorrent.org:25401,"
                 "router.bittorrent.com:6881,"
                 "dht.transmissionbt.com:6881,"
                 "router.utorrent.com:6881");
    return pack;
}
#endif

void BitTorrentHandler::configure_private_mode() {
#ifdef FALCON_USE_LIBTORRENT
    libtorrent::settings_pack settings;
    settings.set_bool(libtorrent::settings_pack::enable_dht, false);
    settings.set_bool(libtorrent::settings_pack::enable_lsd, false);
    settings.set_bool(libtorrent::settings_pack::enable_upnp, false);
    settings.set_bool(libtorrent::settings_pack::enable_natpmp, false);
    session_.apply_settings(std::move(settings));
    FALCON_LOG_INFO("BitTorrent session private mode enabled "
                    "(DHT/LSD/UPnP/NAT-PMP off)");
#else
    stopDht();
    dhtEnabled_.store(false);
#endif
}

#ifdef FALCON_USE_LIBTORRENT
void BitTorrentHandler::set_listen_interfaces(const std::string& interfaces) {
    libtorrent::settings_pack settings;
    settings.set_str(libtorrent::settings_pack::listen_interfaces, interfaces);
    session_.apply_settings(std::move(settings));
}

std::uint64_t BitTorrentHandler::uploaded_bytes(TaskId id) const {
    std::lock_guard<std::mutex> handlesLock(handlesMutex_);
    auto it = torrentHandles_.find(id);
    if (it == torrentHandles_.end() || !it->second.is_valid()) {
        return 0;
    }
    // total_payload_upload 只含真实数据载荷（不含协议握手/元信息开销）
    return it->second.status().total_payload_upload;
}

std::uint64_t BitTorrentHandler::downloaded_bytes(TaskId id) const {
    std::lock_guard<std::mutex> handlesLock(handlesMutex_);
    auto it = torrentHandles_.find(id);
    if (it == torrentHandles_.end() || !it->second.is_valid()) {
        return 0;
    }
    return it->second.status().total_payload_download;
}
#endif

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
        if (pos >= data.length()) {
            throw std::runtime_error("Unterminated integer");  // 截断，缺 'e'
        }
        ++pos;  // 跳过 'e'

        // bencode 整数仅允许可选 '-' 前缀 + 数字（stoll 会宽松接受空白/'+'）
        bool valid = !num.empty();
        for (size_t i = 0; i < num.size() && valid; ++i) {
            if (i == 0 && num[0] == '-') continue;
            if (!std::isdigit(static_cast<unsigned char>(num[i]))) {
                valid = false;
            }
        }
        if (!valid) {
            throw std::runtime_error("Invalid integer format");
        }

        BValue value;
        value.type = BValue::Integer;
        try {
            value.intValue = std::stoll(num);
        } catch (const std::exception&) {
            throw std::runtime_error("Integer out of range");
        }
        return value;
    } else if (c == 'l') {
        // 列表
        ++pos;
        BValue value;
        value.type = BValue::List;
        while (pos < data.length() && data[pos] != 'e') {
            value.listValue.push_back(parseBencode(data, pos));
        }
        if (pos >= data.length()) {
            throw std::runtime_error("Unterminated list");  // 截断，缺 'e'
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
        if (pos >= data.length()) {
            throw std::runtime_error("Unterminated dictionary");  // 截断，缺 'e'
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
        // 大小写不敏感（magnet 中的 Base32 hash 允许小写，can_handle 同语义）
        const auto index = static_cast<unsigned char>(
            std::toupper(static_cast<unsigned char>(c)));
        int value = decode_table[index];
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

// ============================================================================
// DHT 集成
// ============================================================================

void BitTorrentHandler::startDht(uint16_t port) {
#ifdef FALCON_USE_LIBTORRENT
    // libtorrent 模式：DHT 由 session 管理（默认开启），
    // 关闭走 configure_private_mode()
    (void)port;
    FALCON_LOG_DEBUG("DHT lifecycle is managed by the libtorrent session");
#else
    if (dhtClient_) {
        FALCON_LOG_DEBUG("DHT client already running");
        return;
    }

    try {
        dhtClient_ = std::make_unique<DhtClient>(port);
        dhtClient_->start();
        // start() 遇端口占用等失败只记日志不抛异常——留着一个没在运行的
        // 客户端会让 isDhtRunning() 撒谎、findPeers 的查找无人驱动
        if (!dhtClient_->isRunning()) {
            FALCON_LOG_ERROR("Failed to start DHT client on port {}", port);
            dhtClient_.reset();
            return;
        }
        dhtPort_ = port;
        FALCON_LOG_INFO("DHT client started on port {}", port);
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to start DHT client: {}", e.what());
        dhtClient_.reset();
    }
#endif
}

void BitTorrentHandler::clearDhtBootstrapNodes() {
#ifndef FALCON_USE_LIBTORRENT
    if (dhtClient_) {
        dhtClient_->clear_bootstrap_nodes();
    }
#endif
}

void BitTorrentHandler::stopDht() {
#ifndef FALCON_USE_LIBTORRENT
    if (!dhtClient_) {
        return;
    }

    try {
        dhtClient_->stop();
        dhtClient_.reset();
        FALCON_LOG_INFO("DHT client stopped");
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to stop DHT client: {}", e.what());
    }
#endif
}

// ============================================================================
// PEX 集成（自研 PEX 表仅纯 C++ 模式维护；libtorrent 内建 PEX）
// ============================================================================

PexExtensionHandler* BitTorrentHandler::getPexHandler(const std::string& infoHash) {
#ifdef FALCON_USE_LIBTORRENT
    (void)infoHash;
    return nullptr;
#else
    std::lock_guard<std::mutex> lock(pexHandlersMutex_);
    auto it = pexHandlers_.find(infoHash);
    return (it != pexHandlers_.end()) ? it->second.get() : nullptr;
#endif
}

void BitTorrentHandler::removePexHandler(const std::string& infoHash) {
#ifdef FALCON_USE_LIBTORRENT
    (void)infoHash;
#else
    std::lock_guard<std::mutex> lock(pexHandlersMutex_);
    pexHandlers_.erase(infoHash);
    FALCON_LOG_DEBUG("Removed PEX handler for info_hash: {}", infoHash);
#endif
}

// ============================================================================
// Factory function
// ============================================================================

std::unique_ptr<IProtocolHandler> create_bittorrent_handler() {
    return std::make_unique<BitTorrentHandler>();
}

} // namespace protocols
} // namespace falcon
