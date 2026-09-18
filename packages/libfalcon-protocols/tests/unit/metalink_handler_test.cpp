// Metalink handler 回环端到端测试
//
// 复用共享可编程 HTTP 服务器(scripted_http_server.hpp),覆盖:
// can_handle 矩阵、registry 路由特判、本地/远程 .meta4 全链、哈希
// 校验失败换镜像、全镜像失败、慢镜像暂停、output_filename 覆盖、
// get_file_info。
//
// worker 语义复刻(task_manager.cpp:829):handler->download 前任务
// 已置 Downloading;成功后 handler 自置 Completed;失败抛异常由外层
// 收口。暂停/取消由测试侧直接置状态后调 handler->pause/cancel
// (对齐 TaskManager pause_task 的调用序列)。

#include "scripted_http_server.hpp"

#include "plugins/http/http_handler.hpp"
#include "plugins/metalink/metalink_handler.hpp"

#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/protocol_registry.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using falcon::DownloadOptions;
using falcon::DownloadTask;
using falcon::IEventListener;
using falcon::ProgressInfo;
using falcon::ProtocolRegistry;
using falcon::TaskStatus;
using falcon::protocols::HttpHandler;
using falcon::protocols::metalink::MetalinkHandler;
using falcon::testscripts::FakeResponse;
using falcon::testscripts::ScriptedHttpServer;
using falcon::testscripts::TempDir;

namespace {

// 测试向量(sha256sum 现算)
constexpr const char* kMirror1Body = "hello falcon metalink\n"; // 22 bytes
constexpr const char* kMirror1Sha256 =
    "1c6e0e149c118ed923131eb8bc659d9bb02842d2a73e74b611740244cf7c252c";
constexpr const char* kMirror1Md5 = "2552751a4c282c5a2f7d7ddb62ab60ac";
constexpr const char* kMirror2Body = "MIRROR-TWO-CONTENT\n"; // 19 bytes

/// 状态/错误/进度记录器(挂 parent,断言事件序列与防火墙)
class RecordingListener : public IEventListener {
public:
    void on_status_changed(falcon::TaskId, TaskStatus from,
                           TaskStatus to) override {
        std::lock_guard<std::mutex> lock(mutex_);
        statuses.emplace_back(from, to);
    }
    void on_progress(const ProgressInfo& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        progresses.push_back(info.task_id);
        progress_cv.notify_all();
    }
    void on_error(falcon::TaskId id, const std::string& message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        errors.emplace_back(id, message);
    }
    void on_file_info(falcon::TaskId id, const falcon::FileInfo& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        file_infos.emplace_back(id, info.filename);
    }
    falcon::BytesPerSecond query_speed_limit(falcon::TaskId) override {
        return 0;
    }

    bool wait_progress(size_t at_least, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return progress_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                    [&] { return progresses.size() >= at_least; });
    }

    std::mutex mutex_;
    std::condition_variable progress_cv;
    std::vector<std::pair<TaskStatus, TaskStatus>> statuses;
    std::vector<falcon::TaskId> progresses;
    std::vector<std::pair<falcon::TaskId, std::string>> errors;
    std::vector<std::pair<falcon::TaskId, std::string>> file_infos;
};

/// metalink 测试夹具:server + registry(http + metalink)+ handler
class MetalinkHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<ScriptedHttpServer>();
        server_->start();
        registry_ = std::make_unique<ProtocolRegistry>();
        registry_->register_handler(std::make_unique<HttpHandler>());
        handler_ = std::make_unique<MetalinkHandler>();
        handler_->set_protocol_registry(registry_.get());
    }
    void TearDown() override {
        handler_.reset();
        registry_.reset();
        server_->stop();
    }

    ScriptedHttpServer& server() { return *server_; }

    std::string local_meta4(const std::string& inner) {
        const std::string path =
            (dir_.path() / ("doc" + std::to_string(doc_count_++) + ".meta4"))
                .string();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "<?xml version=\"1.0\"?><metalink>" << inner << "</metalink>";
        return path;
    }

    std::unique_ptr<ScriptedHttpServer> server_;
    std::unique_ptr<ProtocolRegistry> registry_;
    std::unique_ptr<MetalinkHandler> handler_;
    TempDir dir_;
    int doc_count_ = 0;
};

DownloadTask::Ptr make_task(falcon::TaskId id, const std::string& url,
                            const DownloadOptions& options) {
    return std::make_shared<DownloadTask>(id, url, options);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

} // namespace

