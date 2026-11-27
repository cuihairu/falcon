// Falcon HTTP Handler - Implementation
// Copyright (c) 2025 Falcon Project

#include "http_handler.hpp"
#include <falcon/detail/injection.hpp>

#include <falcon/protocols/segment_downloader.hpp>
#include <falcon/protocols/fs_rename.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

// Use libcurl if available, otherwise provide a simple fallback
#ifdef FALCON_USE_CURL
#include <curl/curl.h>
#endif

namespace falcon {
namespace protocols {

// Helper to extract scheme from URL
static std::string get_scheme(const std::string& url) {
    auto pos = url.find("://");
    if (pos == std::string::npos) {
        return "";
    }
    std::string scheme = url.substr(0, pos);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return scheme;
}

#ifdef FALCON_USE_CURL

// CURL callback for writing data
static size_t write_callback(void* ptr, size_t size, size_t nmemb,
                              void* userdata) {
    auto* stream = static_cast<std::ofstream*>(userdata);
    size_t total = size * nmemb;
    stream->write(static_cast<const char*>(ptr), static_cast<std::streamsize>(total));
    return total;
}

// CURL callback for getting headers
struct HeaderData {
    std::string content_type;
    std::string filename;
    std::string etag;
    std::string last_modified;
    Bytes content_length = 0;
    bool accept_ranges = false;
};

// 剥掉头值两侧的空白与尾部 CR/LF（Content-Type 解析同款语义，双侧版）
static void trim_header_value(std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        value.clear();
        return;
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
}

/// 文件 mtime → RFC 7231 IMF-fixdate（"Wed, 21 Oct 2015 07:28:00 GMT"）。
/// 续传 If-Range 的 HTTP-date 形态验证器：临时/段文件是"之前落盘的旧
/// 内容"，其 mtime 就是旧内容的天然时间戳——跨会话恢复时 HEAD 探测
/// 到的是当前（新）内容的验证器，与临时数据不属同一代际（发给服务
/// 器必然匹配，防护失效），必须用这个 date 让服务器按 Last-Modified
/// 判代际。读取失败返回空串。HTTP-date 转换在 C++17 无 clock_cast，
/// 这里手工换算与固定英文名表（strftime 依赖 locale 不可用）。
static std::string http_date_from_last_write_time(const std::string& path) {
    std::error_code ec;
    const auto tp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return {};
    }
    namespace chrono = std::chrono;
    // duration_cast 显式转换：libc++(Apple) 的 system_clock::duration
    // 是 microseconds 而 file_clock 差值是 nanoseconds，隐式转换不成立
    const auto sys_tp = chrono::system_clock::now() +
                        chrono::duration_cast<chrono::system_clock::duration>(
                            tp - std::filesystem::file_time_type::clock::now());
    const std::time_t t = chrono::system_clock::to_time_t(sys_tp);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    static const char* const kWday[] = {"Sun", "Mon", "Tue",
                                        "Wed", "Thu", "Fri", "Sat"};
    static const char* const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  kWday[tm_buf.tm_wday], tm_buf.tm_mday, kMon[tm_buf.tm_mon],
                  tm_buf.tm_year + 1900, tm_buf.tm_hour, tm_buf.tm_min,
                  tm_buf.tm_sec);
    return buf;
}

