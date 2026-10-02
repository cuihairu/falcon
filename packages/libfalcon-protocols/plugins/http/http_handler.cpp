// Falcon HTTP Handler - Implementation
// Copyright (c) 2025 Falcon Project

#include "http_handler.hpp"
#include <falcon/detail/injection.hpp>

#include "v2_http_download_adapter.hpp"

#include <falcon/protocols/segment_downloader.hpp>
#include <falcon/protocols/v2_engine_host.hpp>
#include <falcon/exceptions.hpp>
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
/// 判代际。读取失败返回空串。实现镜像 request_group.cpp 的
/// conditional-get 同名 helper（C++17 无 clock_cast，同款换算与固定
/// 英文名表——strftime 依赖 locale 不可用）
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
        return false;
    }

    // start > 0 的段请求必须得到 206：服务器忽略 Range 会以 200 回传
    // 整个文件，追加进已有段文件即静默损坏——截回本次续传起点按失败
    // 收尾（重试机制接管），绝不把损坏数据留给 merge
    if (res == CURLE_OK && start > 0 && response_code != 206) {
        if (existing_size > 0) {
            std::error_code resize_ec;
            std::filesystem::resize_file(output_path, existing_size, resize_ec);
        }
        return false;
    }

    return res == CURLE_OK && !cancelled.load();
}

#endif  // FALCON_USE_CURL

namespace {

/// 终局清扫孤儿段文件（任务取消/删除路径）
/**
 * 暂停路径保留段断点（SegmentDownloader::cancel_preserve_segments），
 * 暂停后 download() 返回、map 条目被擦除——此时取消/删除任务没有任何
 * downloader 持有者，断点文件必须在这里补刀清扫，否则永久滞留。
 *
 * 只删 <output>.falcon.tmp.seg 前缀的普通文件：单连接续传文件
 * <output>.falcon.tmp 不匹配前缀（活跃单连接无 map 条目，误删即
 * 断点丢失）；目录绝不触碰——fs::remove 对空目录 = rmdir 语义，
 * 把占位目录当段文件删掉是 SegmentFileOccupied 教训的红线
 */
void sweep_orphan_segment_files(const std::string& output_path) {
    if (output_path.empty()) return;
    const std::filesystem::path out(output_path);
    const std::string prefix = out.filename().string() + ".falcon.tmp.seg";
    std::filesystem::path dir = out.parent_path();
    if (dir.empty()) dir = ".";

    std::error_code dir_ec;
    std::filesystem::directory_iterator it(dir, dir_ec);
    if (dir_ec) return;
    for (const auto& entry : it) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;  // 非段文件前缀
        std::error_code type_ec;
        if (entry.is_regular_file(type_ec)) {
            std::error_code rm_ec;
            std::filesystem::remove(entry.path(), rm_ec);
        }
    }
}

}  // namespace

class HttpHandler::Impl {
public:
    Impl() {
#ifdef FALCON_USE_CURL
        curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    }

    ~Impl() {
#ifdef FALCON_USE_CURL
        curl_global_cleanup();
#endif
    }

    [[nodiscard]] bool can_handle(const std::string& url) const {
        std::string scheme = get_scheme(url);
        return scheme == "http" || scheme == "https";
    }

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                          const DownloadOptions& options) {
        if (!can_handle(url)) {
            throw NetworkException("Invalid URL: " + url);
        }

        FileInfo info;
        info.url = url;

#ifdef FALCON_USE_CURL
        // 注入命中时短路真实调用，避免已创建句柄在 throw 路径泄漏
        CURL* curl = detail::inject_failure(detail::InjectPoint::CurlEasyInit)
                         ? nullptr
                         : curl_easy_init();
        if (!curl) {
            throw NetworkException("Failed to initialize CURL");
        }

        HeaderData header_data;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  // HEAD request
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_data);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(options.timeout_seconds));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        CurlHeaderList header_list;
        CurlAuthStrings auth_strings;
        apply_common_curl_options(curl, options, /*enable_cookie_jar=*/true, header_list, auth_strings);

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            throw NetworkException(std::string("CURL error: ") +
                                   curl_easy_strerror(res));
        }

        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

        curl_easy_cleanup(curl);

        if (response_code >= 400) {
            throw NetworkException("HTTP error: " + std::to_string(response_code));
        }

        info.total_size = header_data.content_length;
        info.content_type = header_data.content_type;
        info.supports_resume = header_data.accept_ranges;
        info.filename = header_data.filename;
        info.etag = header_data.etag;
        info.last_modified_header = header_data.last_modified;

        // Extract filename from URL if not in headers
        if (info.filename.empty()) {
            auto pos = url.rfind('/');
            if (pos != std::string::npos && pos < url.length() - 1) {
                info.filename = url.substr(pos + 1);
                // Remove query string
                auto query_pos = info.filename.find('?');
                if (query_pos != std::string::npos) {
                    info.filename = info.filename.substr(0, query_pos);
                }
            }
            if (info.filename.empty()) {
                info.filename = "download";
            }
        }
