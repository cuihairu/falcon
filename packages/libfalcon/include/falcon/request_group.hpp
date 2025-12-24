/**
 * @file request_group.hpp
 * @brief 请求组 - aria2 风格的下载任务管理
 * @author Falcon Team
 * @date 2025-12-24
 *
 * 设计参考: aria2/src/RequestGroup.h
 * @see https://github.com/aria2/aria2/blob/master/src/RequestGroup.h
 */

#pragma once

#include <falcon/types.hpp>
#include <falcon/download_options.hpp>
#include <falcon/segment_downloader.hpp>
#include <falcon/commands/command.hpp>

#include <memory>
#include <vector>
#include <string>
#include <map>

namespace falcon {

// 前向声明
class DownloadEngineV2;

/**
 * @brief 请求组状态 - 对应 aria2 的 RequestGroup 状态
 */
enum class RequestGroupStatus {
    WAITING,     // 等待执行（在队列中）
    ACTIVE,      // 正在下载
    PAUSED,      // 已暂停
    COMPLETED,   // 已完成
    ERROR,       // 错误
    REMOVED      // 已移除
};

/**
 * @brief 请求组状态转字符串
 */
inline const char* to_string(RequestGroupStatus status) {
    switch (status) {
        case RequestGroupStatus::WAITING:   return "WAITING";
        case RequestGroupStatus::ACTIVE:    return "ACTIVE";
        case RequestGroupStatus::PAUSED:    return "PAUSED";
        case RequestGroupStatus::COMPLETED: return "COMPLETED";
        case RequestGroupStatus::ERROR:     return "ERROR";
        case RequestGroupStatus::REMOVED:   return "REMOVED";
        default: return "UNKNOWN";
    }
}

/**
 * @brief 请求组 - 表示一个下载任务
 *
 * 对应 aria2 的 RequestGroup 类
 * 一个 RequestGroup 可以包含多个文件（如 BitTorrent）
 *
 * 职责:
 * 1. 管理下载任务的完整生命周期
 * 2. 创建和管理分段下载
 * 3. 协调多个命令的执行
 * 4. 跟踪下载进度和状态
 */
class RequestGroup {
public:
    /**
     * @brief 构造函数
     *
     * @param id 任务 ID
     * @param uris URI 列表（支持镜像/备用地址）
     * @param options 下载选项
     */
    RequestGroup(TaskId id,
                 const std::vector<std::string>& uris,
                 const DownloadOptions& options);

    ~RequestGroup();

    // 禁止拷贝，允许移动
    RequestGroup(const RequestGroup&) = delete;
    RequestGroup& operator=(const RequestGroup&) = delete;
    RequestGroup(RequestGroup&&) = default;
    RequestGroup& operator=(RequestGroup&&) = default;

    /**
     * @brief 初始化下载
     *
     * 验证 URI，获取文件信息，准备下载环境
     */
    bool init();

    /**
     * @brief 创建初始命令
     *
     * 根据协议类型创建第一个执行的命令
     */
    std::unique_ptr<Command> create_initial_command();

    /**
     * @brief 获取/设置状态
     */
    RequestGroupStatus status() const noexcept { return status_; }
    void set_status(RequestGroupStatus status) { status_ = status; }

    /**
     * @brief 获取任务 ID
     */
    TaskId id() const noexcept { return id_; }

    /**
     * @brief 获取 URI 列表
     */
    const std::vector<std::string>& uris() const noexcept { return uris_; }

    /**
     * @brief 获取当前使用的 URI（索引）
     */
    const std::string& current_uri() const noexcept { return uris_[current_uri_index_]; }

    /**
     * @brief 切换到下一个 URI（用于镜像/备用）
     */
    bool try_next_uri() {
        if (current_uri_index_ + 1 < uris_.size()) {
            current_uri_index_++;
            return true;
        }
        return false;
    }

    /**
     * @brief 获取文件列表
     */
    const std::vector<FileInfo>& files() const { return files_; }

    /**
     * @brief 获取主要文件信息（单文件下载）
     */
    const FileInfo& file_info() const { return files_.empty() ? empty_file_ : files_[0]; }

    /**
     * @brief 获取下载选项
     */
    const DownloadOptions& options() const noexcept { return options_; }

    /**
     * @brief 获取分段下载器
     */
    SegmentDownloader* segment_downloader() { return segment_downloader_.get(); }

    /**
     * @brief 下载进度信息
     */
    struct Progress {
        Bytes downloaded = 0;
        Bytes total = 0;
        double progress = 0.0;
        Speed speed = 0;
        std::size_t active_connections = 0;
    };
    Progress get_progress() const;

    /**
     * @brief 检查是否完成
     */
    bool is_completed() const {
        return status_ == RequestGroupStatus::COMPLETED;
    }