static size_t header_callback(char* buffer, size_t size, size_t nitems,
                               void* userdata) {
    size_t total = size * nitems;
    auto* data = static_cast<HeaderData*>(userdata);
    std::string header(buffer, total);

    // Parse Content-Length
    if (header.find("Content-Length:") == 0 ||
        header.find("content-length:") == 0) {
        auto pos = header.find(':');
        if (pos != std::string::npos) {
            data->content_length =
                std::stoull(header.substr(pos + 1));
        }
    }

    // Parse Content-Type
    if (header.find("Content-Type:") == 0 ||
        header.find("content-type:") == 0) {
        auto pos = header.find(':');
        if (pos != std::string::npos) {
            data->content_type = header.substr(pos + 2);
            // Remove trailing whitespace
            while (!data->content_type.empty() &&
                   (data->content_type.back() == '\r' ||
                    data->content_type.back() == '\n' ||
                    data->content_type.back() == ' ')) {
                data->content_type.pop_back();
            }
        }
    }

    // Parse Accept-Ranges
    if (header.find("Accept-Ranges:") == 0 ||
        header.find("accept-ranges:") == 0) {
        if (header.find("bytes") != std::string::npos) {
            data->accept_ranges = true;
        }
    }

    // Parse ETag / Last-Modified（续传 If-Range 验证器，见 download_single）
    if (header.find("ETag:") == 0 || header.find("etag:") == 0) {
        auto pos = header.find(':');
        if (pos != std::string::npos) {
            data->etag = header.substr(pos + 1);
            trim_header_value(data->etag);
        }
    }
    if (header.find("Last-Modified:") == 0 ||
        header.find("last-modified:") == 0) {
        auto pos = header.find(':');
        if (pos != std::string::npos) {
            data->last_modified = header.substr(pos + 1);
            trim_header_value(data->last_modified);
        }
    }

    // Parse Content-Disposition for filename
    if (header.find("Content-Disposition:") == 0 ||
        header.find("content-disposition:") == 0) {
        auto pos = header.find("filename=");
        if (pos != std::string::npos) {
            auto start = pos + 9;
            if (header[start] == '"') {
                ++start;
                auto end = header.find('"', start);
                if (end != std::string::npos) {
                    data->filename = header.substr(start, end - start);
                }
            } else {
                auto end = header.find_first_of(";\r\n", start);
                data->filename = header.substr(
                    start, end == std::string::npos ? std::string::npos
                                                    : end - start);
            }
        }
    }

    return total;
}

// Progress callback data
struct ProgressData {
    DownloadTask::Ptr task;
    IEventListener* listener;
    std::atomic<bool>* cancelled;
    Bytes start_offset;
    std::chrono::steady_clock::time_point last_update;
    Bytes last_bytes;
    CURL* curl = nullptr;              // 运行时限速动态调整目标
    BytesPerSecond applied_limit = 0;  // 当前已生效的每连接限速
};

static int progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* data = static_cast<ProgressData*>(clientp);

    if (data->cancelled && data->cancelled->load()) {
        return 1;  // Abort transfer
    }

    if (data->task) {
        TaskStatus status = data->task->status();
        if (status == TaskStatus::Paused || status == TaskStatus::Cancelled) {
            return 1;  // Abort transfer
        }
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - data->last_update)
            .count();

    if (elapsed >= 200) {  // Update every 200ms
        Bytes current = data->start_offset + static_cast<Bytes>(dlnow);
        Bytes total =
            dltotal > 0 ? data->start_offset + static_cast<Bytes>(dltotal) : 0;

        // Calculate speed
        BytesPerSecond speed = 0;
        if (elapsed > 0) {
            Bytes bytes_since_last = current - data->last_bytes;
            speed = (bytes_since_last * 1000) / static_cast<Bytes>(elapsed);
        }

        data->task->update_progress(current, total, speed);

        // 运行时限速：每窗口查询一次（listener 侧综合全局/任务限制），
        // 变化时热应用——CURLOPT_MAX_RECV_SPEED_LARGE 支持传输中修改
        if (data->curl && data->listener && data->task) {
            const BytesPerSecond want =
                data->listener->query_speed_limit(data->task->id());
            if (want != data->applied_limit) {
                curl_easy_setopt(data->curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                                 static_cast<curl_off_t>(want));
                data->applied_limit = want;
            }
        }

        data->last_update = now;
        data->last_bytes = current;
    }

    return 0;
}

