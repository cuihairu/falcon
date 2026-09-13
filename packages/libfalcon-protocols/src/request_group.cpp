/**
 * @file request_group.cpp
 * @brief 请求组实现
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <falcon/protocols/request_group.hpp>
#include <falcon/logger.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/resume_control.hpp>

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace falcon {

// 静态成员初始化
FileInfo RequestGroup::empty_file_;

namespace {
std::string derive_filename_from_url(const std::string& url) {
    std::string filename = "download";
    auto pos = url.rfind('/');
    if (pos != std::string::npos && pos + 1 < url.size()) {
        filename = url.substr(pos + 1);
        auto query_pos = filename.find('?');
        if (query_pos != std::string::npos) {
            filename = filename.substr(0, query_pos);
        }
        if (filename.empty()) {
            filename = "download";
        }
    }
    return filename;
}

std::string build_output_path_for_options(const std::string& url, const DownloadOptions& options) {
    const bool has_custom_dir =
        !options.output_directory.empty() && options.output_directory != ".";

    std::filesystem::path out_dir =
        has_custom_dir ? std::filesystem::path(options.output_directory) : std::filesystem::path();

    std::filesystem::path out_path;
    if (!options.output_filename.empty()) {
        std::filesystem::path filename(options.output_filename);
        if (has_custom_dir && filename.is_relative()) {
            out_path = out_dir / filename;
        } else {
            out_path = filename;
        }
    } else {
        out_path = has_custom_dir ? (out_dir / derive_filename_from_url(url))
                                  : std::filesystem::path(derive_filename_from_url(url));
    }

    if (options.create_directory) {
        std::error_code ec;
        auto parent = out_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
    }

    return out_path.string();
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}
} // namespace

//==============================================================================
// RequestGroup 实现
//==============================================================================

RequestGroup::RequestGroup(TaskId id,
                             const std::vector<std::string>& uris,
                             const DownloadOptions& options,
                             const std::string& output_path_override)
    : id_(id)
    , status_(RequestGroupStatus::WAITING)
    , uris_(uris)
    , current_uri_index_(0)
    , options_(options)
    , output_path_override_(output_path_override)
    , segment_downloader_(nullptr)
    , downloaded_bytes_(0)
{
    // 验证 URI 列表
    if (uris_.empty()) {
        set_error_message("URI 列表为空");
        status_ = RequestGroupStatus::FAILED;
        return;
    }

    // 创建默认文件信息
    FileInfo info;
    info.url = uris_[0];
    files_.push_back(info);

    FALCON_LOG_INFO_STREAM("创建 RequestGroup: id=" << id_ << ", url=" << uris_[0]);
}

RequestGroup::~RequestGroup() {
    FALCON_LOG_DEBUG_STREAM("销毁 RequestGroup: id=" << id_);
}

bool RequestGroup::init() {
    FALCON_LOG_DEBUG_STREAM("初始化 RequestGroup: id=" << id_);
    if (status_ == RequestGroupStatus::FAILED) {
        return false;
    }

    if (download_task_) {
        return true;
    }

    const std::string url = current_uri();

    // Build output path and initialize internal DownloadTask for reuse.
    download_task_ = std::make_shared<DownloadTask>(id_, url, options_);
    download_task_->set_output_path(build_output_path_for_options(url, options_));
    // 宿主化桥接注入的显式路径优先于自推导（V1 任务的 output_path
    // 已确定，两侧必须写同一文件）；覆盖门禁在下方按最终路径检查
    if (!output_path_override_.empty()) {
        download_task_->set_output_path(output_path_override_);
    }

    // 输出文件已存在且未显式允许覆盖 → 直接失败（aria2 allow-overwrite=false
    // 同语义）。此前 HttpDownloadCommand 首段无条件 trunc，默认配置
    // （overwrite_existing=false）也会静默销毁已存在的同名文件；断点
    // 续传接管了"已存在半成品"的语义：失败/中断留下的临时文件 + 控制
    // 文件可恢复下载，最终名文件仍然受覆盖保护。
    if (!options_.overwrite_existing) {
        std::error_code exists_ec;
        const std::string& out_path = download_task_->output_path();
        if (std::filesystem::exists(out_path, exists_ec) && !exists_ec) {
            const std::string reason =
                "输出文件已存在（设置 overwrite_existing = true 以覆盖）: " + out_path;
            FALCON_LOG_WARN_STREAM("任务组失败: id=" << id_ << ", " << reason);
            set_error_message(reason);
            download_task_->set_error(reason);
            download_task_->set_status(TaskStatus::Failed);
            status_ = RequestGroupStatus::FAILED;
            return false;
        }
    }

    // 代理配置 V2 数据面不支持（socks 系列 / TLS 代理 / 未知 scheme）：
    // 明确失败而非静默直连——M2 适配层据此回退 V1 curl（libcurl 自带
    // socks 支持）；命令层只承接 None / HttpProxy 两种形态
    if (parse_http_proxy(options_).kind == HttpProxyKind::Unsupported) {
        const std::string reason =
            "V2 引擎暂不支持该代理类型（仅明文 HTTP 代理，socks/HTTPS 代理请回退 V1）: " +
            options_.proxy;
        FALCON_LOG_WARN_STREAM("任务组失败: id=" << id_ << ", " << reason);
        set_error_message(reason);
        download_task_->set_error(reason);
        download_task_->set_status(TaskStatus::Failed);
        status_ = RequestGroupStatus::FAILED;
        return false;
    }

    try_load_resume_state(url);

    if (starts_with(url, "http://")) {
        return true;
    }

#ifdef FALCON_ENABLE_OPENSSL
    // TLS 支持在连接命令层（异步握手 + 证书校验硬断连），此处放行
    if (starts_with(url, "https://")) {
        return true;
    }
    set_error_message("V2 当前仅支持 http:// 与 https://");
#else
    if (starts_with(url, "https://")) {
        set_error_message("V2 HTTPS 支持需要启用 OpenSSL");
    } else {
        set_error_message("V2 当前仅支持 http://");
    }
#endif
    // URL 协议不受支持：组 FAILED 的同时同步任务终态（任务此前会永久
    // 停留在初始 Pending 态，与组状态脱节）
    download_task_->set_error(error_message());
    download_task_->set_status(TaskStatus::Failed);
    status_ = RequestGroupStatus::FAILED;
    return false;
}

std::unique_ptr<Command> RequestGroup::create_initial_command() {
    FALCON_LOG_DEBUG_STREAM("创建初始命令: id=" << id_ << ", url=" << current_uri());

    if (!init() || !download_task_) {
        if (status_ != RequestGroupStatus::FAILED) {
            set_error_message("初始化失败");
            status_ = RequestGroupStatus::FAILED;
        }
        return nullptr;
    }

    auto cmd = std::make_unique<HttpInitiateConnectionCommand>(id_, current_uri(), options_);
    // 跨会话恢复：init() 已从控制文件加载续传状态时，初始连接直接
    // 携带第一个未完成段的 Range 与 If-Range（无有效断点时为无操作）
    apply_group_resume_range(*cmd, *this);
    return cmd;
}

RequestGroup::Progress RequestGroup::get_progress() const {
    Progress progress;
    if (download_task_) {
        progress.downloaded = download_task_->downloaded_bytes();
        progress.total = download_task_->total_bytes();
        progress.speed = download_task_->speed();
        progress.progress = download_task_->progress();
        progress.active_connections = 0;
        return progress;
    }

    progress.downloaded = downloaded_bytes_;
    if (!files_.empty()) {
        progress.total = files_[0].total_size;
    }
    progress.progress = (progress.total > 0)
                            ? static_cast<double>(progress.downloaded) /
                                  static_cast<double>(progress.total)
                            : 0.0;

    return progress;
}

void RequestGroup::pause() {
    if (status_ == RequestGroupStatus::ACTIVE) {
        status_ = RequestGroupStatus::PAUSED;
        FALCON_LOG_INFO_STREAM("暂停 RequestGroup: id=" << id_);

        // 暂停即固化进度：控制文件带最新各段断点，恢复（跨引擎重启
        // 重新添加任务）时从断点继续
        save_resume_now();

        if (download_task_) {
            download_task_->pause();
        }
    }
}

void RequestGroup::resume() {
    if (status_ == RequestGroupStatus::PAUSED) {
        status_ = RequestGroupStatus::WAITING;
        FALCON_LOG_INFO_STREAM("恢复 RequestGroup: id=" << id_);
    }
}

DownloadTask::Ptr RequestGroup::download_task() const noexcept {
    return download_task_;
}

//==============================================================================
// 多分段下载跟踪
//==============================================================================

bool RequestGroup::is_multi_segment() const {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    return segment_total_ > 0;
}

void RequestGroup::begin_multi_segment(std::size_t total_segments) {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    segment_total_ = total_segments;
    segment_finished_ = 0;
    segment_failure_ = false;
    FALCON_LOG_INFO_STREAM("多分段下载开始: id=" << id_ << ", 分段数=" << total_segments);
}

bool RequestGroup::finish_segment(bool success) {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    if (segment_total_ == 0) {
        // 单段模式：唯一分段即全部
        return true;
    }
    if (!success) {
        segment_failure_ = true;
    }
    ++segment_finished_;
    return segment_finished_ >= segment_total_;
}

bool RequestGroup::has_segment_failure() const {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    return segment_failure_;
}

//==============================================================================
// 断点续传状态
//==============================================================================

std::string RequestGroup::temp_file_path() const {
    if (temp_extension_.empty() || !download_task_) {
        return {};
    }
    return download_task_->output_path() + temp_extension_;
}

std::string RequestGroup::control_file_path() const {
    if (!download_task_) {
        return {};
    }
    return download_task_->output_path() + kResumeControlExtension;
}

void RequestGroup::try_load_resume_state(const std::string& url) {
    // 续传的四个前置条件：任务开启续传（默认开）、未显式授权覆盖
    //（覆盖 = 明确要求重下）、有临时文件语义（挂点所在）、控制文件可解析
    if (!options_.resume_enabled || options_.overwrite_existing ||
        temp_extension_.empty()) {
        return;
    }

    const std::string ctrl_path = control_file_path();
    const std::string temp_path = temp_file_path();
    ResumeControl control;
    if (!load_resume_control(ctrl_path, control)) {
        return;  // 无控制文件（首次下载）或文件损坏 → 全新下载
    }

    // 严格校验：URL 必须一致（不同 URL 指向同一输出路径视为新任务）；
    // 临时文件尺寸必须与各段断点吻合——下限证明控制文件记录的数据
    // 确实落过盘，上限证明位置写没有越界（越界即文件已损坏）。
    // 汇总进度恰好等于总长不该发生（完成路径已删除控制文件），视为
    // 异常状态重下
    if (control.url != url) {
        FALCON_LOG_INFO_STREAM("续传控制文件 URL 不匹配，放弃续传: " << ctrl_path);
        remove_resume_control(ctrl_path);
        return;
    }

    std::error_code ec;
    const auto temp_size = std::filesystem::file_size(temp_path, ec);
    if (ec) {
        FALCON_LOG_INFO_STREAM("续传临时文件缺失，放弃续传: " << temp_path);
        remove_resume_control(ctrl_path);
        return;
    }

    Bytes max_written = 0;
    Bytes total_downloaded = 0;
    for (const auto& seg : control.segments) {
        // 只统计有进度的段：零进度段尚未写过数据，其 offset 不构成
        // 对临时文件尺寸的下界约束（多段并行时前面的段可能还没轮到）
        if (seg.downloaded > 0) {
            max_written = std::max(max_written, seg.offset + seg.downloaded);
        }
        total_downloaded += seg.downloaded;
    }
    if (temp_size < max_written || temp_size > control.total ||
        total_downloaded >= control.total) {
        FALCON_LOG_WARN_STREAM("续传状态校验失败（临时文件 " << temp_size
                              << " 字节，控制文件记录上界 " << max_written
                              << "，总进度 " << total_downloaded << "/"
                              << control.total << "），放弃续传");
        remove_resume_control(ctrl_path);
        return;
    }

    std::lock_guard<std::mutex> lock(segment_mutex_);
    resume_ = std::move(control);
    resume_valid_ = true;
    last_ctrl_save_ = {};
    FALCON_LOG_INFO_STREAM("恢复断点续传: id=" << id_ << ", 已完成 "
                          << total_downloaded << "/" << resume_.total
                          << " 字节, " << resume_.segments.size() << " 段");
}

void RequestGroup::begin_resume_tracking(std::string url,
                                         Bytes total,
                                         std::string etag,
                                         std::string last_modified,
                                         std::vector<ResumeSegment> segments) {
    // 与 try_load_resume_state 相同的前置门禁：续传被禁用或显式授权
    // 覆盖（明确要求重下）时不建立追踪；总长未知（chunked 等）无从续传
    if (!options_.resume_enabled || options_.overwrite_existing ||
        total == 0 || segments.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(segment_mutex_);
    resume_ = ResumeControl{};
    resume_.url = std::move(url);
    resume_.total = total;
    resume_.etag = std::move(etag);
    resume_.last_modified = std::move(last_modified);
    resume_.segments = std::move(segments);
    resume_valid_ = true;
    last_ctrl_save_ = std::chrono::steady_clock::now();
    if (!save_resume_control(control_file_path(), resume_)) {
        FALCON_LOG_WARN_STREAM("续传控制文件初版写入失败（下载不受影响）: id=" << id_);
    }
}

void RequestGroup::report_segment_flushed(std::size_t segment_index, Bytes downloaded) {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    if (!resume_valid_ || segment_index >= resume_.segments.size()) {
        return;
    }
    auto& seg = resume_.segments[segment_index];
    if (downloaded <= seg.downloaded) {
        return;  // 单调不回退：乱序/重复上报直接忽略
    }
    seg.downloaded = std::min(downloaded, seg.length);

    const auto now = std::chrono::steady_clock::now();
    if (now - last_ctrl_save_ < std::chrono::seconds(1)) {
        return;  // 节流：控制文件写盘每秒至多一次，失败时由收口强制保存
    }
    last_ctrl_save_ = now;
    save_resume_control(control_file_path(), resume_);
}

void RequestGroup::save_resume_now() {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    if (!resume_valid_) {
        return;
    }
    last_ctrl_save_ = std::chrono::steady_clock::now();
    save_resume_control(control_file_path(), resume_);
}

void RequestGroup::clear_resume_tracking() {
    const std::string ctrl_path = control_file_path();
    std::lock_guard<std::mutex> lock(segment_mutex_);
    resume_valid_ = false;
    resume_ = ResumeControl{};
    remove_resume_control(ctrl_path);
}

void RequestGroup::abandon_resume() {
    const std::string ctrl_path = control_file_path();
    {
        std::lock_guard<std::mutex> lock(segment_mutex_);
        resume_valid_ = false;
        resume_ = ResumeControl{};
    }
    remove_resume_control(ctrl_path);
    FALCON_LOG_INFO_STREAM("放弃断点续传，按全新下载处理: id=" << id_);
}

void RequestGroup::prepare_resumed_multi_segment() {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    if (!resume_valid_ || segment_total_ > 0 || resume_.segments.size() <= 1) {
        return;  // 幂等保护；单段续传无需分段跟踪
    }

    segment_total_ = resume_.segments.size();
    segment_finished_ = 0;
    segment_failure_ = false;

    Bytes resumed_total = 0;
    for (const auto& seg : resume_.segments) {
        resumed_total += seg.downloaded;
        if (seg.downloaded >= seg.length) {
            ++segment_finished_;  // 已完成段不再建立连接
        }
    }
    downloaded_bytes_ += resumed_total;
    if (files_[0].total_size == 0) {
        files_[0].total_size = resume_.total;
    }

    FALCON_LOG_INFO_STREAM("多分段续传恢复: id=" << id_ << ", 分段数="
                          << segment_total_ << ", 已完成分段="
                          << segment_finished_ << ", 断点合计 " << resumed_total
                          << " 字节");
}

//==============================================================================
// RequestGroupMan 实现
//==============================================================================

RequestGroupMan::RequestGroupMan(std::size_t max_concurrent)
    : max_concurrent_(max_concurrent)
{
    FALCON_LOG_INFO_STREAM("创建 RequestGroupMan: max_concurrent=" << max_concurrent);
}

void RequestGroupMan::add_request_group(std::unique_ptr<RequestGroup> group) {
    if (!group) {
        FALCON_LOG_ERROR_STREAM("尝试添加空 RequestGroup");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    TaskId id = group->id();

    // 检查是否已存在
    if (group_map_.find(id) != group_map_.end()) {
        FALCON_LOG_WARN_STREAM("RequestGroup 已存在: id=" << id);
        return;
    }

    FALCON_LOG_INFO_STREAM("添加 RequestGroup: id=" << id << ", url=" << group->current_uri());

    // 添加到所有组列表（转移所有权）
    all_groups_.push_back(std::move(group));
    RequestGroup* raw_ptr = all_groups_.back().get();

    // 添加到映射
    group_map_[id] = raw_ptr;

    // 新任务默认进入等待队列，实际激活由引擎调度
    raw_ptr->set_status(RequestGroupStatus::WAITING);
    reserved_groups_.push_back(raw_ptr);
    FALCON_LOG_DEBUG_STREAM("任务加入等待队列: id=" << id);
}

void RequestGroupMan::fill_request_group_from_reserver(DownloadEngineV2* engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 当有空闲槽位时，从等待队列激活任务（跳过 PAUSED）
    while (request_groups_.size() < max_concurrent_) {
        if (reserved_groups_.empty()) {
            return;
        }

        bool found_waiting = false;
        const std::size_t scan = reserved_groups_.size();
        for (std::size_t i = 0; i < scan; ++i) {
            RequestGroup* group = reserved_groups_.front();
            reserved_groups_.pop_front();

            if (!group) {
                continue;
            }

            if (group->status() == RequestGroupStatus::PAUSED) {
                reserved_groups_.push_back(group);
                continue;
            }

            if (group->status() != RequestGroupStatus::WAITING) {
                continue;
            }

            TaskId id = group->id();
            group->set_status(RequestGroupStatus::ACTIVE);
            request_groups_.push_back(group);

            FALCON_LOG_INFO_STREAM("从等待队列激活任务: id=" << id);
            found_waiting = true;

            if (engine) {
                // 激活时注入引擎临时文件扩展名：续传状态据此定位
                // 临时文件做尺寸校验（配置在组创建后才可见）
                group->set_temp_extension(engine->config().temp_extension);
                auto cmd = group->create_initial_command();
                if (cmd) {
                    engine->add_command(std::move(cmd));
                } else {
                    group->set_status(RequestGroupStatus::FAILED);
                }
            }
            break;
        }

        if (!found_waiting) {
            return;
        }
    }
}

bool RequestGroupMan::pause_group(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = group_map_.find(id);
    if (it == group_map_.end()) {
        FALCON_LOG_WARN_STREAM("任务不存在: id=" << id);
        return false;
    }

    auto* group = it->second;
    if (!group) return false;

    // 终态组不可暂停；已暂停则幂等成功
    const auto st = group->status();
    if (st == RequestGroupStatus::COMPLETED || st == RequestGroupStatus::FAILED ||
        st == RequestGroupStatus::REMOVED) {
        FALCON_LOG_WARN_STREAM("任务已终态，无法暂停: id=" << id);
        return false;
    }
    if (st == RequestGroupStatus::PAUSED) {
        return true;
    }

    // 若在活动队列中，移回等待队列
    auto active_it = std::find(request_groups_.begin(), request_groups_.end(), group);
    if (active_it != request_groups_.end()) {
        request_groups_.erase(active_it);
        reserved_groups_.push_back(group);
    }

    // RequestGroup::pause 仅在 ACTIVE 态执行副作用（固化断点 + 暂停
    // 内部任务），必须先调 pause() 再补标状态；旧实现顺序颠倒使
    // ACTIVE 检查恒假，两条副作用从未执行
    group->pause();
    if (group->status() != RequestGroupStatus::PAUSED) {
        group->set_status(RequestGroupStatus::PAUSED);
    }
    return true;
}

bool RequestGroupMan::resume_group(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = group_map_.find(id);
    if (it == group_map_.end()) {
        FALCON_LOG_WARN_STREAM("任务不存在: id=" << id);
        return false;
    }

    auto* group = it->second;
    if (!group) return false;

    if (group->status() == RequestGroupStatus::PAUSED) {
        group->set_status(RequestGroupStatus::WAITING);
    }
    group->resume();

    // Ensure it is queued for scheduling.
    if (std::find(reserved_groups_.begin(), reserved_groups_.end(), group) == reserved_groups_.end() &&
        std::find(request_groups_.begin(), request_groups_.end(), group) == request_groups_.end()) {
        reserved_groups_.push_back(group);
    }

    return true;
}

bool RequestGroupMan::remove_group(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = group_map_.find(id);
    if (it == group_map_.end()) {
        FALCON_LOG_WARN_STREAM("任务不存在: id=" << id);
        return false;
    }

    RequestGroup* group = it->second;
    group->set_status(RequestGroupStatus::REMOVED);

    // 从活动组中移除
    auto active_it = std::find_if(request_groups_.begin(), request_groups_.end(),
        [id](const RequestGroup* g) { return g && g->id() == id; });
    if (active_it != request_groups_.end()) {
        request_groups_.erase(active_it);
    }

    // 从等待队列中移除
    reserved_groups_.erase(
        std::remove(reserved_groups_.begin(), reserved_groups_.end(), group),
        reserved_groups_.end()
    );

    FALCON_LOG_INFO_STREAM("标记 RequestGroup 为 REMOVED: id=" << id);
    return true;
}

RequestGroup* RequestGroupMan::find_group(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = group_map_.find(id);
    return it != group_map_.end() ? it->second : nullptr;
}

void RequestGroupMan::cleanup_finished_active() {
    std::lock_guard<std::mutex> lock(mutex_);
    request_groups_.erase(
        std::remove_if(request_groups_.begin(), request_groups_.end(),
                       [](RequestGroup* g) {
                           if (!g) return true;
                           auto st = g->status();
                           return st == RequestGroupStatus::COMPLETED ||
                                  st == RequestGroupStatus::FAILED ||
                                  st == RequestGroupStatus::REMOVED;
                       }),
        request_groups_.end());
}

void RequestGroupMan::purge_finished_groups() {
    std::vector<std::unique_ptr<RequestGroup>> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = all_groups_.begin(); it != all_groups_.end();) {
            RequestGroup* group = it->get();
            const auto st = group->status();
            if (st != RequestGroupStatus::COMPLETED &&
                st != RequestGroupStatus::FAILED &&
                st != RequestGroupStatus::REMOVED) {
                ++it;
                continue;
            }

            group_map_.erase(group->id());
            // 终态组本不应留在调度队列（remove_group/
            // cleanup_finished_active 已清），防御性移除防悬垂指针
            request_groups_.erase(
                std::remove(request_groups_.begin(), request_groups_.end(), group),
                request_groups_.end());
            reserved_groups_.erase(
                std::remove(reserved_groups_.begin(), reserved_groups_.end(), group),
                reserved_groups_.end());

            retired.push_back(std::move(*it));
            it = all_groups_.erase(it);
        }
    }
    // 锁外析构（析构仅打日志，与 add_request_group 的锁纪律一致；
    // PAUSED 组是停机恢复的挂点，不在回收之列）
}

} // namespace falcon
