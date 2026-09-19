// Falcon Task Manager Edge Case Tests (coverage batch H)
//
// 覆盖 task_manager.cpp 的三类缺口：
//   1. 状态持久化容错：save/load 的损坏文件解析防御（截断行/坏版本/
//      非法 id/重复 id/越界优先级/活动状态净化）与 auto-save 异步保存链
//   2. 任务注册与调度防御：add_task 非法/重复 id、start_task 拒绝分支、
//      worker 出队过期条目
//   3. 公共 API 转发层：set_state_file / on_task_status_changed /
//      on_task_progress / 双参 start_task
//
// 损坏状态文件以行式手工构造（save_state version-2 行格式的镜像），
// 每行独立解析失败互不影响，一批行即可命中全部容错分支。

#include <falcon/task_manager.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/protocol_handler.hpp>
#include <falcon/download_task.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using falcon::DownloadOptions;
using falcon::DownloadTask;
using falcon::EventDispatcher;
using falcon::FileInfo;
using falcon::IEventListener;
using falcon::IProtocolHandler;
using falcon::ProgressInfo;
using falcon::TaskId;
using falcon::TaskManager;
using falcon::TaskManagerConfig;
using falcon::TaskPriority;
using falcon::TaskStatus;

std::filesystem::path unique_temp_file(const std::string& prefix) {
    static std::atomic<unsigned> counter{0};
    auto base = std::filesystem::temp_directory_path();
    auto name = prefix +
        std::to_string(static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count())) +
        "_" + std::to_string(counter.fetch_add(1));
    return base / name;
}

std::string q(const std::string& s) { return '"' + s + '"'; }

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += ' ';
        out += p;
    }
    return out;
}

// save_state 行格式中的 options 字段序列（version 2），与序列化顺序
// 一一对应；损坏行变体通过截断该序列精确对准每个解析失败分支。
std::vector<std::string> option_field_groups(const DownloadOptions& o) {
    auto b = [](bool v) { return v ? "1" : "0"; };
    return {
        std::to_string(o.max_connections),        // 0
        std::to_string(o.timeout_seconds),        // 1
        std::to_string(o.max_retries),            // 2
        std::to_string(o.retry_delay_seconds),    // 3
        q(o.output_directory),                    // 4
        q(o.output_filename),                     // 5
        std::to_string(o.speed_limit),            // 6
        b(o.resume_enabled),                      // 7
        q(o.user_agent),                          // 8
        q(o.proxy),                               // 9
        q(o.proxy_type),                          // 10
        q(o.proxy_username),                      // 11
        q(o.proxy_password),                      // 12
        b(o.verify_ssl),                          // 13
        q(o.referer),                             // 14
        q(o.cookie_file),                         // 15
        q(o.cookie_jar),                          // 16
        std::to_string(o.min_segment_size),       // 17
        b(o.adaptive_segment_sizing),             // 18
        std::to_string(o.progress_interval_ms),   // 19
        b(o.create_directory),                    // 20
        b(o.overwrite_existing),                  // 21
    };
}

// 行首部：tag id status downloaded total speed priority url output error
std::string basic_task_prefix(TaskId id, int status_int, int priority_int,
                              const std::string& url = "https://example.com/file.bin",
                              const std::string& error_message = "") {
    std::ostringstream os;
    os << "task " << id << ' ' << status_int << " 100 200 0 " << priority_int
       << ' ' << q(url) << ' ' << q("/tmp/file.bin") << ' ' << q(error_message);
    return os.str();
}

// 完整合法的 version-2 任务行（options 全默认 + header_count=0）
std::string full_task_line(TaskId id, int status_int, int priority_int,
                           const std::string& url = "https://example.com/file.bin") {
    auto groups = option_field_groups(DownloadOptions{});
    groups.push_back("0");  // header_count
    return basic_task_prefix(id, status_int, priority_int, url) + " " + join(groups);
}