//==============================================================================
// can_handle 与 registry 路由
//==============================================================================

TEST_F(MetalinkHandlerTest, CanHandleMatrix) {
    EXPECT_TRUE(handler_->can_handle("http://a/x.meta4"));
    EXPECT_TRUE(handler_->can_handle("http://a/x.metalink"));
    EXPECT_TRUE(handler_->can_handle("HTTP://a/x.META4?q=1"));
    EXPECT_TRUE(handler_->can_handle("https://a/dir/y.metalink#frag"));
    EXPECT_TRUE(handler_->can_handle("/local/path/z.meta4"));
    EXPECT_TRUE(handler_->can_handle("file:///w.meta4"));
    EXPECT_FALSE(handler_->can_handle("http://a/x.bin"));
    EXPECT_FALSE(handler_->can_handle("http://a/x.meta"));
    EXPECT_FALSE(handler_->can_handle("http://a/?x=.meta4"));
}

TEST_F(MetalinkHandlerTest, RegistryRoutesMeta4ToMetalinkHandler) {
    // 同 registry:带 .meta4 后缀的 http URL 截获给 metalink,
    // 普通 http 不受影响
    registry_->register_handler(std::move(handler_));
    auto* routed = registry_->get_handler_for_url("http://mirror.example/a.iso.meta4");
    ASSERT_NE(routed, nullptr);
    EXPECT_EQ(routed->protocol_name(), "metalink");
    auto* plain = registry_->get_handler_for_url("http://mirror.example/a.iso");
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->protocol_name(), "http");
}

//==============================================================================
// 本地 .meta4 全链
//==============================================================================

