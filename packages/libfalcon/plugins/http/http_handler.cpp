// Falcon HTTP Handler - Implementation
// Copyright (c) 2025 Falcon Project

#include "http_handler.hpp"

#include <falcon/segment_downloader.hpp>
#include <falcon/exceptions.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <iostream>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>

// Use libcurl if available, otherwise provide a simple fallback
#ifdef FALCON_USE_CURL
#include <curl/curl.h>
#endif

namespace falcon {
namespace plugins {

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
};

static int progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* data = static_cast<ProgressData*>(clientp);

    if (data->cancelled && data->cancelled->load()) {
        return 1;  // Abort transfer
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

        ProgressInfo progress;
        progress.task_id = data->task->id();
        progress.downloaded_bytes = current;
        progress.total_bytes = total;
        progress.speed = speed;

        data->task->update_progress(current, total, speed);

        // Dispatch event
        if (data->listener) {
            data->listener->on_progress(progress);
        }

        data->last_update = now;
        data->last_bytes = current;
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

    // Open file for writing
    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
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

    // Set Range header for segmented download
    std::string range_header = std::to_string(start) + "-" + std::to_string(end);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_header.c_str());

    if (!options.user_agent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, options.user_agent.c_str());
    }

    if (!options.proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, options.proxy.c_str());
    }

    if (!options.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    // Speed limit
    if (options.speed_limit > 0) {
        curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                         static_cast<curl_off_t>(options.speed_limit));
    }

    // Custom headers
    struct curl_slist* headers = nullptr;
    for (const auto& pair : options.headers) {
        std::string header = pair.first + ": " + pair.second;
        headers = curl_slist_append(headers, header.c_str());
    }
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode res = curl_easy_perform(curl);

    if (headers) {
        curl_slist_free_all(headers);
    }

    file.close();
    curl_easy_cleanup(curl);

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
        CURL* curl = curl_easy_init();
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

        if (!options.user_agent.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERAGENT, options.user_agent.c_str());
        }

        if (!options.proxy.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, options.proxy.c_str());
        }

        if (!options.verify_ssl) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        // Add custom headers
        struct curl_slist* headers = nullptr;
        for (const auto& pair : options.headers) {
            std::string header = pair.first + ": " + pair.second;
            headers = curl_slist_append(headers, header.c_str());
        }
        if (headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        CURLcode res = curl_easy_perform(curl);

        if (headers) {
            curl_slist_free_all(headers);
        }

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

        // Check if segmented download is beneficial
        bool use_segments = info.supports_resume &&
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

        CURL* curl = curl_easy_init();
        if (!curl) {
            throw NetworkException("Failed to initialize CURL");
        }

        std::atomic<bool> cancelled{false};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_tasks_.insert(task->id());
            cancelled_tasks_.erase(task->id());
        }

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
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        if (!options.user_agent.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERAGENT, options.user_agent.c_str());
        }

        if (!options.proxy.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, options.proxy.c_str());
        }

        if (!options.verify_ssl) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        // Resume support
        if (start_offset > 0) {
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                             static_cast<curl_off_t>(start_offset));
        }

        // Speed limit
        if (options.speed_limit > 0) {
            curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                             static_cast<curl_off_t>(options.speed_limit));
        }

        // Custom headers
        struct curl_slist* headers = nullptr;
        for (const auto& pair : options.headers) {
            std::string header = pair.first + ": " + pair.second;
            headers = curl_slist_append(headers, header.c_str());
        }
        if (headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        CURLcode res = curl_easy_perform(curl);

        if (headers) {
            curl_slist_free_all(headers);
        }

        file.close();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_tasks_.erase(task->id());
        }

        if (res == CURLE_ABORTED_BY_CALLBACK) {
            task->set_status(TaskStatus::Cancelled);
            curl_easy_cleanup(curl);
            return;
        }

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

        // Move temp file to final destination
        if (std::rename(temp_path.c_str(), task->output_path().c_str()) != 0) {
            throw FileIOException("Failed to move downloaded file to destination");
        }
        task->set_status(TaskStatus::Completed);

#endif  // FALCON_USE_CURL
    }

    void download_segmented(DownloadTask::Ptr task, IEventListener* listener, const FileInfo& info) {
#ifdef FALCON_USE_CURL
        const auto& options = task->options();

        // Configure segment downloader
        SegmentConfig seg_config;
        seg_config.num_connections = options.max_connections;
        seg_config.min_segment_size = options.min_segment_size;
        seg_config.timeout_seconds = options.timeout_seconds;
        seg_config.max_retries = options.max_retries;

        // Create segment downloader
        SegmentDownloader downloader(
            task,
            task->url(),
            task->output_path(),
            seg_config
        );

        downloader.set_event_listener(listener);

        // Start segmented download
        bool success = downloader.start([&](const std::string& url,
                                            Bytes start,
                                            Bytes end,
                                            const std::string& output_path,
                                            std::atomic<bool>& cancelled) -> bool {
            return download_segment_curl(url, start, end, output_path, options, cancelled);
        });

        if (success) {
            task->set_status(TaskStatus::Completed);
        } else {
            throw FileIOException("Segmented download failed");
        }
#endif  // FALCON_USE_CURL
    }

    void pause(DownloadTask::Ptr task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_tasks_.count(task->id())) {
            cancelled_tasks_.insert(task->id());
        }
        task->set_status(TaskStatus::Paused);
    }

    void resume(DownloadTask::Ptr task, IEventListener* listener) {
        // Resume is handled by download() checking for existing partial file
        download(task, listener);
    }

    void cancel(DownloadTask::Ptr task) {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_tasks_.insert(task->id());
    }

private:
    std::mutex mutex_;
    std::unordered_set<TaskId> active_tasks_;
    std::unordered_set<TaskId> cancelled_tasks_;
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
    impl_->download(task, listener);
}

void HttpHandler::pause(DownloadTask::Ptr task) {
    impl_->pause(task);
}

void HttpHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    impl_->resume(task, listener);
}

void HttpHandler::cancel(DownloadTask::Ptr task) {
    impl_->cancel(task);
}

std::unique_ptr<IProtocolHandler> create_http_handler() {
    return std::make_unique<HttpHandler>();
}

}  // namespace plugins
}  // namespace falcon