void write_state_file(const std::filesystem::path& path,
                      const std::vector<std::string>& task_lines, int version = 2) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << "falcon_task_state " << version << "\n";
    for (const auto& line : task_lines) {
        out << line << "\n";
    }
}

DownloadTask::Ptr make_task(TaskId id, const std::string& url,
                            const DownloadOptions& options = {}) {
    return std::make_shared<DownloadTask>(id, url, options);
}

template <typename Done>
bool wait_until(Done&& done, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

class RecordingListener final : public IEventListener {
public:
    std::atomic<int> status_calls{0};
    std::atomic<int> progress_calls{0};
    std::atomic<int> completed_calls{0};

    void on_status_changed(TaskId, TaskStatus, TaskStatus) override {
        status_calls.fetch_add(1);
    }
    void on_progress(const ProgressInfo&) override { progress_calls.fetch_add(1); }
    void on_completed(TaskId, const std::string&) override {
        completed_calls.fetch_add(1);
    }

    template <typename Done>
    bool wait_for(Done&& done, std::chrono::milliseconds timeout) const {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    }
};

TaskManagerConfig base_config() {
    TaskManagerConfig cfg;
    cfg.auto_save_state = false;
    cfg.cleanup_interval = std::chrono::seconds(0);
    return cfg;
}

// ============================================================================
// 任务注册防御
// ============================================================================

TEST(TaskManagerEdgesTest, AddTaskRejectsInvalidId) {
    TaskManager tm(base_config(), nullptr);

    auto task = make_task(falcon::INVALID_TASK_ID, "https://example.com/a.bin");
    EXPECT_EQ(tm.add_task(task, TaskPriority::Normal), falcon::INVALID_TASK_ID);
    EXPECT_EQ(tm.get_all_tasks().size(), 0u);
}

TEST(TaskManagerEdgesTest, AddTaskRejectsDuplicateId) {
    TaskManager tm(base_config(), nullptr);

    auto task = make_task(61, "https://example.com/a.bin");
    EXPECT_EQ(tm.add_task(task, TaskPriority::Normal), 61u);
    EXPECT_EQ(tm.add_task(task, TaskPriority::High), falcon::INVALID_TASK_ID);
    EXPECT_EQ(tm.get_all_tasks().size(), 1u);
}

// ============================================================================
// 调度防御
// ============================================================================

TEST(TaskManagerEdgesTest, StartTaskRejections) {
    TaskManager tm(base_config(), nullptr);

    // 不存在的任务（单参重载内部取任务优先级后转发双参实现）
    EXPECT_FALSE(tm.start_task(999));

    // 已终结的任务拒绝重新启动
    auto done = make_task(51, "https://example.com/done.bin");
    ASSERT_EQ(tm.add_task(done, TaskPriority::Normal), 51u);
    done->set_status(TaskStatus::Cancelled);
    EXPECT_FALSE(tm.start_task(51));
    EXPECT_FALSE(tm.start_task(51, TaskPriority::High));

    // 已入队的任务拒绝重复调度
    auto queued = make_task(52, "https://example.com/queued.bin");
    ASSERT_EQ(tm.add_task(queued, TaskPriority::Normal), 52u);
    EXPECT_TRUE(tm.start_task(52));
    EXPECT_EQ(tm.get_queue_size(), 1u);
    EXPECT_FALSE(tm.start_task(52));

    // 双参公共重载：显式优先级入队
    auto explicit_prio = make_task(53, "https://example.com/prio.bin");
    ASSERT_EQ(tm.add_task(explicit_prio, TaskPriority::Normal), 53u);
    EXPECT_TRUE(tm.start_task(53, TaskPriority::High));
    EXPECT_EQ(explicit_prio->get_priority(), TaskPriority::High);
}

TEST(TaskManagerEdgesTest, WorkerSkipsFinishedTaskDequeuedFromQueue) {
    TaskManager tm(base_config(), nullptr);

    auto task = make_task(41, "https://example.com/stale.bin");
    ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 41u);
    ASSERT_TRUE(tm.start_task(41));
    ASSERT_EQ(tm.get_queue_size(), 1u);

    // 绕过 TaskManager 直接终结任务（cancel_task 会清理入队版本，
    // 直接改状态则条目过期滞留队列）——worker 出队后必须跳过而非启动
    task->set_status(TaskStatus::Cancelled);

    tm.start();
    EXPECT_TRUE(wait_until([&] { return tm.get_queue_size() == 0; },
                           std::chrono::milliseconds(2000)));
    tm.stop();

    // 过期条目被丢弃，任务保持终结态从未被启动
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
    EXPECT_EQ(tm.get_active_task_count(), 0u);
}

