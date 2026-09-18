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
#include <falcon/protocols/segment_downloader.hpp>
#include <falcon/protocols/resume_control.hpp>
#include <falcon/protocols/commands/command.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <map>
#include <deque>
#include <unordered_map>

namespace falcon {

// 前向声明
class DownloadEngineV2;

#ifdef FAILED
#undef FAILED
#endif

#ifdef COMPLETED
#undef COMPLETED
#endif

/**
 * @brief 请求组状态 - 对应 aria2 的 RequestGroup 状态
 */
enum class RequestGroupStatus {
    WAITING,     // 等待执行（在队列中）
    ACTIVE,      // 正在下载
    PAUSED,      // 已暂停
    COMPLETED,   // 已完成
    FAILED,      // 失败（原 ERROR，避免与 Windows 宏冲突）
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
        case RequestGroupStatus::FAILED:    return "FAILED";
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
     * @param output_path_override 非空时 init() 用它覆盖自推导的
     *        输出路径（宿主化桥接注入 V1 已确定的路径）
     */
    RequestGroup(TaskId id,
                 const std::vector<std::string>& uris,
                 const DownloadOptions& options,
                 const std::string& output_path_override = {});

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
     * @brief 获取内部 DownloadTask（用于复用现有下载器/插件逻辑）
     */
    [[nodiscard]] DownloadTask::Ptr download_task() const noexcept;

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

    // ------------------------------------------------------------------
    // 多连接分段下载状态（V2 引擎）
    // ------------------------------------------------------------------

    /**
     * @brief 是否处于多分段下载模式
     */
    bool is_multi_segment() const;

    /**
     * @brief 进入多分段模式
     *
     * @param total_segments 分段总数（>1）
     */
    void begin_multi_segment(std::size_t total_segments);

    /**
     * @brief 记录一个分段结束
     *
     * @param success 该分段是否成功
     * @return true 所有分段均已结束（此后由调用方设置任务/组终态）
     */
    bool finish_segment(bool success);

    /**
     * @brief 是否存在已失败的分段
     */
    bool has_segment_failure() const;

    /**
     * @brief 复位多分段跟踪计数（回零）
     *
     * 供 abandon 后重新发起全新下载的调度路径恢复不变量：组回到
     * 非多段态后，二次 begin_multi_segment 的计数才从干净基线开始
     */
    void reset_multi_segment_tracking();

    /**
     * @brief 段级换源重试计数 +1，返回递增后的本次尝试序号
     *
     * 调度点据此判定预算：attempt > options.max_retries 时放弃该段。
     * 段号越界返回 INT_MAX（直接放弃，不重试）
     */
    int increment_segment_retry(std::size_t idx);

    /**
     * @brief 重置全部段的重试计数（begin_multi_segment /
     *        prepare_resumed_multi_segment 建立分段时调用）
     */
    void reset_segment_retries(std::size_t total_segments);

    /**
     * @brief 段已落盘进度（控制文件确认的字节数；无续传追踪或段号
     *        越界返回 0）——段级换源重试的剩余 Range 数据源
     */
    Bytes segment_progress(std::size_t idx) const;

    // ------------------------------------------------------------------
    // 断点续传状态（V2 引擎）
    // ------------------------------------------------------------------

    /**
     * @brief 注入引擎临时文件扩展名（激活时由引擎写入，供续传状态
     * 定位临时文件；未注入时为 EngineConfigV2 的默认值）
     */
    void set_temp_extension(std::string ext) { temp_extension_ = std::move(ext); }

    /**
     * @brief 临时文件路径（<输出路径><temp_extension>；扩展名为空时
     * 返回空串——无临时文件语义即无续传挂点）
     */
    std::string temp_file_path() const;

    /// 控制文件路径（<输出路径>.falcon.ctrl，固定后缀）
    std::string control_file_path() const;

    /**
     * @brief 是否持有有效的续传状态
     *
     * 两个来源：init() 从磁盘控制文件加载成功（跨会话恢复），或本次
     * 下载调度成功后 begin_resume_tracking 建立（同进程连接级重试的
     * 断点续传）。为真时 resume_plan() 可用
     */
    bool has_resume_state() const {
        std::lock_guard<std::mutex> lock(segment_mutex_);
        return resume_valid_;
    }

    /// 续传状态（仅 has_resume_state() 为真时有效）
    ResumeControl resume_plan() const {
        std::lock_guard<std::mutex> lock(segment_mutex_);
        return resume_;
    }

    /// If-Range 请求头取值（ETag 优先，否则 Last-Modified；可空）
    std::string resume_if_range() const {
        std::lock_guard<std::mutex> lock(segment_mutex_);
        return resume_if_range_value(resume_);
    }

    /**
     * @brief 建立本次下载的续传追踪（全新下载调度成功后调用）
     *
     * 记录 URL/总长/验证头/分段计划（进度全 0）并立即写控制文件。
     * 控制文件写失败只记录告警——续传是 best-effort，绝不影响下载
     */
    void begin_resume_tracking(std::string url,
                               Bytes total,
                               std::string etag,
                               std::string last_modified,
                               std::vector<ResumeSegment> segments);

    /**
     * @brief 上报分段已落盘进度（写缓冲冲刷成功后由下载命令调用）
     *
     * 单调不回退（取 max，防御乱序/重复上报）；距上次落盘控制文件
     * 不足 1 秒时只更新内存，节流写盘
     */
    void report_segment_flushed(std::size_t segment_index, Bytes downloaded);

