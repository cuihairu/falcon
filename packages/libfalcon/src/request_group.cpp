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

namespace falcon {

// 静态成员初始化
FileInfo RequestGroup::empty_file_;

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
        status_ = RequestGroupStatus::ERROR;
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
    // TODO: 实现初始化逻辑
    // 1. 验证 URL 格式
    // 2. 检测协议类型
    // 3. 获取文件元信息（大小、支持断点续传等）
    // 4. 创建分段下载器

    FALCON_LOG_DEBUG("初始化 RequestGroup: id=" << id_);
    return true;
}

std::unique_ptr<Command> RequestGroup::create_initial_command() {
    // TODO: 根据 URL 协议创建初始命令
    // HTTP: HttpInitiateConnectionCommand
    // FTP: FtpInitiateConnectionCommand
    // BitTorrent: BtInitiateConnectionCommand

    FALCON_LOG_DEBUG("创建初始命令: id=" << id_ << ", url=" << current_uri());

    // 占位实现：返回空命令（实际使用时需要创建真实命令）
    return nullptr;
}

RequestGroup::Progress RequestGroup::get_progress() const {
    Progress progress;
    progress.downloaded = downloaded_bytes_;

    if (!files_.empty()) {
        progress.total = files_[0].total_size;
        progress.speed = 0;  // TODO: 从 segment_downloader 获取速度
        progress.active_connections = segment_downloader_ ?
            segment_downloader_->active_connections() : 0;
    }

    if (progress.total > 0) {
        progress.progress = static_cast<double>(progress.downloaded) / progress.total;
    }

    return progress;
}

void RequestGroup::pause() {
    if (status_ == RequestGroupStatus::ACTIVE) {
        status_ = RequestGroupStatus::PAUSED;
        FALCON_LOG_INFO("暂停 RequestGroup: id=" << id_);

        if (segment_downloader_) {
            segment_downloader_->pause();
        }
    }
}

void RequestGroup::resume() {
    if (status_ == RequestGroupStatus::PAUSED) {
        status_ = RequestGroupStatus::WAITING;
        FALCON_LOG_INFO("恢复 RequestGroup: id=" << id_);

        if (segment_downloader_) {
            segment_downloader_->resume();
        }
    }
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

    // 获取原始指针
    RequestGroup* raw_ptr = group.get();

    // 添加到所有组列表（转移所有权）
    all_groups_.push_back(std::move(group));

    // 添加到映射
    group_map_[id] = raw_ptr;

    // 如果有空闲槽位，直接激活
    if (request_groups_.size() < max_concurrent_) {
        request_groups_.push_back(raw_ptr);
        raw_ptr->set_status(RequestGroupStatus::ACTIVE);
        FALCON_LOG_DEBUG("直接激活任务: id=" << id);
    } else {
        // 否则从 all_groups_ 中移除并加入等待队列
        // 找到刚添加的组并转移所有权
        for (auto it = all_groups_.begin(); it != all_groups_.end(); ++it) {
            if (it->get() == raw_ptr) {
                reserved_groups_.push_back(std::move(*it));
                all_groups_.erase(it);
                break;
            }
        }
        FALCON_LOG_DEBUG("任务加入等待队列: id=" << id);
    }
}

void RequestGroupMan::fill_request_group_from_reserver(DownloadEngineV2* engine) {
    // 当有空闲槽位时，从等待队列激活任务
    while (!reserved_groups_.empty() &&
           request_groups_.size() < max_concurrent_) {

        auto group = std::move(reserved_groups_.front());
        reserved_groups_.pop_front();

        TaskId id = group->id();
        group->set_status(RequestGroupStatus::ACTIVE);
        request_groups_.push_back(group.get());

        FALCON_LOG_INFO("从等待队列激活任务: id=" << id);

        // TODO: 创建初始命令并添加到引擎
        // auto cmd = group->create_initial_command();
        // engine->add_command(std::move(cmd));
    }
}

bool RequestGroupMan::pause_group(TaskId id) {
    auto it = group_map_.find(id);
    if (it == group_map_.end()) {
        FALCON_LOG_WARN("任务不存在: id=" << id);
        return false;
    }

    it->second->pause();
    return true;
}

bool RequestGroupMan::resume_group(TaskId id) {
    auto it = group_map_.find(id);
    if (it == group_map_.end()) {
        FALCON_LOG_WARN("任务不存在: id=" << id);
        return false;
    }

    it->second->resume();

    // 如果任务在等待队列中，尝试立即激活
    if (it->second->status() == RequestGroupStatus::WAITING) {
        // TODO: 尝试立即激活
        FALCON_LOG_DEBUG("尝试立即激活等待中的任务: id=" << id);
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
        [id](const RequestGroup* g) { return g->id() == id; });
    if (active_it != request_groups_.end()) {
        request_groups_.erase(active_it);
    }

    // 从所有组中移除
    auto all_it = std::find_if(all_groups_.begin(), all_groups_.end(),
        [id](const std::unique_ptr<RequestGroup>& g) { return g->id() == id; });
    if (all_it != all_groups_.end()) {
        all_groups_.erase(all_it);
    }

    group_map_.erase(it);

    FALCON_LOG_INFO("移除 RequestGroup: id=" << id);
    return true;
}

RequestGroup* RequestGroupMan::find_group(TaskId id) {
    auto it = group_map_.find(id);
    return it != group_map_.end() ? it->second : nullptr;
}

} // namespace falcon