TEST(TaskManagerEdgesTest, WorkerFailsTaskWithoutHandler) {
    TaskManager tm(base_config(), nullptr);

    // 任务未绑定协议处理器：worker 启动后在下载线程内收口为 Failed
    auto task = make_task(43, "https://example.com/nohandler.bin");
    ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 43u);
    ASSERT_TRUE(tm.start_task(43));

    tm.start();
    EXPECT_TRUE(wait_until(
        [&] { return task->status() == TaskStatus::Failed; },
        std::chrono::milliseconds(3000)));
    tm.stop();

    EXPECT_NE(task->error_message().find("No protocol handler"),
              std::string::npos);
}

TEST(TaskManagerEdgesTest, StopCancelsActiveDownloads) {
    class BlockUntilCancelledHandler final : public IProtocolHandler {
    public:
        std::string protocol_name() const override { return "block_until_cancelled"; }
        std::vector<std::string> supported_schemes() const override { return {"https"}; }
        bool can_handle(const std::string& url) const override {
            return url.rfind("https://", 0) == 0;
        }

        void download(DownloadTask::Ptr task, IEventListener*) override {
            task->set_status(TaskStatus::Downloading);
            started_.store(true);
            // 挂起直到被取消（TaskManager::stop 会先取消全部活动任务）
            wait_until([&] { return task->status() == TaskStatus::Cancelled; },
                       std::chrono::milliseconds(5000));
            cancelled_.store(task->status() == TaskStatus::Cancelled);
        }

        FileInfo get_file_info(const std::string& url,
                               const DownloadOptions&) override {
            FileInfo info;
            info.url = url;
            return info;
        }

        void pause(DownloadTask::Ptr task) override {
            task->set_status(TaskStatus::Paused);
        }

        void resume(DownloadTask::Ptr task, IEventListener* listener) override {
            download(std::move(task), listener);
        }

        void cancel(DownloadTask::Ptr task) override {
            task->set_status(TaskStatus::Cancelled);
        }

        bool wait_started() const {
            return wait_until([&] { return started_.load(); },
                              std::chrono::milliseconds(3000));
        }
        bool observed_cancel() const { return cancelled_.load(); }

    private:
        std::atomic<bool> started_{false};
        std::atomic<bool> cancelled_{false};
    };

    auto handler = std::make_shared<BlockUntilCancelledHandler>();
    auto task = make_task(44, "https://example.com/stop.bin");
    task->set_handler(handler);
    task->mark_started();  // 模拟引擎启动的元数据

    TaskManager tm(base_config(), nullptr);
    ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 44u);
    ASSERT_TRUE(tm.start_task(44));
    tm.start();
    ASSERT_TRUE(handler->wait_started());
    ASSERT_EQ(tm.get_active_task_count(), 1u);

    // 停机：先取消活动下载再拆除调度线程
    tm.stop();

    EXPECT_TRUE(handler->observed_cancel());
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
}

