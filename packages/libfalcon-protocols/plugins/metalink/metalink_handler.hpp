/**
 * @file metalink_handler.hpp
 * @brief Metalink(.meta4/.metalink,RFC 5854)协议处理器
 * @author Falcon Team
 * @date 2026-09-18
 *
 * 阶段1 架构(委托模式,thunder 蓝本):
 *   解析 .meta4 → 镜像按优先级排序 → 逐个委托 HTTP/FTP handler
 *   (继承 V1 分段/续传/限速能力)→ 成功后流式哈希校验 → 校验通过
 *   才 rename 发布并置 Completed;失败换下一镜像,全灭才 FAILED。
 *
 * 事件语义(影子子任务 + 防火墙):委托时自建的影子 DownloadTask
 * 不进 TaskManager(无监听者),挂 MetalinkDelegateListener 吞掉
 * on_status_changed、透传进度/文件信息/错误到 parent 的监听链——
 * parent 的事件序列恒为「一次 Downloading → 一次 Completed/Failed」,
 * 与普通 HTTP 任务无异。影子 id 取 parent id(同一时刻每个 parent
 * 至多一个活跃镜像,天然唯一,防火墙因此无需重写 task_id)。
 *
 * 完成不变式:rename 先于 parent->set_status(Completed)——监听者
 * 看到完成时成品必然已就位且哈希校验通过(「完成=可信」)。校验
 * 失败的数据绝不 rename,删除半成品后换下一镜像。
 */

#pragma once

#include <falcon/protocol_handler.hpp>
#include <falcon/protocol_handler_extension.hpp>
#include <falcon/protocols/file_hash.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace falcon {

// Forward declaration
class ProtocolRegistry;

namespace protocols::metalink {

/// 优先级占位:RFC 5854 缺省 priority 视为最低
constexpr int kNoPriority = 2147483647;

/**
 * @struct MetalinkUrl
 * @brief 单个镜像 URL 及其排序属性
 */
struct MetalinkUrl {
    std::string url;
    std::string type;      ///< type 属性(http/ftp/bittorrent/...)
    std::string location;  ///< 地理位置 提示(仅记录,阶段1 不参与排序)
    int priority = kNoPriority;   ///< meta4:数值越小越优先
    int preference = -1;          ///< metalink3:数值越大越优先
};

/**
 * @struct MetalinkFile
 * @brief metalink 文档中一个 <file> 条目
 */
struct MetalinkFile {
    std::string name;              ///< 最终文件名(已拒绝路径穿越)
    std::uint64_t size = 0;        ///< 预期字节数(0 = 未知)
    /// 整文件哈希 {期望值(原样大小写), 算法};未知 type 已跳过
    std::vector<std::pair<std::string, HashAlgorithm>> hashes;
    std::vector<MetalinkUrl> urls; ///< 已按优先级排序的镜像列表
    // <pieces> 分片哈希阶段1 忽略(委托模式下整文件校验已足够)
};

/**
 * @class MetalinkFileParser
 * @brief metalink XML 文档 → MetalinkFile 列表
 *
 * 同时兼容 RFC 5854(.meta4,priority 升序)与 Metalink3
 * (.metalink,preference 降序)。解析失败(非法 XML/根元素不是
 * metalink/无 file/name 缺失或路径穿越/无可用镜像)抛
 * std::runtime_error(含 XmlParseError 子类,带行列号)。
 */
class MetalinkFileParser {
public:
    /// 解析文档,返回全部 file 条目(通常 1 个)
    static std::vector<MetalinkFile> parse(const std::string& xml_text);

    /// metalink hash type 属性值 → 算法;未知返回 false
    static bool hash_type_to_algorithm(const std::string& type,
                                       HashAlgorithm& out);
};

/**
 * @class MetalinkDelegateListener
 * @brief 事件防火墙:屏蔽影子任务状态变化,透传其余事件
 *
 * on_status_changed 吞掉(影子任务进终态不得污染 parent 的状态机,
 * parent 终态只由 MetalinkHandler 在校验/收口后统一驱动);
 * on_progress/on_file_info/on_error 透传给 parent 监听者。
 * 影子 id = parent id,task_id 无需重写。
 */
class MetalinkDelegateListener final : public IEventListener {
public:
    MetalinkDelegateListener(TaskId task_id, IEventListener* upstream)
        : task_id_(task_id), upstream_(upstream) {}