struct CurlHeaderList {
    curl_slist* list = nullptr;
    ~CurlHeaderList() {
        if (list) {
            curl_slist_free_all(list);
        }
    }
    CurlHeaderList(const CurlHeaderList&) = delete;
    CurlHeaderList& operator=(const CurlHeaderList&) = delete;
    CurlHeaderList() = default;
};

struct CurlAuthStrings {
    std::string userpwd;
    std::string proxy_userpwd;
};

static void apply_common_curl_options(CURL* curl,
                                      const DownloadOptions& options,
                                      bool enable_cookie_jar,
                                      CurlHeaderList& header_list,
                                      CurlAuthStrings& auth_strings) {
    if (!options.user_agent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, options.user_agent.c_str());
    }

    if (!options.proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, options.proxy.c_str());
    }

    if (!options.proxy_username.empty()) {
        auth_strings.proxy_userpwd = options.proxy_username + ":" + options.proxy_password;
        curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, auth_strings.proxy_userpwd.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    }

    if (!options.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    // 双向 TLS（mTLS）：客户端证书 + 私钥（PEM，aria2 --certificate/
    // --private-key 同语义）；两字段可指向同一复合 PEM 文件
    if (!options.client_certificate.empty()) {
        curl_easy_setopt(curl, CURLOPT_SSLCERT,
                         options.client_certificate.c_str());
        curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    }
    if (!options.client_private_key.empty()) {
        curl_easy_setopt(curl, CURLOPT_SSLKEY,
                         options.client_private_key.c_str());
        curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
    }

    if (!options.referer.empty()) {
        curl_easy_setopt(curl, CURLOPT_REFERER, options.referer.c_str());
    }

    if (!options.cookie_file.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, options.cookie_file.c_str());
    }

    if (enable_cookie_jar && !options.cookie_jar.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, options.cookie_jar.c_str());
    }

    if (!options.http_username.empty()) {
        auth_strings.userpwd = options.http_username + ":" + options.http_password;
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth_strings.userpwd.c_str());
    }

    for (const auto& pair : options.headers) {
        std::string header = pair.first + ": " + pair.second;
        header_list.list = curl_slist_append(header_list.list, header.c_str());
    }

    if (header_list.list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list.list);
    }
}

struct SegmentProgressData {
    std::atomic<bool>* cancelled = nullptr;
    // 段内实时进度接收器（SegmentDownloader 的 segment->downloaded）：
    // 每个进度回调把「起始时已有段文件尺寸 + 本次已传字节」relaxed
    // store 进去——监控线程 1s tick 汇总算任务级速度与进度。dlnow 是
    // curl 自己的记账（本次 easy handle 已收字节），不受 ofstream
    // streambuf 落盘时滞影响，正是显示层需要的进度源
    std::atomic<Bytes>* live_progress = nullptr;
    Bytes baseline = 0;
};

static int segment_progress_callback(void* clientp,
                                     curl_off_t /*dltotal*/,
                                     curl_off_t dlnow,
                                     curl_off_t /*ultotal*/,
                                     curl_off_t /*ulnow*/) {
    auto* data = static_cast<SegmentProgressData*>(clientp);
    if (data && data->cancelled && data->cancelled->load()) {
        return 1;
    }
    if (data && data->live_progress) {
        data->live_progress->store(
            data->baseline + static_cast<Bytes>(dlnow),
            std::memory_order_relaxed);
    }
    return 0;
}