#else
        // Fallback without libcurl - just extract filename from URL
        auto pos = url.rfind('/');
        if (pos != std::string::npos && pos < url.length() - 1) {
            info.filename = url.substr(pos + 1);
            auto query_pos = info.filename.find('?');
            if (query_pos != std::string::npos) {
                info.filename = info.filename.substr(0, query_pos);
            }
        }
        if (info.filename.empty()) {
            info.filename = "download";
        }
        info.supports_resume = false;
#endif

        return info;
    }

    void download(DownloadTask::Ptr task, IEventListener* listener) {
#ifdef FALCON_USE_CURL
        const auto& options = task->options();

        // First, get file info to determine if we should use segmented download
        FileInfo info = get_file_info(task->url(), options);
        task->set_file_info(info);

        // Check if segmented download is beneficial
        bool use_segments = options.resume_enabled &&
                            info.supports_resume &&
                            info.total_size > static_cast<Bytes>(options.min_segment_size) &&
                            options.max_connections > 1;

        if (use_segments) {
            // Use multi-threaded segmented download
            download_segmented(task, listener, info);
        } else {
            // Use single connection download
            download_single(task, listener, info);
        }

#else
        // Without libcurl, we cannot download
        throw UnsupportedProtocolException(
            "HTTP downloads require libcurl. Please compile with FALCON_USE_CURL=ON");
#endif
    }

    void download_single(DownloadTask::Ptr task, IEventListener* listener, const FileInfo& info) {
#ifdef FALCON_USE_CURL
        const auto& options = task->options();
        std::string temp_path = task->output_path() + ".falcon.tmp";

        std::string last_error;
        for (std::size_t attempt = 0; attempt <= options.max_retries; ++attempt) {
            // Paused/Cancelled：既有静默收口语义（resume 不复位状态，
            // Paused 下直调 download 静默返回——恢复前置位 Downloading
            // 是 TaskManager 职责）。Completed/Failed：孤儿 attempt
            // 守卫——快速 pause→resume 会排队多个 worker，首个完成的
            // attempt 出成品后其余迟到 worker 若继续跑会整文件重下并
            // 把终态改写。TaskManager::start_task 对终态任务入口拒绝，
            // 故终态下到达这里只可能是孤儿（与分段路径同形状收口）
            const auto attempt_status = task->status();
            if (attempt_status == TaskStatus::Paused ||
                attempt_status == TaskStatus::Cancelled ||
                attempt_status == TaskStatus::Completed ||
                attempt_status == TaskStatus::Failed) {
                return;
            }

            // Check for existing partial download
            Bytes start_offset = 0;
            if (options.resume_enabled) {
                std::ifstream test(temp_path, std::ios::binary | std::ios::ate);
                if (test.is_open()) {
                    start_offset = static_cast<Bytes>(test.tellg());
                    test.close();
                }
            }

            // Open file for writing
            std::ofstream file;
            if (start_offset > 0) {
                file.open(temp_path, std::ios::binary | std::ios::app);
            } else {
                file.open(temp_path, std::ios::binary | std::ios::trunc);
            }

            if (!file.is_open()) {
                throw FileIOException("Failed to open file: " + temp_path);
            }

            // 注入命中时短路真实调用，避免已创建句柄在 throw 路径泄漏
            CURL* curl =
                detail::inject_failure(detail::InjectPoint::CurlEasyInit)
                    ? nullptr
                    : curl_easy_init();
            if (!curl) {
                throw NetworkException("Failed to initialize CURL");
            }

            std::atomic<bool> cancelled{false};
            ProgressData progress_data;
            progress_data.task = task;
            progress_data.listener = listener;
            progress_data.cancelled = &cancelled;
            progress_data.start_offset = start_offset;
            progress_data.last_update = std::chrono::steady_clock::now();
            progress_data.last_bytes = start_offset;

            curl_easy_setopt(curl, CURLOPT_URL, task->url().c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            // 停滞超时（同上）：慢而健康的传输不受总时长惩罚
            if (options.timeout_seconds > 0) {
                curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
                curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                                 static_cast<long>(options.timeout_seconds));
            }
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

            CurlHeaderList header_list;
            CurlAuthStrings auth_strings;
            apply_common_curl_options(curl, options, /*enable_cookie_jar=*/true, header_list, auth_strings);

            // Resume support
            bool sent_if_range = false;
            if (start_offset > 0) {
                curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                                 static_cast<curl_off_t>(start_offset));
                // If-Range（RFC 7233 §3.2）：带断点续传时附加强验证器，
                // 资源在传输间隙被变更时服务器应回 200 全量而非 206——
                // 客户端据此重启而非把新内容拼接在旧前缀上（静默污染）。
                // 验证器按代际选择：attempt 0 的临时文件是之前会话写
                // 的旧内容，而本次 HEAD 探测到的是当前（新）内容的验
                // 证器——发它必然匹配，防护失效（走查 W3 实锤形态）；
                // 必须用临时文件 mtime 的 HTTP-date（旧内容落盘时刻）
                // 让服务器按 Last-Modified 判代际。attempt ≥ 1 的会话
                // 内重试窗口恰是 HEAD 保护的对象——ETag 优先（强验证
                // 器），缺失回落 Last-Modified，再回落 mtime date
                std::string validator;
                if (attempt == 0) {
                    validator = http_date_from_last_write_time(temp_path);
                } else if (!info.etag.empty()) {
                    validator = info.etag;
                } else if (!info.last_modified_header.empty()) {
                    validator = info.last_modified_header;
                } else {
                    validator = http_date_from_last_write_time(temp_path);
                }
                if (!validator.empty()) {
                    const std::string if_range = "If-Range: " + validator;
                    header_list.list = curl_slist_append(header_list.list,
                                                         if_range.c_str());
                    if (header_list.list) {
                        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,
                                         header_list.list);
                        sent_if_range = true;
                    }
                }
            }

            // Speed limit：listener 查询优先（综合引擎全局/任务限速），
            // handler 独立使用（无 listener）时回落任务自身限制
            BytesPerSecond effective_limit = options.speed_limit;
            if (listener) {
                effective_limit = listener->query_speed_limit(task->id());
            }
            progress_data.curl = curl;
            progress_data.applied_limit = effective_limit;
            if (effective_limit > 0) {
                curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                                 static_cast<curl_off_t>(effective_limit));
            }

            CURLcode res = curl_easy_perform(curl);

            file.close();

            if (res == CURLE_ABORTED_BY_CALLBACK) {
                curl_easy_cleanup(curl);
                return;
            }

            if (res != CURLE_OK) {
                last_error = curl_easy_strerror(res);
                // If-Range 已附带且 curl 报 RANGE_ERROR：续传被拒的
                // 典型形态是资源已变更（If-Range 不匹配 → 200 全量 +
                // RESUME_FROM 冲突）。断点数据属于旧内容，绝不接续
                // ——清空临时文件，下次尝试走全新下载（成品 = 新资源）
                if (res == CURLE_RANGE_ERROR && sent_if_range) {
                    std::error_code resize_ec;
                    std::filesystem::resize_file(temp_path, 0, resize_ec);
                }
                curl_easy_cleanup(curl);
            } else {
                long response_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
                curl_easy_cleanup(curl);

                if (response_code >= 400) {
                    last_error = "HTTP error: " + std::to_string(response_code);
                    if (response_code < 500) {
                        throw NetworkException(last_error);
                    }
                } else if (start_offset > 0 && response_code != 206) {
                    // 续传请求被以 200 应答（Range 被忽略，服务器把整个
                    // 文件传回）：追加进临时文件即静默损坏——清空临时
                    // 文件从头重下（下次尝试自然走完整下载路径），一次
                    // 有界的浪费尝试优于损坏的成品
                    last_error =
                        "Server ignored Range request (200 instead of 206)";
                    std::error_code resize_ec;
                    std::filesystem::resize_file(temp_path, 0, resize_ec);
                    if (resize_ec) {
                        throw FileIOException(
                            "Failed to reset temp file after ignored Range: " +
                            temp_path);
                    }
                } else {
                    // Move temp file to final destination
                    if (std::rename(temp_path.c_str(), task->output_path().c_str()) != 0) {
                        throw FileIOException("Failed to move downloaded file to destination");
                    }
                    // 终态进度记账：快速下载全程落在进度回调 200ms 节流
                    // 窗外时 update_progress 一次都没触发过——按成品尺寸
                    // 补记，避免"已完成 0%"的假进度（终态更新穿透节流）。
                    // 未知总长（EOF/chunked 定界）跳过：total 未报告过就
                    // 不发明一个（downloaded 由写回调即时记账，本就准确）
                    std::error_code size_ec;
                    const auto final_size =
                        std::filesystem::file_size(task->output_path(), size_ec);
                    if (!size_ec && task->total_bytes() > 0) {
                        task->update_progress(final_size, task->total_bytes(), 0);
                    }
                    task->set_status(TaskStatus::Completed);
                    return;
                }
            }

            if (attempt >= options.max_retries) {
                throw NetworkException(last_error.empty() ? "Download failed" : last_error);
            }

            if (task->status() == TaskStatus::Paused || task->status() == TaskStatus::Cancelled) {
                return;
            }

            if (options.retry_delay_seconds > 0) {
                std::size_t backoff = options.retry_delay_seconds;
                backoff *= (std::size_t{1} << attempt);
                std::this_thread::sleep_for(std::chrono::seconds(backoff));
            }
        }