/// handler->download 抛异常：worker 内层 catch 收口 set_error + Failed
///（与"无 handler"同一路径，但异常源是 handler 本身）
TEST(TaskManagerEdgesTest, WorkerCatchesHandlerExceptionAndFails) {
    class ThrowingHandler final : public IProtocolHandler {
    public:
        std::string protocol_name() const override { return "throwing"; }
        std::vector<std::string> supported_schemes() const override { return {"https"}; }
        bool can_handle(const std::string& url) const override {
            return url.rfind("https://", 0) == 0;
        }
        void download(DownloadTask::Ptr, IEventListener*) override {
            throw std::runtime_error("boom inside handler download");
        }
        FileInfo get_file_info(const std::string& url,
                               const DownloadOptions&) override {
            FileInfo info;
            info.url = url;
            return info;
        }
        void pause(DownloadTask::Ptr task) override {
            task->set_status(TaskStatus::Paused);
        }
        void resume(DownloadTask::Ptr task, IEventListener* listener) override {
            download(std::move(task), listener);
        }
        void cancel(DownloadTask::Ptr task) override {
            task->set_status(TaskStatus::Cancelled);
        }
    };

    auto handler = std::make_shared<ThrowingHandler>();
    auto task = make_task(45, "https://example.com/boom.bin");
    task->set_handler(handler);
    task->mark_started();

    TaskManager tm(base_config(), nullptr);
    ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 45u);
    ASSERT_TRUE(tm.start_task(45));
    tm.start();
    EXPECT_TRUE(wait_until(
        [&] { return task->status() == TaskStatus::Failed; },
        std::chrono::milliseconds(3000)));
    tm.stop();

    EXPECT_EQ(task->error_message(), "boom inside handler download");
}


// ============================================================================
// auto-save 异步保存链
// ============================================================================

TEST(TaskManagerEdgesTest, AddTaskTriggersAutoSave) {
    TaskManagerConfig cfg = base_config();
    cfg.auto_save_state = true;
    auto state_path = unique_temp_file("falcon_tm_add_save_");
    cfg.state_file = state_path.string();

    DownloadOptions opts;
    opts.user_agent = "Falcon/edges";

    {
        TaskManager tm(cfg, nullptr);
        auto task = make_task(7, "https://example.com/auto.bin", opts);
        ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 7u);
        // 析构前 stop() 排空异步保存线程池
        tm.stop();
    }

    EXPECT_TRUE(std::filesystem::exists(state_path));

    TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));
    auto loaded = tm2.get_task(7);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->options().user_agent, "Falcon/edges");

    std::filesystem::remove(state_path);
}

TEST(TaskManagerEdgesTest, RemoveTaskTriggersAutoSave) {
    TaskManagerConfig cfg = base_config();
    cfg.auto_save_state = true;
    auto state_path = unique_temp_file("falcon_tm_remove_save_");
    cfg.state_file = state_path.string();

    {
        TaskManager tm(cfg, nullptr);
        auto task = make_task(8, "https://example.com/rm.bin");
        ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 8u);
        task->set_status(TaskStatus::Cancelled);
        EXPECT_TRUE(tm.remove_task(8));
        tm.stop();
    }

    EXPECT_TRUE(std::filesystem::exists(state_path));
    std::filesystem::remove(state_path);
}

TEST(TaskManagerEdgesTest, SetStateFileEnablesAutoSave) {
    TaskManagerConfig cfg = base_config();
    cfg.auto_save_state = true;  // state_file 初始为空

    auto state_path = unique_temp_file("falcon_tm_setfile_");

    {
        TaskManager tm(cfg, nullptr);
        tm.set_state_file(state_path.string());
        auto task = make_task(9, "https://example.com/setfile.bin");
        ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 9u);
        tm.stop();
    }

    EXPECT_TRUE(std::filesystem::exists(state_path));
    std::filesystem::remove(state_path);
}

TEST(TaskManagerEdgesTest, AutoSaveSkipsWhenStateFileEmpty) {
    TaskManagerConfig cfg = base_config();
    cfg.auto_save_state = true;  // state_file 保持为空

    TaskManager tm(cfg, nullptr);
    auto task = make_task(10, "https://example.com/nofile.bin");
    ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 10u);
    tm.stop();
    // 无状态文件路径时静默跳过，不崩溃
}

// ============================================================================
// 状态清理线程
// ============================================================================