    /**
     * @brief 检查是否下载中
     */
    bool is_active() const {
        return status_ == RequestGroupStatus::ACTIVE;
    }

    /**
     * @brief 暂停下载
     */
    void pause();

    /**
     * @brief 恢复下载
     */
    void resume();

    /**
     * @brief 获取错误信息
     */
    const std::string& error_message() const noexcept { return error_message_; }
    void set_error_message(const std::string& msg) { error_message_ = msg; }

    /**
     * @brief 添加文件到列表
     */
    void add_file(const FileInfo& file) {
        files_.push_back(file);
    }

    /**
     * @brief 设置文件总大小
     */
    void set_total_size(Bytes size) {
        if (!files_.empty()) {
            files_[0].total_size = size;
        }
    }

    /**
     * @brief 获取已下载字节数
     */
    Bytes downloaded_bytes() const noexcept { return downloaded_bytes_; }

    /**
     * @brief 增加已下载字节数
     */
    void add_downloaded_bytes(Bytes bytes) {
        downloaded_bytes_ += bytes;
    }

private:
    TaskId id_;
    RequestGroupStatus status_ = RequestGroupStatus::WAITING;
    std::vector<std::string> uris_;
    std::size_t current_uri_index_ = 0;
    DownloadOptions options_;
    std::vector<FileInfo> files_;
    std::unique_ptr<SegmentDownloader> segment_downloader_;

    // 下载状态
    Bytes downloaded_bytes_ = 0;
    std::string error_message_;

    // 空文件引用（用于 files_ 为空的情况）
    static FileInfo empty_file_;
};

/**
 * @brief 请求组管理器 - aria2 风格
 *
 * 管理多个 RequestGroup 的生命周期，负责：
 * - 队列管理（等待队列 vs 活动队列）
 * - 并发控制（最大同时下载数）
 * - 任务调度（优先级、等待时间）
 * - 状态持久化
 */
class RequestGroupMan {
public:
    /**
     * @brief 构造函数
     *
     * @param max_concurrent 最大并发下载数
     */
    explicit RequestGroupMan(std::size_t max_concurrent = 5);

    ~RequestGroupMan() = default;

    // 禁止拷贝和移动
    RequestGroupMan(const RequestGroupMan&) = delete;
    RequestGroupMan& operator=(const RequestGroupMan&) = delete;

    /**
     * @brief 添加请求组
     *
     * @param group 请求组（移动语义）
     */
    void add_request_group(std::unique_ptr<RequestGroup> group);

    /**
     * @brief 从保留队列激活请求组
     *
     * 当有空闲槽位时，自动从等待队列激活任务
     */
    void fill_request_group_from_reserver(DownloadEngineV2* engine);

    /**
     * @brief 暂停请求组
     *
     * @param id 任务 ID
     * @return true 成功，false 任务不存在
     */
    bool pause_group(TaskId id);

    /**
     * @brief 恢复请求组
     *
     * @param id 任务 ID
     * @return true 成功，false 任务不存在
     */
    bool resume_group(TaskId id);

    /**
     * @brief 移除请求组
     *
     * @param id 任务 ID
     * @return true 成功，false 任务不存在
     */
    bool remove_group(TaskId id);

    /**
     * @brief 获取请求组
     *
     * @param id 任务 ID
     * @return 请求组指针，不存在返回 nullptr
     */
    RequestGroup* find_group(TaskId id);

    /**
     * @brief 获取活动组数量
     */
    std::size_t active_count() const { return request_groups_.size(); }

    /**
     * @brief 获取等待组数量
     */
    std::size_t waiting_count() const { return reserved_groups_.size(); }

    /**
     * @brief 检查是否所有组已完成
     */
    bool all_completed() const {
        return request_groups_.empty() && reserved_groups_.empty();
    }

    /**
     * @brief 设置最大并发数
     */
    void set_max_concurrent(std::size_t max_concurrent) {
        max_concurrent_ = max_concurrent;
    }

    /**
     * @brief 获取最大并发数
     */
    std::size_t max_concurrent() const {
        return max_concurrent_;
    }

    /**
     * @brief 获取所有请求组（用于遍历）
     */
    const std::vector<std::unique_ptr<RequestGroup>>& all_groups() const {
        return all_groups_;
    }

private:
    std::size_t max_concurrent_;

    // 活动组（正在下载）- 存储原始指针指向 all_groups_
    std::vector<RequestGroup*> request_groups_;

    // 等待队列（按优先级排序）- 存储 unique_ptr
    std::deque<std::unique_ptr<RequestGroup>> reserved_groups_;

    // 所有组（用于查找和管理，不保证顺序）
    std::vector<std::unique_ptr<RequestGroup>> all_groups_;

    // ID 到组的映射（用于快速查找）
    std::unordered_map<TaskId, RequestGroup*> group_map_;
};

} // namespace falcon
