/**
 * @file ftp_plugin.cpp
 * @brief FTP/FTPS protocol handler (libcurl)
 * @author Falcon Team
 * @date 2025-12-26
 */

#include "ftp_handler.hpp"

#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

namespace falcon::plugins {
namespace {

struct ProgressData {
    DownloadTask::Ptr task;
    IEventListener* listener = nullptr;
    Bytes start_offset = 0;
    std::chrono::steady_clock::time_point last_update = std::chrono::steady_clock::now();
    Bytes last_bytes = 0;
};

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* file = static_cast<std::ofstream*>(userp);
    if (!file || !file->is_open()) {
        return 0;
    }
    const size_t total = size * nmemb;
    file->write(static_cast<const char*>(contents), static_cast<std::streamsize>(total));
    return total;
}

static int progress_callback(void* clientp,
                             curl_off_t dltotal,
                             curl_off_t dlnow,
                             curl_off_t /*ultotal*/,
                             curl_off_t /*ulnow*/) {
    auto* data = static_cast<ProgressData*>(clientp);
    if (!data || !data->task) return 0;

    TaskStatus status = data->task->status();
    if (status == TaskStatus::Paused || status == TaskStatus::Cancelled) {
        return 1;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - data->last_update);
    if (elapsed.count() < 200) {
        return 0;
    }

    Bytes downloaded = data->start_offset + static_cast<Bytes>(dlnow);
    Bytes total = dltotal > 0 ? (data->start_offset + static_cast<Bytes>(dltotal)) : 0;
    Bytes diff = downloaded - data->last_bytes;

    BytesPerSecond speed = 0;
    if (elapsed.count() > 0) {
        speed = static_cast<BytesPerSecond>(diff * 1000 / static_cast<Bytes>(elapsed.count()));
    }

    data->task->update_progress(downloaded, total, speed);
    data->last_update = now;
    data->last_bytes = downloaded;

    return 0;
}

static void apply_common_curl_options(CURL* curl, const DownloadOptions& options) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(options.timeout_seconds));

    if (!options.proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, options.proxy.c_str());
    }

    if (!options.proxy_username.empty()) {
        std::string proxy_userpwd = options.proxy_username + ":" + options.proxy_password;
        curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, proxy_userpwd.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    }

    if (!options.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (options.speed_limit > 0) {
        curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                         static_cast<curl_off_t>(options.speed_limit));
    }
}

} // namespace

FtpHandler::FtpHandler() = default;
FtpHandler::~FtpHandler() = default;

bool FtpHandler::can_handle(const std::string& url) const {
    return url.rfind("ftp://", 0) == 0 || url.rfind("ftps://", 0) == 0;
}

FileInfo FtpHandler::get_file_info(const std::string& url, const DownloadOptions& options) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw NetworkException("Failed to initialize CURL");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
    curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);

    apply_common_curl_options(curl, options);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string msg = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        throw NetworkException("CURL error: " + msg);
    }

    curl_off_t cl = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);

    FileInfo info;
    info.url = url;
    info.total_size = cl > 0 ? static_cast<Bytes>(cl) : 0;
    info.supports_resume = true; // FTP REST is common; actual resume is best-effort.

    curl_easy_cleanup(curl);
    return info;
}

void FtpHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    if (!task) return;
    const auto& options = task->options();
    const std::string temp_path = task->output_path() + ".falcon.tmp";

    std::string last_error;
    for (std::size_t attempt = 0; attempt <= options.max_retries; ++attempt) {
        if (task->status() == TaskStatus::Paused || task->status() == TaskStatus::Cancelled) {
            return;
        }

        // Best-effort file info for total length
        try {
            auto info = get_file_info(task->url(), options);
            task->set_file_info(info);
        } catch (...) {
            // Ignore; progress may still work with unknown total.
        }

        Bytes start_offset = 0;
        if (options.resume_enabled) {
            std::ifstream test(temp_path, std::ios::binary | std::ios::ate);
            if (test.is_open()) {
                start_offset = static_cast<Bytes>(test.tellg());
            }
        }

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

        curl_easy_setopt(curl, CURLOPT_URL, task->url().c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        ProgressData progress;
        progress.task = task;
        progress.listener = listener;
        progress.start_offset = start_offset;
        progress.last_update = std::chrono::steady_clock::now();
        progress.last_bytes = start_offset;
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);

        apply_common_curl_options(curl, options);

        if (start_offset > 0) {
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(start_offset));
        }

        CURLcode res = curl_easy_perform(curl);
        file.close();
        curl_easy_cleanup(curl);

        if (res == CURLE_ABORTED_BY_CALLBACK) {
            return;
        }

        if (res != CURLE_OK) {
            last_error = curl_easy_strerror(res);
        } else {
            if (std::rename(temp_path.c_str(), task->output_path().c_str()) != 0) {
                throw FileIOException("Failed to move downloaded file to destination");
            }
            task->set_status(TaskStatus::Completed);
            return;
        }

        if (attempt >= options.max_retries) {
            throw NetworkException(last_error.empty() ? "FTP download failed" : last_error);
        }

        if (options.retry_delay_seconds > 0) {
            std::size_t backoff = options.retry_delay_seconds;
            backoff *= (std::size_t{1} << attempt);
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
        }
    }
}

void FtpHandler::pause(DownloadTask::Ptr task) {
    if (!task) return;
    task->set_status(TaskStatus::Paused);
}

void FtpHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    if (!task) return;
    task->set_status(TaskStatus::Downloading);
    download(std::move(task), listener);
}

void FtpHandler::cancel(DownloadTask::Ptr task) {
    if (!task) return;
    task->set_status(TaskStatus::Cancelled);
}

std::unique_ptr<IProtocolHandler> create_ftp_handler() {
    return std::make_unique<FtpHandler>();
}

} // namespace falcon::plugins