    /**
     * @brief 立即落盘控制文件（失败收口/暂停时调用，节流旁路）
     */
    void save_resume_now();

    /**
     * @brief 结束续传追踪并删除控制文件（下载完成发布成品后调用）
     */
    void clear_resume_tracking();

    /**
     * @brief 放弃续传（服务器对续传请求回了不兼容响应：内容已变更
     * 或不再支持 Range）——删除控制文件、清空续传状态，后续全新下载
     */
    void abandon_resume();

    /**
     * @brief 恢复多分段下载的组级状态（幂等）
     *
     * 已处于多分段模式时不做任何事（同进程连接级重试不会走到这里：
     * 多连接任务不参与连接级重试）；否则按续传计划重建分段跟踪——
     * 已完成的段立即计入完成数，并把各段断点进度预置进组聚合值
     *（此后下载命令只上报增量）
     */
    void prepare_resumed_multi_segment();

private:
    TaskId id_;
    RequestGroupStatus status_ = RequestGroupStatus::WAITING;
    std::vector<std::string> uris_;
    std::size_t current_uri_index_ = 0;
    DownloadOptions options_;
    /// 非空时 init() 以此覆盖自推导输出路径（宿主化桥接注入）
    std::string output_path_override_;
    std::vector<FileInfo> files_;
    std::unique_ptr<SegmentDownloader> segment_downloader_;
    DownloadTask::Ptr download_task_;

    // 下载状态
    Bytes downloaded_bytes_ = 0;
    std::string error_message_;

    // 多分段下载跟踪（多连接模式；单段模式 total_ == 0）
    mutable std::mutex segment_mutex_;
    std::size_t segment_total_ = 0;
    std::size_t segment_finished_ = 0;
    bool segment_failure_ = false;
    /// 各段换源重试计数（与 segment_* 同锁；建立分段时重置）
    std::vector<int> segment_retry_counts_;

    // 断点续传状态（segment_mutex_ 保护；引擎单线程执行命令，pause
    // 等控制入口来自其他线程）。resume_valid_ 为真时 resume_ 记录
    // URL/验证头/分段计划与各段已落盘进度，last_ctrl_save_ 用于
    // 控制文件写盘节流
    std::string temp_extension_ = ".falcon.tmp";
    ResumeControl resume_;
    bool resume_valid_ = false;
    std::chrono::steady_clock::time_point last_ctrl_save_{};

    /// init() 阶段尝试从磁盘控制文件恢复续传状态（严格校验，失败即
    /// 清理控制文件按全新下载处理）
    void try_load_resume_state(const std::string& url);

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
    std::size_t active_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_groups_.size();
    }

    /**
     * @brief 获取等待组数量
     */
    std::size_t waiting_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reserved_groups_.size();
    }

    /**
     * @brief 检查是否所有组已完成
     */
    bool all_completed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& group : all_groups_) {
            if (!group) continue;
            auto st = group->status();
            if (st != RequestGroupStatus::COMPLETED &&
                st != RequestGroupStatus::FAILED &&
                st != RequestGroupStatus::REMOVED) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 是否全部组已安顿（无 WAITING/ACTIVE 组）
     *
     * 宿主排水判据（V2EngineHost::shutdown_and_join）：pause_all 后
     * 所有组应固化为 PAUSED（断点已保存）或终态，全部安顿后才能收
     * 引擎。与 all_completed 不同，PAUSED 视为已安顿
     */
    bool all_settled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& group : all_groups_) {
            if (!group) continue;
            const auto st = group->status();
            if (st == RequestGroupStatus::WAITING ||
                st == RequestGroupStatus::ACTIVE) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 设置最大并发数
     */
    void set_max_concurrent(std::size_t max_concurrent) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_concurrent_ = max_concurrent;
    }

    /**
     * @brief 获取最大并发数
     */
    std::size_t max_concurrent() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_concurrent_;
    }

    /**
     * @brief 获取所有请求组（用于遍历）
     */
    const std::vector<std::unique_ptr<RequestGroup>>& all_groups() const {
        return all_groups_;
    }

    /**
     * @brief 清理活动队列中的已完成任务，释放并发槽位
     */
    void cleanup_finished_active();

    /**
     * @brief 回收终态组（COMPLETED/FAILED/REMOVED），释放 all_groups_
     *        持有的对象
     *
     * 宿主化后引擎常驻（wait_when_idle），终态组不再随 run() 退出
     * 而销毁，不回收则 all_groups_/group_map_ 无界增长。PAUSED 与
     * 调度中的组不受影响。被回收组在锁外析构，find_group 随之返回
     * nullptr
     */
    void purge_finished_groups();

private:
    std::size_t max_concurrent_;
    mutable std::mutex mutex_;

    // 活动组（正在下载）- 存储原始指针指向 all_groups_
    std::vector<RequestGroup*> request_groups_;

    // 等待队列（FIFO）- 存储原始指针指向 all_groups_
    std::deque<RequestGroup*> reserved_groups_;

    // 所有组（用于查找和管理，不保证顺序）
    std::vector<std::unique_ptr<RequestGroup>> all_groups_;

    // ID 到组的映射（用于快速查找）
    std::unordered_map<TaskId, RequestGroup*> group_map_;
};

} // namespace falcon