TEST(TaskManagerEdgesTest, CleanupLoopRemovesFinishedTasksAndAutoSaves) {
    TaskManagerConfig cfg;
    cfg.auto_save_state = true;
    cfg.cleanup_interval = std::chrono::seconds(1);
    auto state_path = unique_temp_file("falcon_tm_cleanup_");
    cfg.state_file = state_path.string();

    {
        TaskManager tm(cfg, nullptr);
        auto task = make_task(42, "https://example.com/cleanup.bin");
        ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 42u);
        task->set_status(TaskStatus::Completed);

        tm.start();
        // 清理线程按周期回收终结任务
        EXPECT_TRUE(wait_until([&] { return tm.get_task(42) == nullptr; },
                               std::chrono::milliseconds(4000)));
        tm.stop();  // 排空异步保存
    }

    ASSERT_TRUE(std::filesystem::exists(state_path));
    // 快照产生于清理之后：重新加载得到零任务
    TaskManager tm2(cfg, nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));
    EXPECT_EQ(tm2.get_all_tasks().size(), 0u);

    std::filesystem::remove(state_path);
}

// ============================================================================
// save/load 容错
// ============================================================================

TEST(TaskManagerEdgesTest, SaveStateFailsOnUnopenablePath) {
    TaskManager tm(base_config(), nullptr);
    auto task = make_task(11, "https://example.com/save.bin");
    ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 11u);

    EXPECT_FALSE(tm.save_state("/nonexistent_dir_zz_no_such/falcon.state"));
}

TEST(TaskManagerEdgesTest, LoadStateRejectsEmptyFile) {
    auto path = unique_temp_file("falcon_tm_empty_");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
    }
    TaskManager tm(base_config(), nullptr);
    EXPECT_FALSE(tm.load_state(path.string()));
    std::filesystem::remove(path);
}

TEST(TaskManagerEdgesTest, LoadStateRejectsBadVersion) {
    TaskManager tm(base_config(), nullptr);

    auto missing = unique_temp_file("falcon_tm_nover_");
    {
        std::ofstream out(missing, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << "falcon_task_state\n";  // magic 后无版本号
    }
    EXPECT_FALSE(tm.load_state(missing.string()));

    auto unknown = unique_temp_file("falcon_tm_badver_");
    write_state_file(unknown, {}, 99);
    EXPECT_FALSE(tm.load_state(unknown.string()));

    std::filesystem::remove(missing);
    std::filesystem::remove(unknown);
}

TEST(TaskManagerEdgesTest, LoadStateSkipsTruncatedTaskData) {
    auto path = unique_temp_file("falcon_tm_trunc_basic_");
    write_state_file(path, {
        "task 200 0 100 200",        // speed 缺失
        "task 201 0 100 200 0",      // priority 缺失（version 2）
        "task 202 0 100 200 0 1",    // url/output/error 缺失
    });

    TaskManager tm(base_config(), nullptr);
    // 行级损坏跳过，文件整体接受
    EXPECT_TRUE(tm.load_state(path.string()));
    EXPECT_EQ(tm.get_all_tasks().size(), 0u);
    std::filesystem::remove(path);
}

TEST(TaskManagerEdgesTest, LoadStateSkipsTruncatedOptionFields) {
    // 每行保留 options 字段序列的一个前缀：截断点逐一扫过
    // read_download_options 的全部解析失败分支
    std::vector<std::string> lines;
    auto truncated = [&](std::size_t keep) {
        auto groups = option_field_groups(DownloadOptions{});
        groups.resize(keep);
        return basic_task_prefix(300, 0, 1) + " " + join(groups);
    };
    for (std::size_t keep : {1u, 4u, 5u, 6u, 7u, 8u, 10u, 11u, 12u, 13u,
                             14u, 17u, 18u, 19u, 20u, 21u, 22u}) {
        lines.push_back(truncated(keep));
    }
    // header_count=1 但缺少键值对：header 循环内读取失败
    {
        auto groups = option_field_groups(DownloadOptions{});
        groups.push_back("1");  // header_count，无后续键值
        lines.push_back(basic_task_prefix(301, 0, 1) + " " + join(groups));
    }

    auto path = unique_temp_file("falcon_tm_trunc_opts_");
    write_state_file(path, lines);

    TaskManager tm(base_config(), nullptr);
    EXPECT_TRUE(tm.load_state(path.string()));
    EXPECT_EQ(tm.get_all_tasks().size(), 0u);
    std::filesystem::remove(path);
}

TEST(TaskManagerEdgesTest, LoadStateSkipsInvalidIdEmptyUrlAndDuplicates) {
    auto path = unique_temp_file("falcon_tm_invalid_");
    write_state_file(path, {
        full_task_line(0, 0, 1),    // 非法任务 id
        full_task_line(302, 0, 1, ""),  // 空 URL
        full_task_line(303, 0, 1),
        full_task_line(303, 0, 1),  // 重复 id：第二行跳过
    });

    TaskManager tm(base_config(), nullptr);
    EXPECT_TRUE(tm.load_state(path.string()));
    EXPECT_EQ(tm.get_task(0), nullptr);
    EXPECT_EQ(tm.get_task(302), nullptr);
    EXPECT_NE(tm.get_task(303), nullptr);
    EXPECT_EQ(tm.get_all_tasks().size(), 1u);
    std::filesystem::remove(path);
}

TEST(TaskManagerEdgesTest, LoadStateSanitizesPriorityAndActiveStatus) {
    auto path = unique_temp_file("falcon_tm_sanitize_");
    write_state_file(path, {
        full_task_line(304, 2, 99),   // Downloading + 越界优先级
        full_task_line(305, 1, 0),    // Preparing
    });

    TaskManager tm(base_config(), nullptr);
    EXPECT_TRUE(tm.load_state(path.string()));

    // 越界优先级回落 Normal；活动状态净化为 Paused（恢复不自动启动）
    auto first = tm.get_task(304);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->status(), TaskStatus::Paused);
    EXPECT_EQ(first->get_priority(), TaskPriority::Normal);

    auto second = tm.get_task(305);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->status(), TaskStatus::Paused);

    std::filesystem::remove(path);
}