    void on_status_changed(TaskId, TaskStatus, TaskStatus) override {
        // 吞掉:影子任务的状态变化与 parent 无关
    }
    void on_progress(const ProgressInfo& info) override {
        if (upstream_) upstream_->on_progress(info);
    }
    void on_error(TaskId, const std::string& message) override {
        if (upstream_) upstream_->on_error(task_id_, message);
    }
    void on_file_info(TaskId, const FileInfo& info) override {
        if (upstream_) upstream_->on_file_info(task_id_, info);
    }
    BytesPerSecond query_speed_limit(TaskId) override {
        return upstream_ ? upstream_->query_speed_limit(task_id_) : 0;
    }

private:
    TaskId task_id_;
    IEventListener* upstream_;
};

/**
 * @class MetalinkHandler
 * @brief Metalink 协议处理器
 */
class MetalinkHandler : public IProtocolHandler,
                        public IProtocolHandlerExtension {
public:
    /// 构造/析构 out-of-line(cpp 里 = default):contexts_ 持
    /// unique_ptr<ActiveContext>,保持头文件零隐式实例化依赖
    MetalinkHandler();
    ~MetalinkHandler() override;

    MetalinkHandler(const MetalinkHandler&) = delete;
    MetalinkHandler& operator=(const MetalinkHandler&) = delete;

    [[nodiscard]] std::string protocol_name() const override {
        return "metalink";
    }

    [[nodiscard]] std::vector<std::string> supported_schemes() const override;

    /// .meta4/.metalink 后缀(忽略 query/fragment)、file:// 与裸本地路径
    [[nodiscard]] bool can_handle(const std::string& url) const override;

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                         const DownloadOptions& options)
        override;

    void download(DownloadTask::Ptr task, IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;

    void resume(DownloadTask::Ptr task, IEventListener* listener) override;

    void cancel(DownloadTask::Ptr task) override;

    /// 继承底层 HTTP handler 的续传能力(镜像下载走同一数据面)
    [[nodiscard]] bool supports_resume() const override { return true; }

    /// 排在 http(100)/hls(40) 之前,保证 http(s) URL 上 .meta4 后缀优先路由
    [[nodiscard]] int priority() const override { return 30; }

    void set_protocol_registry(ProtocolRegistry* registry) override {
        registry_ = registry;
    }

private:
    /// 委托期间的活跃上下文(pause/cancel 转发到底层的锚点)。
    /// 必须在此完整定义:contexts_ 的 unique_ptr 在头文件 inline
    /// 构造/清理路径(EH)需要完整类型,仅前置声明会炸消费 TU
    struct ActiveContext {
        std::mutex shadow_mutex;    // 保护 shadow / active_target
        std::shared_ptr<DownloadTask> shadow;  // 当前受托影子任务
        IProtocolHandler* active_target = nullptr;
    };

    /// 取活动上下文(不存在返回 nullptr)
    ActiveContext* find_context(TaskId task_id);

    /// 获取 metalink 文档内容:本地直接读,http(s) 经 HTTP handler 抓取;
    /// context 非空时把抓取影子注册进上下文(pause/cancel 可转发中止)
    std::string fetch_metalink_document(const std::string& url,
                                        const DownloadOptions& options,
                                        TaskId task_id,
                                        IEventListener* listener,
                                        ActiveContext* context);

    /// 委托单个镜像到 target handler(同步阻塞);成功 true,被暂停/
    /// 取消 false;其余失败抛异常换下一镜像
    bool run_mirror(ActiveContext* context, IProtocolHandler* target,
                    const DownloadTask::Ptr& parent,
                    IEventListener* listener, const std::string& mirror_url,
                    const std::string& part_path);

    ProtocolRegistry* registry_ = nullptr;

    std::mutex mutex_;
    std::map<TaskId, std::unique_ptr<ActiveContext>> contexts_;
};

/// Factory(与其他内置插件同款注册形态)
std::unique_ptr<IProtocolHandler> create_metalink_handler();

} // namespace protocols::metalink

} // namespace falcon
