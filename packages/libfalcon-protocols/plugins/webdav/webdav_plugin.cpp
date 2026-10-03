/// @file webdav_plugin.cpp
/// WebDAV 协议处理器（libcurl 数据面）。
///
/// dav:// → http://、davs:// → https:// scheme 重写后交 libcurl；认证经
/// CURLOPT_USERNAME/PASSWORD + CURLOPT_HTTPAUTH(CURLAUTH_ANY)——Basic 与
/// Digest 由 libcurl 按 401 挑战协商。userinfo percent-decode；path 保持
/// percent-encoded 原样（curl 原样发送、服务器解码）。断点续传经
/// CURLOPT_RESUME_FROM_LARGE（`.falcon.tmp` 尺寸即续传起点）；curl 自带
/// Range 防护：带 Range 的请求被服务器以 200/416 应答时 CURLE_RANGE_ERROR
/// ——续传响应未带 Content-Range 绝不接续，成品不可能混合新旧内容。
/// timeout_seconds 语义与 http_handler 同契约（B6 P0 教训）：下载数据面
/// 映射 LOW_SPEED 停滞看门狗（低于 1 B/s 持续该秒数才中止；0 = 关），
/// **绝不映射 CURLOPT_TIMEOUT**（总时长硬帽，默认 30s 会杀死一切慢而
/// 健康的真实下载）；HEAD 探测保持 CURLOPT_TIMEOUT（探测应秒级完成）。
/// 暂停/取消语义与 FtpHandler 同约定：progress_callback 查任务状态返回 1
/// → CURLE_ABORTED_BY_CALLBACK，download 静默 return（保留 .falcon.tmp
/// 断点，绝不抛异常——worker 的 catch 会把 Paused 覆写成 Failed）。

#include "webdav_handler.hpp"
#include <falcon/detail/injection.hpp>

#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>

#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

namespace falcon::protocols {
namespace {

/// RFC 3986 percent-decode（'+' 不作空格——URL userinfo 语义）
std::string percent_decode(const std::string& s) {
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_value(s[i + 1]);
            int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::uint16_t parse_port(const std::string& text, const std::string& url) {
    if (text.empty()) return 0;
    if (text.find_first_not_of("0123456789") != std::string::npos) {
        throw InvalidURLException("WebDAV URL has invalid port '" + text +
                                  "': " + url);
    }
    unsigned long value = 0;
    try {
        value = std::stoul(text);
    } catch (const std::exception&) {
        throw InvalidURLException("WebDAV URL has invalid port '" + text +
                                  "': " + url);
    }
    if (value == 0 || value > 65535) {
        throw InvalidURLException("WebDAV URL port out of range '" + text +
                                  "': " + url);
    }
    return static_cast<std::uint16_t>(value);
}

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

static void apply_dav_curl_options(CURL* curl,
                                   const DownloadOptions& options,
                                   const detail::WebdavEndpoint& endpoint) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    if (!endpoint.user.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, endpoint.user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, endpoint.password.c_str());
        // Basic 与 Digest 由 libcurl 按 401 挑战协商（CURLAUTH_ANY）
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    }

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

WebdavHandler::WebdavHandler() = default;
WebdavHandler::~WebdavHandler() = default;

bool WebdavHandler::can_handle(const std::string& url) const {
    return url.rfind("dav://", 0) == 0 || url.rfind("davs://", 0) == 0;
}

FileInfo WebdavHandler::get_file_info(const std::string& url, const DownloadOptions& options) {
    const auto endpoint = detail::parse_dav_url(url, options);
    const std::string http_url = detail::to_http_url(endpoint);

    // 注入命中时短路真实调用，避免已创建句柄在 throw 路径泄漏
    // 注入点全限定：webdav_handler.hpp 的 detail::（WebdavEndpoint 等）
    // 占住了 falcon::protocols::detail，限定查找不会落到 falcon::detail
    CURL* curl = falcon::detail::inject_failure(
                     falcon::detail::InjectPoint::CurlEasyInit)
                     ? nullptr
                     : curl_easy_init();
    if (!curl) {
        throw NetworkException("Failed to initialize CURL");
    }

    curl_easy_setopt(curl, CURLOPT_URL, http_url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
    // HEAD 探测保持总帽语义（探测应秒级完成；与 http_handler HEAD 同款）
    if (options.timeout_seconds > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                         static_cast<long>(options.timeout_seconds));
    }

    apply_dav_curl_options(curl, options, endpoint);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string msg = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        throw NetworkException("CURL error: " + msg);
    }

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code >= 400) {
        curl_easy_cleanup(curl);
        throw NetworkException("WebDAV server returned HTTP " +
                               std::to_string(response_code) + " for " + url);
    }

    curl_off_t cl = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);

    FileInfo info;
    info.url = url;
    info.total_size = cl > 0 ? static_cast<Bytes>(cl) : 0;
    info.supports_resume = true; // curl Range 防护兜底；WebDAV 服务器普遍支持

    curl_easy_cleanup(curl);
    return info;
}

void WebdavHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    if (!task) return;
    const auto& options = task->options();
    const auto endpoint = detail::parse_dav_url(task->url(), options);
    const std::string http_url = detail::to_http_url(endpoint);
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

        // 注入命中时短路真实调用，避免已创建句柄在 throw 路径泄漏
        CURL* curl = falcon::detail::inject_failure(
                         falcon::detail::InjectPoint::CurlEasyInit)
            ? nullptr
            : curl_easy_init();
        if (!curl) {
            throw NetworkException("Failed to initialize CURL");
        }

        curl_easy_setopt(curl, CURLOPT_URL, http_url.c_str());
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

        apply_dav_curl_options(curl, options, endpoint);

        if (start_offset > 0) {
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(start_offset));
        }

        // timeout_seconds 是停滞超时（aria2 --timeout 同语义）：低于
        // 1 B/s 持续该秒数才中止——绝不映射 CURLOPT_TIMEOUT（整传输
        // 硬上限，默认 30s 会杀死一切慢而健康的真实下载，B6 P0 教训）。
        // 0 = 关停滞看门狗（契约见 download_options.hpp）
        if (options.timeout_seconds > 0) {
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                             static_cast<long>(options.timeout_seconds));
        }

        CURLcode res = curl_easy_perform(curl);
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        file.close();
        curl_easy_cleanup(curl);

        if (res == CURLE_ABORTED_BY_CALLBACK) {
            return;
        }

        // 未开 FAILONERROR：4xx/5xx 也走 CURLE_OK——不查状态码就会把
        // 错误页 body 改名成成品。>=400 一律按失败收口，残量留给重试。
        if (res == CURLE_OK && response_code < 400) {
            if (std::rename(temp_path.c_str(), task->output_path().c_str()) != 0) {
                throw FileIOException("Failed to move downloaded file to destination");
            }
            task->set_status(TaskStatus::Completed);
            return;
        }

        if (res != CURLE_OK) {
            last_error = curl_easy_strerror(res);
        } else {
            last_error = "WebDAV server returned HTTP " + std::to_string(response_code);
        }

        if (attempt >= options.max_retries) {
            throw NetworkException(last_error.empty() ? "WebDAV download failed" : last_error);
        }

        if (options.retry_delay_seconds > 0) {
            std::size_t backoff = options.retry_delay_seconds;
            backoff *= (std::size_t{1} << attempt);
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
        }
    }
}

void WebdavHandler::pause(DownloadTask::Ptr task) {
    if (!task) return;
    task->set_status(TaskStatus::Paused);
}

void WebdavHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    if (!task) return;
    task->set_status(TaskStatus::Downloading);
    download(std::move(task), listener);
}

void WebdavHandler::cancel(DownloadTask::Ptr task) {
    if (!task) return;
    task->set_status(TaskStatus::Cancelled);
}

std::unique_ptr<IProtocolHandler> create_webdav_handler() {
    return std::make_unique<WebdavHandler>();
}

namespace detail {

WebdavEndpoint parse_dav_url(const std::string& url,
                             const DownloadOptions& options) {
    const bool secure = url.rfind("davs://", 0) == 0;
    const std::string scheme = secure ? "davs://" : "dav://";
    std::string rest = url.substr(scheme.size());

    const std::size_t slash = rest.find('/');
    const std::string authority =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string path =
        slash == std::string::npos ? std::string() : rest.substr(slash);

    std::string userpart;
    std::string hostpart;
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        userpart = authority.substr(0, at);
        hostpart = authority.substr(at + 1);
    } else {
        hostpart = authority;
    }

    std::string host;
    std::uint16_t port = 0;
    if (!hostpart.empty() && hostpart[0] == '[') {
        // RFC 3986：[IPv6-literal] 与可选 ]:port
        const std::size_t close = hostpart.find(']');
        if (close == std::string::npos) {
            throw InvalidURLException("WebDAV URL has malformed IPv6 literal: " +
                                      url);
        }
        host = hostpart.substr(1, close - 1);
        if (close + 2 < hostpart.size() && hostpart[close + 1] == ':') {
            port = parse_port(hostpart.substr(close + 2), url);
        }
    } else {
        const std::size_t colon = hostpart.rfind(':');
        if (colon != std::string::npos) {
            port = parse_port(hostpart.substr(colon + 1), url);
            host = hostpart.substr(0, colon);
        } else {
            host = hostpart;
        }
    }
    if (host.empty()) {
        throw InvalidURLException("WebDAV URL has no host: " + url);
    }

    std::string user;
    std::string password;
    if (at != std::string::npos) {
        const std::size_t colon = userpart.find(':');
        if (colon == std::string::npos) {
            user = userpart;
        } else {
            user = userpart.substr(0, colon);
            password = userpart.substr(colon + 1);
        }
    }
    if (user.empty()) user = options.http_username;
    if (password.empty()) password = options.http_password;
    if (path.empty()) {
        throw InvalidURLException("WebDAV URL has no remote path: " + url);
    }

    WebdavEndpoint endpoint;
    endpoint.secure = secure;
    endpoint.host = host;
    endpoint.port = port;
    endpoint.user = percent_decode(user);
    endpoint.password = percent_decode(password);
    endpoint.path = path; // 保持 percent-encoded 原样交 curl
    return endpoint;
}

std::string to_http_url(const WebdavEndpoint& endpoint) {
    std::string out = endpoint.secure ? "https://" : "http://";
    if (endpoint.host.find(':') != std::string::npos) {
        out += '[' + endpoint.host + ']'; // IPv6 字面量保持 [..] 形态
    } else {
        out += endpoint.host;
    }
    if (endpoint.port != 0) {
        out += ':' + std::to_string(endpoint.port);
    }
    out += endpoint.path;
    return out;
}

} // namespace detail

} // namespace falcon::protocols