// version 3 落盘的 auto_file_renaming 尾字段往返；version 2 旧档
// 无该字段，按默认值 false 解析（升级兼容）
TEST(TaskManagerEdgesTest, AutoFileRenamingVersionGatedRoundTrip) {
    DownloadOptions opts;
    opts.auto_file_renaming = true;
    auto task = make_task(310, "https://example.com/v3.bin", opts);

    auto path = unique_temp_file("falcon_tm_v3_");
    {
        TaskManager tm(base_config(), nullptr);
        ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 310u);
        ASSERT_TRUE(tm.save_state(path.string()));
    }

    TaskManager tm2(base_config(), nullptr);
    ASSERT_TRUE(tm2.load_state(path.string()));
    auto loaded = tm2.get_task(310);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->options().auto_file_renaming);

    // version 2 行格式（无尾字段）：加载成功且新字段取默认 false
    auto v2_path = unique_temp_file("falcon_tm_v2compat_");
    write_state_file(v2_path, {full_task_line(311, 0, 1)}, 2);
    TaskManager tm3(base_config(), nullptr);
    ASSERT_TRUE(tm3.load_state(v2_path.string()));
    auto legacy = tm3.get_task(311);
    ASSERT_NE(legacy, nullptr);
    EXPECT_FALSE(legacy->options().auto_file_renaming);

    std::filesystem::remove(path);
    std::filesystem::remove(v2_path);
}