// Download a single segment using CURL
static bool download_segment_curl(
    const std::string& url,
    Bytes start,
    Bytes end,
    const std::string& output_path,
    const DownloadOptions& options,
    const std::string& if_range,
    std::atomic<bool>& cancelled,
    std::atomic<Bytes>& live_progress) {

    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    // Open file for writing (append if resuming an existing segment)
    std::ios::openmode mode = std::ios::binary;
    std::uintmax_t existing_size = 0;
    {
        std::error_code ec;
        if (std::filesystem::exists(output_path, ec) && !ec) {
            auto sz = std::filesystem::file_size(output_path, ec);
            if (!ec && sz > 0) {
                existing_size = sz;
                mode |= std::ios::app;
            } else {
                mode |= std::ios::trunc;
            }
        } else {
            mode |= std::ios::trunc;
        }
    }

    std::ofstream file(output_path, mode);
    if (!file.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }

    // Configure CURL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // timeout_seconds 是停滞超时（aria2 --timeout 同语义）：低于
    // 1 B/s 持续该秒数才中止——绝不能映射 CURLOPT_TIMEOUT（整传输
    // 硬上限，默认 30s 会杀死一切慢而健康的真实下载，用户实测
    // 「HEAD 识别大小正常，30s 后必然失败」的 P0 根因）
    if (options.timeout_seconds > 0) {  // 0 = 关停滞看门狗（契约见 download_options.hpp）
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         static_cast<long>(options.timeout_seconds));
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    // Set Range header for segmented download
    std::string range_header = std::to_string(start) + "-" + std::to_string(end);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_header.c_str());

    CurlHeaderList header_list;
    CurlAuthStrings auth_strings;
    apply_common_curl_options(curl, options, /*enable_cookie_jar=*/false, header_list, auth_strings);

    // If-Range：段请求同样附加强验证器（全部段请求——新段与续传段
    // 都必须属于同一内容代际；不匹配时服务器回 200 全量，由下方
    // 206-required 门禁拒绝，段失败进入重试/换源逻辑，绝不把变更后
    // 的内容拼接进旧代际的段文件）。验证器按代际选择：续传段（段
    // 文件已有落盘进度，数据属于之前的会话）用段文件 mtime 的
    // HTTP-date——HEAD 探测到的是当前（新）内容的验证器，与旧段
    // 数据不属同一代际；新段（本会话创建）用 HEAD 验证器（ETag 优
    // 先，上层已拼好回退链）
    std::string validator = if_range;
    if (existing_size > 0) {
        validator = http_date_from_last_write_time(output_path);
    }
    if (!validator.empty()) {
        const std::string if_range_header = "If-Range: " + validator;
        header_list.list =
            curl_slist_append(header_list.list, if_range_header.c_str());
        if (header_list.list) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list.list);
        }
    }

    SegmentProgressData progress_data;
    progress_data.cancelled = &cancelled;
    progress_data.live_progress = &live_progress;
    progress_data.baseline = static_cast<Bytes>(existing_size);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, segment_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // Speed limit
    if (options.speed_limit > 0) {
        curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                         static_cast<curl_off_t>(options.speed_limit));
    }

    CURLcode res = curl_easy_perform(curl);

    file.close();

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && response_code >= 400) {
        FALCON_LOG_WARN("HTTP segment failed: status {} for range {}-{} of {}",
                        response_code, start, end, url);
        return false;
    }

    // start > 0 的段请求必须得到 206：服务器忽略 Range 会以 200 回传
    // 整个文件，追加进已有段文件即静默损坏——截回本次续传起点按失败
    // 收尾（重试机制接管），绝不把损坏数据留给 merge
    if (res == CURLE_OK && start > 0 && response_code != 206) {
        FALCON_LOG_WARN(
            "HTTP segment range ignored: status {} for range {}-{} of {}",
            response_code, start, end, url);
        if (existing_size > 0) {
            std::error_code resize_ec;
            std::filesystem::resize_file(output_path, existing_size, resize_ec);
        }
        return false;
    }

    if (res != CURLE_OK) {
        FALCON_LOG_WARN("HTTP segment curl error: {} for range {}-{} of {}",
                        curl_easy_strerror(res), start, end, url);
    }

    return res == CURLE_OK && !cancelled.load();
}

#endif  // FALCON_USE_CURL

} // namespace protocols
} // namespace falcon
