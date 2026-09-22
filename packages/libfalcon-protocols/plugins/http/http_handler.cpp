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
    Bytes content_length = 0;
    bool accept_ranges = false;
};

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
};

static int segment_progress_callback(void* clientp,
                                     curl_off_t /*dltotal*/,
                                     curl_off_t /*dlnow*/,
                                     curl_off_t /*ultotal*/,
                                     curl_off_t /*ulnow*/) {
    auto* data = static_cast<SegmentProgressData*>(clientp);
    if (data && data->cancelled && data->cancelled->load()) {
        return 1;
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
    std::atomic<bool>& cancelled) {

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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(options.timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    // Set Range header for segmented download
    std::string range_header = std::to_string(start) + "-" + std::to_string(end);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_header.c_str());

    CurlHeaderList header_list;
    CurlAuthStrings auth_strings;
    apply_common_curl_options(curl, options, /*enable_cookie_jar=*/false, header_list, auth_strings);

    SegmentProgressData progress_data;
    progress_data.cancelled = &cancelled;
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

    void download_single(DownloadTask::Ptr task, IEventListener* listener, const FileInfo& /*info*/) {
#ifdef FALCON_USE_CURL
        const auto& options = task->options();
        std::string temp_path = task->output_path() + ".falcon.tmp";

        std::string last_error;
        for (std::size_t attempt = 0; attempt <= options.max_retries; ++attempt) {
            if (task->status() == TaskStatus::Paused || task->status() == TaskStatus::Cancelled) {
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
            curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                             static_cast<long>(options.timeout_seconds));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

            CurlHeaderList header_list;
            CurlAuthStrings auth_strings;
            apply_common_curl_options(curl, options, /*enable_cookie_jar=*/true, header_list, auth_strings);

            // Resume support
            if (start_offset > 0) {
                curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                                 static_cast<curl_off_t>(start_offset));
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

    void download_segmented(DownloadTask::Ptr task, IEventListener* listener, const FileInfo& /*info*/) {
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
        {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            active_segmented_downloads_[task->id()] = downloader;
        }
        struct EraseGuard {
            std::mutex* m;
            std::unordered_map<TaskId, std::shared_ptr<SegmentDownloader>>* map;
            TaskId id;
            ~EraseGuard() {
                if (!m || !map) return;
                std::lock_guard<std::mutex> lock(*m);
                map->erase(id);
            }
        } erase_guard{&downloads_mutex_, &active_segmented_downloads_, task->id()};

        // Start segmented download
        bool success = downloader->start([&](const std::string& url,
                                             Bytes start,
                                             Bytes end,
                                             const std::string& output_path,
                                             std::atomic<bool>& cancelled) -> bool {
            return download_segment_curl(url, start, end, output_path, seg_options, cancelled);
        });

        if (task->status() == TaskStatus::Paused || task->status() == TaskStatus::Cancelled) {
            return;
        }

        if (success) {
            // 终态进度记账（同 download_single：段路径的段级回调不汇入
            // 任务级 update_progress，成品尺寸是唯一可信的完成进度；
            // 未知总长跳过——total 未报告过就不发明一个）
            std::error_code size_ec;
            const auto final_size =
                std::filesystem::file_size(task->output_path(), size_ec);
            if (!size_ec && task->total_bytes() > 0) {
                task->update_progress(final_size, task->total_bytes(), 0);
            }
            task->set_status(TaskStatus::Completed);
        } else {
            throw FileIOException("Segmented download failed");
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
            downloader->cancel();
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
            downloader->cancel();
        }
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