// version 4 落盘的 conditional_get 尾字段往返；version 3 旧档（v4 读取
// 方）保留 auto_file_renaming、conditional_get 按默认 false 解析
TEST(TaskManagerEdgesTest, ConditionalGetVersionGatedRoundTrip) {
    DownloadOptions opts;
    opts.conditional_get = true;
    auto task = make_task(320, "https://example.com/v4.bin", opts);

    auto path = unique_temp_file("falcon_tm_v4_");
    {
        TaskManager tm(base_config(), nullptr);
        ASSERT_EQ(tm.add_task(task, TaskPriority::Normal), 320u);
        ASSERT_TRUE(tm.save_state(path.string()));
    }

    TaskManager tm2(base_config(), nullptr);
    ASSERT_TRUE(tm2.load_state(path.string()));
    auto loaded = tm2.get_task(320);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->options().conditional_get);

    // version 3 行格式（仅有 auto_file_renaming 尾字段）：v4 读取方
    // 版本门控跳过 conditional_get（默认 false），v3 字段原样保留
    const std::string v3_line = basic_task_prefix(321, 0, 1) + " " +
                                join(option_field_groups(DownloadOptions{})) +
                                " 1 0";  // auto_file_renaming=1, header_count=0
    auto v3_path = unique_temp_file("falcon_tm_v3compat_");
    write_state_file(v3_path, {v3_line}, 3);
    TaskManager tm3(base_config(), nullptr);
    ASSERT_TRUE(tm3.load_state(v3_path.string()));
    auto legacy = tm3.get_task(321);
    ASSERT_NE(legacy, nullptr);
    EXPECT_TRUE(legacy->options().auto_file_renaming);
    EXPECT_FALSE(legacy->options().conditional_get);

    std::filesystem::remove(path);
    std::filesystem::remove(v3_path);
}

TEST(TaskManagerEdgesTest, SaveLoadRoundTripAllOptionFields) {
    DownloadOptions opts;
    opts.max_connections = 8;
    opts.timeout_seconds = 99;
    opts.max_retries = 5;
    opts.retry_delay_seconds = 7;
    opts.output_directory = "/tmp/dl dir";
    opts.output_filename = "out put.bin";
    opts.speed_limit = 123456;
    opts.resume_enabled = false;
    opts.user_agent = "Falcon/1.0 (\"edge\")";
    opts.proxy = "http://proxy.example:8080";
    opts.proxy_type = "http";
    opts.proxy_username = "user name";
    opts.proxy_password = "p@ss\"word\\";
    opts.verify_ssl = false;
    opts.referer = "https://ref.example/";
    opts.cookie_file = "/tmp/ck file.txt";
    opts.cookie_jar = "/tmp/jar.txt";
    opts.min_segment_size = 4096;
    opts.adaptive_segment_sizing = false;
    opts.progress_interval_ms = 250;
    opts.create_directory = false;
    opts.overwrite_existing = true;
    opts.auto_file_renaming = true;
    opts.conditional_get = true;
    opts.headers = {{"X-A \"quoted\"", "back\\slash"},
                    {"X-B", "space value"}};

    auto state_path = unique_temp_file("falcon_tm_roundtrip_");

    {
        TaskManager tm(base_config(), nullptr);
        auto task = make_task(71, "https://example.com/rt.bin", opts);
        task->set_output_path("/tmp/rt saved.bin");
        task->set_error("boom \"quoted\"");
        task->update_progress(300, 900, 42);
        task->set_status(TaskStatus::Paused);
        ASSERT_EQ(tm.add_task(task, TaskPriority::High), 71u);
        ASSERT_TRUE(tm.save_state(state_path.string()));
    }

    TaskManager tm2(base_config(), nullptr);
    ASSERT_TRUE(tm2.load_state(state_path.string()));

    auto loaded = tm2.get_task(71);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->url(), "https://example.com/rt.bin");
    EXPECT_EQ(loaded->output_path(), "/tmp/rt saved.bin");
    EXPECT_EQ(loaded->error_message(), "boom \"quoted\"");
    EXPECT_EQ(loaded->status(), TaskStatus::Paused);
    EXPECT_EQ(loaded->get_priority(), TaskPriority::High);
    EXPECT_EQ(loaded->downloaded_bytes(), 300u);
    EXPECT_EQ(loaded->total_bytes(), 900u);
    EXPECT_EQ(loaded->speed(), 42u);

    const auto& ro = loaded->options();
    EXPECT_EQ(ro.max_connections, 8u);
    EXPECT_EQ(ro.timeout_seconds, 99u);
    EXPECT_EQ(ro.max_retries, 5u);
    EXPECT_EQ(ro.retry_delay_seconds, 7u);
    EXPECT_EQ(ro.output_directory, "/tmp/dl dir");
    EXPECT_EQ(ro.output_filename, "out put.bin");
    EXPECT_EQ(ro.speed_limit, 123456u);
    EXPECT_FALSE(ro.resume_enabled);
    EXPECT_EQ(ro.user_agent, "Falcon/1.0 (\"edge\")");
    EXPECT_EQ(ro.proxy, "http://proxy.example:8080");
    EXPECT_EQ(ro.proxy_type, "http");
    EXPECT_EQ(ro.proxy_username, "user name");
    EXPECT_EQ(ro.proxy_password, "p@ss\"word\\");
    EXPECT_FALSE(ro.verify_ssl);
    EXPECT_EQ(ro.referer, "https://ref.example/");
    EXPECT_EQ(ro.cookie_file, "/tmp/ck file.txt");
    EXPECT_EQ(ro.cookie_jar, "/tmp/jar.txt");
    EXPECT_EQ(ro.min_segment_size, 4096u);
    EXPECT_FALSE(ro.adaptive_segment_sizing);
    EXPECT_EQ(ro.progress_interval_ms, 250u);
    EXPECT_FALSE(ro.create_directory);
    EXPECT_TRUE(ro.overwrite_existing);
    EXPECT_TRUE(ro.auto_file_renaming);
    EXPECT_TRUE(ro.conditional_get);
    EXPECT_EQ(ro.headers, opts.headers);

    std::filesystem::remove(state_path);
}

