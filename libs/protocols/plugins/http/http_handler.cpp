#include "http_handler_helpers.hpp"

namespace falcon {
namespace protocols {

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
        CURL* curl = falcon::detail::inject_failure(
                         falcon::detail::InjectPoint::CurlEasyInit)
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
                falcon::detail::inject_failure(
                    falcon::detail::InjectPoint::CurlEasyInit)
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
                    // Move temp file to final destination。Windows 杀毒/
                    // 索引对刚写完文件的瞬时句柄占用值得重试而不是直接
                    // 判失败（数据已完整落盘）
                    const auto rr = protocols::detail::rename_with_retry(
                        temp_path, task->output_path());
                    if (!rr.ok) {
                        throw FileIOException(
                            "Failed to move downloaded file to destination: " +
                            rr.error);
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
            // 陈旧失败收口守卫（B11 的镜像）：download() 实际运行在
            // TaskManager 的 download_pool_ 线程（worker 仅出队置
            // Downloading 后 submit），pause→resume 可在旧 attempt
            // 尚未收尾时把同 id 任务重新派发——旧 attempt 的失败抛
            // 出会被 worker catch 改写成 Failed，覆盖已被新 attempt
            // 接管（或已回 Pending/终态）的任务；新 attempt 的 start
            // 前终态复查见 Failed 静默返回 → 任务永久 Failed 且无
            // 成品（Windows CI 红面：循环内断言全绿、终态 Failed、
            // 成品缺失）。preserve 标志只在用户暂停路径置位，是本
            // 实例「经历过暂停接管」的机制级信号；段断点已保留，
            // 真正的永久失败由续跑方再次失败并如实上报
            if (downloader->cancel_preserved() || task->is_finished() ||
                task->status() == TaskStatus::Paused) {
                return;
            }
            const auto reason = downloader->failure_message();
            throw FileIOException(reason.empty()
                                      ? "Segmented download failed"
                                      : reason);
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
    impl_->download(task, listener);
}

void HttpHandler::pause(DownloadTask::Ptr task) {
    if (!task) return;  // 与 FtpHandler 同款防御：空任务无操作
    impl_->pause(task);
}

void HttpHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    if (!task) return;
    impl_->resume(task, listener);
}

void HttpHandler::cancel(DownloadTask::Ptr task) {
    if (!task) return;
    impl_->cancel(task);
}

std::unique_ptr<IProtocolHandler> create_http_handler() {
    return std::make_unique<HttpHandler>();
}

} // namespace protocols
} // namespace falcon