#endif  // FALCON_USE_CURL
    }

    void download_segmented(DownloadTask::Ptr task, IEventListener* listener, const FileInfo& info) {
#ifdef FALCON_USE_CURL
        const auto& options = task->options();

        // Configure segment downloader
        SegmentConfig seg_config;
        seg_config.num_connections = options.max_connections;
        seg_config.min_segment_size = options.min_segment_size;
        seg_config.min_file_size = options.min_segment_size;
        seg_config.timeout_seconds = options.timeout_seconds;
        seg_config.max_retries = options.max_retries;
        seg_config.retry_delay_ms = options.retry_delay_seconds * 1000;
        seg_config.adaptive_sizing = options.adaptive_segment_sizing;

        // 段下载限速：把任务限速（综合引擎全局/任务限制）均摊到各连接。
        // 段是短生命周期连接，启动时静态分摊即可（download_segment_curl
        // 消费 options.speed_limit）
        DownloadOptions seg_options = options;
        if (listener) {
            const BytesPerSecond task_limit =
                listener->query_speed_limit(task->id());
            if (task_limit > 0) {
                const std::size_t conns =
                    std::max<std::size_t>(1, options.max_connections);
                seg_options.speed_limit = task_limit / conns;
            }
        }

        // Create segment downloader
        auto downloader = std::make_shared<SegmentDownloader>(
            task,
            task->url(),
            task->output_path(),
            seg_config
        );

        downloader->set_event_listener(listener);

        // Register for external cancellation (pause/cancel)
        //
        // 注册前有界等待同 id 旧实例退出：pause→resume 抢跑竞态下，
        // 旧 attempt 的 worker 尚未 join 完成时新 attempt 已启动——
        // 两者会并行写同一段文件（同 offset 同内容，损坏风险低但
        // 并存）。以表内条目消失为旧实例收口信号，5s 上限（旧实例
        // 的 abort 汇合是亚秒级，超限放行——宁可少量并存也不阻塞）
        {
            int stale_wait_ms = 0;
            for (;;) {
                std::shared_ptr<SegmentDownloader> stale;
                {
                    std::lock_guard<std::mutex> lock(downloads_mutex_);
                    auto it = active_segmented_downloads_.find(task->id());
                    if (it == active_segmented_downloads_.end()) {
                        active_segmented_downloads_[task->id()] = downloader;
                        break;
                    }
                    stale = it->second;
                }
                if (++stale_wait_ms > 500) {  // 500 × 10ms = 5s 上限
                    std::lock_guard<std::mutex> lock(downloads_mutex_);
                    active_segmented_downloads_[task->id()] = downloader;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                (void)stale;
            }
        }
        // 只擦自己注册的条目（指针比较）：pause 后本尝试返回、resume
        // 建立的新 attempt 已按同 id 重登记，迟到的旧 attempt 无条件
        // erase(id) 会把新条目抹掉——之后的 pause/cancel 找不到
        // downloader，段连接失去中止通道。owned 引用同时把 downloader
        // 析构推迟到 guard 成员析构（erase 的锁已释放）
        struct EraseGuard {
            std::mutex* m;
            std::unordered_map<TaskId, std::shared_ptr<SegmentDownloader>>* map;
            TaskId id;
            std::shared_ptr<SegmentDownloader> owned;
            ~EraseGuard() {
                if (!m || !map) return;
                std::lock_guard<std::mutex> lock(*m);
                auto it = map->find(id);
                if (it != map->end() && it->second == owned) {
                    map->erase(it);
                }
            }
        } erase_guard{&downloads_mutex_, &active_segmented_downloads_, task->id(), downloader};

        // 等待旧实例退出期间任务可能已被再次暂停/取消（彼时表内无本
        // 实例条目，pause/cancel 转发不到），也可能已由其他 attempt
        // 完成——快速 pause→resume 会排队多个 worker，首个完成的
        // attempt 出成品后其余 attempt 是孤儿，继续跑会整文件重下并
        // 把已完成任务改写成 Failed。start 前复查终态：孤儿 attempt
        // 见到的任务必然已终态（TaskManager::start_task 对活动/终态
        // 任务拒绝入队，Pending/Preparing/Downloading 均不可能是孤
        // 儿形态；直调 handler 的测试路径任务恒 Pending，同放行）
        {
            const auto st = task->status();
            if (st == TaskStatus::Paused || st == TaskStatus::Cancelled ||
                st == TaskStatus::Completed || st == TaskStatus::Failed) {
                return;
            }
        }

        // Start segmented download
        // If-Range 验证器捕获（ETag 优先，缺失回落 Last-Modified）：
        // 段下载的多条连接可能跨越传输间隙，全部段请求绑定同一内容
        // 代际（见 download_segment_curl 的 206-required 门禁）；续传
        // 段在 download_segment_curl 内改用段文件 mtime 的 HTTP-date
        // （旧段数据属于之前的会话，HEAD 验证器是当前内容——代际错
        // 配，发它必然匹配等于无防护）
        const std::string& if_range_validator =
            !info.etag.empty() ? info.etag : info.last_modified_header;
        bool success = downloader->start([&](const std::string& url,
                                             Bytes start,
                                             Bytes end,
                                             const std::string& output_path,
                                             std::atomic<bool>& cancelled,
                                             std::atomic<Bytes>& live_progress) -> bool {
            return download_segment_curl(url, start, end, output_path, seg_options,
                                         if_range_validator, cancelled,
                                         live_progress);
        });

        // 退出归类以 SegmentDownloader 自身的 failed_ 标志为失败权
        // 威——任务状态不可作归类依据：resume 抢跑竞态下旧实例退出
        // 时任务已被并发置回 Downloading（B11「点继续后任务立即
        // Failed」根因：按任务状态归类把正常暂停退出误判为失败）
        if (success) {
            // 终态进度记账（同 download_single：传输中的段级进度经
            // live_progress 汇入监控线程的周期 update_progress，但 1s
            // tick 粒度下最后一窗可能落在终态分支——成品尺寸补记消
            // 除尾差；未知总长跳过——total 未报告过就不发明一个）
            std::error_code size_ec;
            const auto final_size =
                std::filesystem::file_size(task->output_path(), size_ec);
            if (!size_ec && task->total_bytes() > 0) {
                task->update_progress(final_size, task->total_bytes(), 0);
            }
            task->set_status(TaskStatus::Completed);
        } else if (downloader->was_failed()) {
            throw FileIOException("Segmented download failed");
        } else {
            // 用户停止（pause/cancel）的正常收口，非失败语义——任务
            // 状态归 pause/resume 调用方所有，此处不触碰
            return;
        }
#endif  // FALCON_USE_CURL
    }

    void pause(DownloadTask::Ptr task) {
        task->set_status(TaskStatus::Paused);

        std::shared_ptr<SegmentDownloader> downloader;
        {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            auto it = active_segmented_downloads_.find(task->id());
            if (it != active_segmented_downloads_.end()) {
                downloader = it->second;
            }
        }
        if (downloader) {
            // 暂停 = 中止在途连接但保留段断点：preserve 标志先置位，
            // 析构跳过段文件清理，resume 的恢复检测从断点续传（进度
            // 从暂停点接着走）。此前走 cancel()（析构清段文件），
            // resume 只能从 0 重下，且旧 attempt 迟到析构会删掉新
            // attempt 正在写的段文件 → 后续尝试全线失败
            downloader->cancel_preserve_segments();
        }
    }

    void resume(DownloadTask::Ptr task, IEventListener* listener) {
        // Resume is handled by download() checking for existing partial file
        download(task, listener);
    }

    void cancel(DownloadTask::Ptr task) {
        task->set_status(TaskStatus::Cancelled);

        std::shared_ptr<SegmentDownloader> downloader;
        {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            auto it = active_segmented_downloads_.find(task->id());
            if (it != active_segmented_downloads_.end()) {
                downloader = it->second;
            }
        }
        if (downloader) {
            // 先停在途写（join 返回后段文件不再被写），再清扫
            downloader->cancel();
        }
        // 终局清扫孤儿段文件：暂停后 download() 已返回、EraseGuard
        // 擦掉了 map 条目（preserve=true 的析构又跳过清理）——取消/
        // 删除暂停态任务必须在这里补刀，否则断点文件永久滞留。
        // 条目不存在 ⇒ 该任务没有活跃段下载者，扫描无误删风险
        // （单连接 <output>.falcon.tmp 不匹配段前缀，不在此列）
        sweep_orphan_segment_files(task->output_path());
    }

private:
    std::mutex downloads_mutex_;
    std::unordered_map<TaskId, std::shared_ptr<SegmentDownloader>> active_segmented_downloads_;
};

