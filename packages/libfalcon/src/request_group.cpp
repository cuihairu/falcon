/**
 * @file request_group.cpp
 * @brief 请求组实现
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <falcon/request_group.hpp>
#include <falcon/logger.hpp>
#include <falcon/download_engine_v2.hpp>

#include <algorithm>
#include <filesystem>
#include <string_view>

#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
#include "../plugins/http/http_handler.hpp"
#endif

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

class RequestGroupDownloadCommand final : public AbstractCommand {
public:
    explicit RequestGroupDownloadCommand(TaskId task_id)
        : AbstractCommand(task_id) {}

    ~RequestGroupDownloadCommand() override {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    const char* name() const override { return "RequestGroupDownload"; }

    bool execute(DownloadEngineV2* engine) override {
        if (!engine) {
            return handle_result(ExecutionResult::ERROR_OCCURRED);
        }

        auto* group_man = engine->request_group_man();
        auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
        if (!group) {
            return handle_result(ExecutionResult::OK);
        }

        auto task = group->download_task();
        if (!task) {
            group->set_error_message("RequestGroup 缺少下载任务对象");
            group->set_status(RequestGroupStatus::FAILED);
            return handle_result(ExecutionResult::OK);
        }

        if (!started_) {
            // Start download in background to keep the event loop responsive.
            started_ = true;

            task->mark_started();
            task->set_status(TaskStatus::Downloading);

            worker_ = std::thread([task]() {
                try {
                    auto handler = task->get_handler();
                    if (!handler) {
                        task->set_error("No protocol handler available for RequestGroup");
                        task->set_status(TaskStatus::Failed);
                        return;
                    }

                    handler->download(task, /*listener=*/nullptr);

                    if (task->status() == TaskStatus::Downloading) {
                        // Handlers are expected to set final status; treat missing transition as failure.
                        task->set_error("Handler returned without final status");
                        task->set_status(TaskStatus::Failed);
                    }
                } catch (const std::exception& e) {
                    task->set_error(e.what());
                    task->set_status(TaskStatus::Failed);
                }
            });

            mark_active();
            return false;
        }

        // Poll completion
        TaskStatus st = task->status();
        if (st == TaskStatus::Completed) {
            if (worker_.joinable()) worker_.join();
            group->set_status(RequestGroupStatus::COMPLETED);
            return handle_result(ExecutionResult::OK);
        }
        if (st == TaskStatus::Failed) {
            if (worker_.joinable()) worker_.join();
            group->set_error_message(task->error_message());
            group->set_status(RequestGroupStatus::FAILED);
            return handle_result(ExecutionResult::OK);
        }
        if (st == TaskStatus::Cancelled) {
            if (worker_.joinable()) worker_.join();
            group->set_status(RequestGroupStatus::REMOVED);
            return handle_result(ExecutionResult::OK);
        }
        if (st == TaskStatus::Paused) {
            if (worker_.joinable()) worker_.join();
            group->set_status(RequestGroupStatus::PAUSED);
            return handle_result(ExecutionResult::OK);
        }

        mark_active();
        return false;
    }

private:
    bool started_ = false;
    std::thread worker_;
};
} // namespace

//==============================================================================
// RequestGroup 实现
//==============================================================================

RequestGroup::RequestGroup(TaskId id,
                             const std::vector<std::string>& uris,
                             const DownloadOptions& options)
    : id_(id)
    , status_(RequestGroupStatus::WAITING)
    , uris_(uris)
    , current_uri_index_(0)
    , options_(options)
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

    FALCON_LOG_INFO("创建 RequestGroup: id=" << id_ << ", url=" << uris_[0]);
}

RequestGroup::~RequestGroup() {
    FALCON_LOG_DEBUG("销毁 RequestGroup: id=" << id_);
}

bool RequestGroup::init() {
    FALCON_LOG_DEBUG("初始化 RequestGroup: id=" << id_);
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

#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
    if (starts_with(url, "http://") || starts_with(url, "https://")) {
        auto handler = std::make_shared<plugins::HttpHandler>();
        download_task_->set_handler(handler);
        return true;
    }
#endif

    set_error_message("V2 当前仅支持 HTTP/HTTPS（且需启用 HTTP 插件）");
    status_ = RequestGroupStatus::FAILED;
    return false;
}