TEST_F(MetalinkHandlerTest, LocalMeta4EndToEndEventSequence) {
    server().set_response("/m1.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    server().set_response("/m2.bin",
                          FakeResponse{200, "OK", {}, "junk", true});
    // 镜像 2 排在前面但内容与哈希不符;镜像 1 兜底成功
    const std::string doc = local_meta4(
        "<file name=\"out.bin\">"
        "<size>22</size>"
        "<hash type=\"sha-256\">" + std::string(kMirror1Sha256) + "</hash>"
        "<url priority=\"1\">" + server().url("/m2.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/m1.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(1, doc, options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading); // worker 语义

    handler_->download(task, &listener);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    // 事件序列:恰一次 Downloading → 一次 Completed;影子的状态变化
    // (Paused/Failed/Downloading)全部被防火墙吞掉
    {
        std::lock_guard<std::mutex> lock(listener.mutex_);
        ASSERT_EQ(listener.statuses.size(), 2u);
        EXPECT_EQ(listener.statuses[0].first, TaskStatus::Pending);
        EXPECT_EQ(listener.statuses[0].second, TaskStatus::Downloading);
        EXPECT_EQ(listener.statuses[1].second, TaskStatus::Completed);
    }
    // 成品逐字节一致
    const std::string final_path = (dir_.path() / "out.bin").string();
    EXPECT_EQ(read_file(final_path), kMirror1Body);
    // part 文件消失
    EXPECT_FALSE(fs::exists(final_path + ".metalink-part"));
    // 失败镜像恰一次尝试(HEAD 探测 + GET = 2 次请求),好镜像接管
    EXPECT_EQ(server().count_requests("/m2.bin"), 2u);
    EXPECT_GE(server().count_requests("/m1.bin"), 2u);
}

TEST_F(MetalinkHandlerTest, OutputFilenameOptionOverridesDocName) {
    server().set_response("/m1.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    const std::string doc = local_meta4(
        "<file name=\"doc-name.bin\">"
        "<url>" + server().url("/m1.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.output_filename = "renamed.bin";
    auto task = make_task(2, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "renamed.bin").string()), kMirror1Body);
    EXPECT_FALSE(fs::exists((dir_.path() / "doc-name.bin").string()));
}

TEST_F(MetalinkHandlerTest, BadHashFallsToNextMirror) {
    // 镜像 1 可下载但内容与哈希不符 → 校验失败删 part → 镜像 2 成功
    server().set_response("/bad.bin",
                          FakeResponse{200, "OK", {}, "corrupted!", true});
    server().set_response("/good.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    const std::string doc = local_meta4(
        "<file name=\"out.bin\">"
        "<hash type=\"sha-256\">" + std::string(kMirror1Sha256) + "</hash>"
        "<url priority=\"1\">" + server().url("/bad.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/good.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(3, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "out.bin").string()), kMirror1Body);
    // 失败镜像恰一次尝试(HEAD + GET),好镜像接管
    EXPECT_EQ(server().count_requests("/bad.bin"), 2u);
    EXPECT_GE(server().count_requests("/good.bin"), 2u);
    EXPECT_FALSE(fs::exists((dir_.path() / "out.bin.metalink-part").string()));
}

TEST_F(MetalinkHandlerTest, BadHashAllMirrorsFail) {
    // 唯一镜像内容与哈希不符 → 全灭 FAILED,part 不残留,成品不出现
    server().set_response("/liar.bin",
                          FakeResponse{200, "OK", {}, kMirror2Body, true});
    const std::string doc = local_meta4(
        "<file name=\"out.bin\">"
        "<hash type=\"sha-256\">" + std::string(kMirror1Sha256) + "</hash>"
        "<url>" + server().url("/liar.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(4, doc, options);
    task->set_status(TaskStatus::Downloading);

    EXPECT_THROW(handler_->download(task, nullptr), std::exception);
    EXPECT_EQ(task->status(), TaskStatus::Downloading); // worker 负责置 Failed
    EXPECT_FALSE(fs::exists((dir_.path() / "out.bin").string()));
    EXPECT_FALSE(fs::exists((dir_.path() / "out.bin.metalink-part").string()));
}

TEST_F(MetalinkHandlerTest, AllMirrorsUnreachableFailsWithDetail) {
    const std::string doc = local_meta4(
        "<file name=\"out.bin\">"
        "<url priority=\"1\">http://down1.invalid/f.bin</url>"
        "<url priority=\"2\">http://down2.invalid/f.bin</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(5, doc, options);
    task->set_status(TaskStatus::Downloading);

    try {
        handler_->download(task, nullptr);
        FAIL() << "expected exception";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("所有镜像均失败"),
                  std::string::npos);
    }
    EXPECT_FALSE(fs::exists((dir_.path() / "out.bin").string()));
}

TEST_F(MetalinkHandlerTest, RemoteMeta4FullChain) {
    // meta4 文档本身从服务器抓取 → 解析 → 委托镜像 → 校验 → 完成
    server().set_response("/m1.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    const std::string xml =
        "<?xml version=\"1.0\"?><metalink>"
        "<file name=\"remote.bin\">"
        "<size>22</size>"
        "<hash type=\"md5\">" + std::string(kMirror1Md5) + "</hash>"
        "<url>" + server().url("/m1.bin") + "</url>"
        "</file></metalink>";
    server().set_response("/doc.meta4", FakeResponse{200, "OK", {}, xml});

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(6, server().url("/doc.meta4"), options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, &listener);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "remote.bin").string()), kMirror1Body);
    // 远程抓取阶段至少一个 HEAD 探测 + GET
    EXPECT_GE(server().count_requests("/doc.meta4", "GET"), 1u);
}

TEST_F(MetalinkHandlerTest, PauseDuringSlowMirror) {
    // 慢发镜像给出暂停窗口;pause 后 download 正常返回,状态保持
    // Paused,part 清理,最终名不出现
    server().set_response("/slow.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    server().set_slow_body("/slow.bin", 200000, 4);
    const std::string doc = local_meta4(
        "<file name=\"out.bin\">"
        "<url>" + server().url("/slow.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(7, doc, options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread worker([&] { handler_->download(task, &listener); });

    // 等影子进度透传到 parent listener(防火墙生效的证据),再暂停
    ASSERT_TRUE(listener.wait_progress(1));

    handler_->pause(task); // 对齐 TaskManager:handler 负责置 Paused
    worker.join();

    EXPECT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(fs::exists((dir_.path() / "out.bin.metalink-part").string()));
    EXPECT_FALSE(fs::exists((dir_.path() / "out.bin").string()));
}

TEST_F(MetalinkHandlerTest, GetFileInfoFromLocalDoc) {
    const std::string doc = local_meta4(
        "<file name=\"info.bin\">"
        "<size>123456</size>"
        "<url>http://a/info.bin</url>"
        "</file>");

    const auto info = handler_->get_file_info("file://" + doc, {});
    EXPECT_EQ(info.filename, "info.bin");
    EXPECT_EQ(info.total_size, 123456u);
    EXPECT_TRUE(info.supports_resume);
}