// ============================================================================
// 公共 API 转发层（事件注入）
// ============================================================================

class TaskManagerEdgesDispatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        dispatcher_ = std::make_unique<EventDispatcher>();
        dispatcher_->start();
        manager_ = std::make_unique<TaskManager>(base_config(), dispatcher_.get());
        dispatcher_->add_listener(&listener_);
    }

    void TearDown() override {
        if (manager_) manager_->stop();
        if (dispatcher_) dispatcher_->stop();
    }

    std::unique_ptr<EventDispatcher> dispatcher_;
    std::unique_ptr<TaskManager> manager_;
    RecordingListener listener_;
};

TEST_F(TaskManagerEdgesDispatchTest, OnTaskStatusChangedForwardsToDispatcher) {
    auto task = make_task(81, "https://example.com/fwd.bin");
    ASSERT_EQ(manager_->add_task(task, TaskPriority::Normal), 81u);

    // 手动注入状态迁移：Pending → Downloading 进入活动集
    manager_->on_task_status_changed(81, TaskStatus::Pending, TaskStatus::Downloading);
    EXPECT_TRUE(listener_.wait_for(
        [&] { return listener_.status_calls.load() >= 1; },
        std::chrono::milliseconds(2000)));
    EXPECT_EQ(manager_->get_active_task_count(), 1u);

    // Downloading → Completed：派发完成事件
    manager_->on_task_status_changed(81, TaskStatus::Downloading, TaskStatus::Completed);
    EXPECT_TRUE(listener_.wait_for(
        [&] { return listener_.completed_calls.load() >= 1; },
        std::chrono::milliseconds(2000)));
    EXPECT_EQ(manager_->get_active_task_count(), 0u);
}

TEST_F(TaskManagerEdgesDispatchTest, OnTaskProgressForwardsToDispatcher) {
    auto task = make_task(82, "https://example.com/prog.bin");
    ASSERT_EQ(manager_->add_task(task, TaskPriority::Normal), 82u);

    ProgressInfo info;
    info.task_id = 82;
    info.downloaded_bytes = 128;
    info.total_bytes = 512;

    manager_->on_task_progress(82, info);
    EXPECT_TRUE(listener_.wait_for(
        [&] { return listener_.progress_calls.load() >= 1; },
        std::chrono::milliseconds(2000)));
}

} // namespace