std::unique_ptr<Command> RequestGroup::create_initial_command() {
    FALCON_LOG_DEBUG("创建初始命令: id=" << id_ << ", url=" << current_uri());

    if (!init() || !download_task_ || !download_task_->get_handler()) {
        if (status_ != RequestGroupStatus::FAILED) {
            set_error_message("初始化失败或缺少协议处理器");
            status_ = RequestGroupStatus::FAILED;
        }
        return nullptr;
    }

    return std::make_unique<RequestGroupDownloadCommand>(id_);
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
        FALCON_LOG_INFO("暂停 RequestGroup: id=" << id_);

        if (download_task_) {
            download_task_->pause();
        }
    }
}

void RequestGroup::resume() {
    if (status_ == RequestGroupStatus::PAUSED) {
        status_ = RequestGroupStatus::WAITING;
        FALCON_LOG_INFO("恢复 RequestGroup: id=" << id_);
    }
}

DownloadTask::Ptr RequestGroup::download_task() const noexcept {
    return download_task_;
}

//==============================================================================
// RequestGroupMan 实现
//==============================================================================

RequestGroupMan::RequestGroupMan(std::size_t max_concurrent)
    : max_concurrent_(max_concurrent)
{
    FALCON_LOG_INFO("创建 RequestGroupMan: max_concurrent=" << max_concurrent);
}

void RequestGroupMan::add_request_group(std::unique_ptr<RequestGroup> group) {
    if (!group) {
        FALCON_LOG_ERROR("尝试添加空 RequestGroup");
        return;
    }

    TaskId id = group->id();

    // 检查是否已存在
    if (group_map_.find(id) != group_map_.end()) {
        FALCON_LOG_WARN("RequestGroup 已存在: id=" << id);
        return;
    }

    FALCON_LOG_INFO("添加 RequestGroup: id=" << id << ", url=" << group->current_uri());

    // 添加到所有组列表（转移所有权）
    all_groups_.push_back(std::move(group));
    RequestGroup* raw_ptr = all_groups_.back().get();

    // 添加到映射
    group_map_[id] = raw_ptr;

    // 新任务默认进入等待队列，实际激活由引擎调度
    raw_ptr->set_status(RequestGroupStatus::WAITING);
    reserved_groups_.push_back(raw_ptr);
    FALCON_LOG_DEBUG("任务加入等待队列: id=" << id);
}

void RequestGroupMan::fill_request_group_from_reserver(DownloadEngineV2* engine) {
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

            FALCON_LOG_INFO("从等待队列激活任务: id=" << id);
            found_waiting = true;

            if (engine) {
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
    auto it = group_map_.find(id);
    if (it == group_map_.end()) {
        FALCON_LOG_WARN("任务不存在: id=" << id);
        return false;
    }

    auto* group = it->second;
    if (!group) return false;

    // 若在活动队列中，移回等待队列
    auto active_it = std::find(request_groups_.begin(), request_groups_.end(), group);
    if (active_it != request_groups_.end()) {
        request_groups_.erase(active_it);
        reserved_groups_.push_back(group);
    }

    group->set_status(RequestGroupStatus::PAUSED);
    group->pause();
    return true;
}

bool RequestGroupMan::resume_group(TaskId id) {
    auto it = group_map_.find(id);
    if (it == group_map_.end()) {
        FALCON_LOG_WARN("任务不存在: id=" << id);
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
    auto it = group_map_.find(id);
    if (it == group_map_.end()) {
        FALCON_LOG_WARN("任务不存在: id=" << id);
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

    FALCON_LOG_INFO("标记 RequestGroup 为 REMOVED: id=" << id);
    return true;
}

RequestGroup* RequestGroupMan::find_group(TaskId id) {
    auto it = group_map_.find(id);
    return it != group_map_.end() ? it->second : nullptr;
}

void RequestGroupMan::cleanup_finished_active() {
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

} // namespace falcon