HttpHandler::HttpHandler() : impl_(std::make_unique<Impl>()) {}

HttpHandler::~HttpHandler() = default;

bool HttpHandler::can_handle(const std::string& url) const {
    return impl_->can_handle(url);
}

FileInfo HttpHandler::get_file_info(const std::string& url,
                                     const DownloadOptions& options) {
    return impl_->get_file_info(url, options);
}

void HttpHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    // V2 数据面分叉（默认关，逐任务可回退 curl）：开关开启且 options
    // 无 curl 专属能力时桥接到共享 V2 引擎
    if (V2HttpDownloadAdapter::supports(task->options())) {
        // 保 V1 事件序列：on_file_info 先于任何 on_progress
        const FileInfo info = impl_->get_file_info(task->url(), task->options());
        task->set_file_info(info);
        V2HttpDownloadAdapter(task).run();
        return;
    }
    impl_->download(task, listener);
}

void HttpHandler::pause(DownloadTask::Ptr task) {
    if (!task) return;  // 与 FtpHandler 同款防御：空任务无操作
    impl_->pause(task);
    // V2 组同步暂停（V1 任务无 V2 组时引擎侧幂等返回 false）
    if (auto engine = V2EngineHost::instance().try_engine()) {
        engine->pause_task(task->id());
    }
}

void HttpHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    if (!task) return;
    impl_->resume(task, listener);
}

void HttpHandler::cancel(DownloadTask::Ptr task) {
    if (!task) return;
    impl_->cancel(task);
    // V2 组同步取消（桥接轮询亦会兜底转发，幂等）
    if (auto engine = V2EngineHost::instance().try_engine()) {
        engine->cancel_task(task->id());
    }
}

std::unique_ptr<IProtocolHandler> create_http_handler() {
    return std::make_unique<HttpHandler>();
}

}  // namespace protocols
}  // namespace falcon
