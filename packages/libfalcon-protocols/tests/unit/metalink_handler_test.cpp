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

    // 等影子进度透传到 parent listener(防火墙生效的证据),再暂停。
    // 等待结果先记下不提前退出:pause+join 必须先收 worker,断言后置
    const bool progressed = listener.wait_progress(1);

    handler_->pause(task); // 对齐 TaskManager:handler 负责置 Paused
    worker.join();

    ASSERT_TRUE(progressed);

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

/// file 本体不可读(路径指向目录):读取阶段失败,异常携带具体原因
TEST_F(MetalinkHandlerTest, LocalDocDirectoryReadFailsWithDetail) {
    const std::string dir_path = (dir_.path() / "not-a-doc").string();
    fs::create_directories(dir_path);

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(31, dir_path, options);
    task->set_status(TaskStatus::Downloading);

    try {
        handler_->download(task, nullptr);
        FAIL() << "expected exception";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("读取 metalink 文件失败"),
                  std::string::npos);
    }
}

/// 非 http(s)/file 的获取方式:can_handle 只看后缀,download 在获取
/// 阶段按不支持协议干净拒绝(绝不回落普通 http 处理器)
TEST_F(MetalinkHandlerTest, UnsupportedFetchSchemeRejected) {
    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(32, "ftp://192.0.2.10/doc.meta4", options);
    task->set_status(TaskStatus::Downloading);

    try {
        handler_->download(task, nullptr);
        FAIL() << "expected exception";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("不支持的 metalink 获取方式"),
                  std::string::npos);
    }
}

/// 远程文档但注册表无 http handler:抓取前置检查拒绝
TEST_F(MetalinkHandlerTest, RemoteFetchWithoutHttpHandler) {
    auto empty_registry = std::make_unique<ProtocolRegistry>();
    auto lone = std::make_unique<MetalinkHandler>();
    lone->set_protocol_registry(empty_registry.get());

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(33, "http://192.0.2.10/doc.meta4", options);
    task->set_status(TaskStatus::Downloading);

    try {
        lone->download(task, nullptr);
        FAIL() << "expected exception";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("HTTP handler 未注册"),
                  std::string::npos);
    }
}

/// 抓取远程文档途中暂停:pause 转发抓取连接中止数据流,fetch 以非
/// Completed 收口 → 按暂停语义收口,parent 停在 Paused
TEST_F(MetalinkHandlerTest, PauseDuringRemoteFetchStaysPaused) {
    const std::string xml =
        "<?xml version=\"1.0\"?><metalink><file name=\"x.bin\">"
        "<url>http://192.0.2.10/x.bin</url></file></metalink>";
    server().set_response("/remote.meta4",
                          FakeResponse{200, "OK", {}, xml, false});
    server().set_slow_body("/remote.meta4", 100000, 2);  // 2B/100ms 慢发

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(34, server().url("/remote.meta4"), options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread worker([&] { handler_->download(task, &listener); });
    // 等待结果先记下不提前退出:pause+join 必须先收 worker
    const bool progressed = listener.wait_progress(1);
    handler_->pause(task);  // 对齐 TaskManager:handler 负责置 Paused
    worker.join();

    ASSERT_TRUE(progressed);
    ASSERT_EQ(task->status(), TaskStatus::Paused);
    EXPECT_FALSE(fs::exists((dir_.path() / "x.bin").string()));
}

/// 文档含多个 file:告警并只取第一个,其余条目不产生任何下载
TEST_F(MetalinkHandlerTest, MultiFileDocTakesFirstOnly) {
    server().set_response("/first.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    const std::string doc = local_meta4(
        "<file name=\"first.bin\">"
        "<url>" + server().url("/first.bin") + "</url>"
        "</file>"
        "<file name=\"second.bin\">"
        "<url>" + server().url("/second.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(35, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "first.bin").string()), kMirror1Body);
    EXPECT_FALSE(fs::exists((dir_.path() / "second.bin").string()));
    EXPECT_EQ(server().count_requests("/second.bin"), 0u);
}

/// file 条目零镜像:解析层即拒绝(不进镜像循环,零网络请求)
TEST_F(MetalinkHandlerTest, FileWithoutUrlsRejectedAtParse) {
    const std::string doc = local_meta4(
        "<file name=\"out.bin\">"
        "<hash type=\"sha-256\">" + std::string(64, 'a') + "</hash>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(36, doc, options);
    task->set_status(TaskStatus::Downloading);

    try {
        handler_->download(task, nullptr);
        FAIL() << "expected exception";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("无可用 HTTP/FTP 镜像"),
                  std::string::npos);
    }
    EXPECT_FALSE(fs::exists((dir_.path() / "out.bin").string()));
}

/// 镜像 URL 带 .meta4 后缀:路由特判把镜像指回 metalink 自身 →
/// 视为无可用协议处理器,该镜像不计入可委托列表
TEST_F(MetalinkHandlerTest, MirrorRoutedToSelfTreatedUnavailable) {
    server().set_response("/loop.meta4",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    // 自注册:metalink 必须在册,.meta4 后缀镜像才被路由特判截获
    auto own_registry = std::make_unique<ProtocolRegistry>();
    own_registry->register_handler(std::make_unique<HttpHandler>());
    auto ml = std::make_unique<MetalinkHandler>();
    MetalinkHandler* ml_raw = ml.get();
    own_registry->register_handler(std::move(ml));
    ml_raw->set_protocol_registry(own_registry.get());

    const std::string doc = local_meta4(
        "<file name=\"out.bin\">"
        "<url>" + server().url("/loop.meta4") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(37, doc, options);
    task->set_status(TaskStatus::Downloading);

    try {
        ml_raw->download(task, nullptr);
        FAIL() << "expected exception";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("无可用协议处理器"),
                  std::string::npos);
    }
    // 自指镜像从未被当作普通 http 镜像委托(零请求)
    EXPECT_EQ(server().count_requests("/loop.meta4"), 0u);
}

/// 无 registry 的 handler:镜像循环拿到 null target → "无可用协议
/// 处理器"(513 的 else 分支),异常消息携带逐镜像原因
TEST_F(MetalinkHandlerTest, NoRegistryTreatedUnavailable) {
    server().set_response("/nr.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    const std::string doc = local_meta4(
        "<file name=\"nrout.bin\">"
        "<url>" + server().url("/nr.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    MetalinkHandler bare;  // 不 set_protocol_registry
    auto task = make_task(41, doc, options);
    task->set_status(TaskStatus::Downloading);

    try {
        bare.download(task, nullptr);
        FAIL() << "expected exception";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("无可用协议处理器"),
                  std::string::npos);
    }
    // 镜像从未被委托(零请求),最终名不出现
    EXPECT_EQ(server().count_requests("/nr.bin"), 0u);
    EXPECT_FALSE(fs::exists((dir_.path() / "nrout.bin").string()));
}

/// 串行委托阶段取消:cancel 先转发 V2 引擎(无桥接组,无操作)再经
/// ActiveContext 转发影子任务的目标 handler(832),parent 置 Cancelled
TEST_F(MetalinkHandlerTest, CancelDuringSlowMirror) {
    server().set_response("/cslow.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    server().set_slow_body("/cslow.bin", 200000, 4);
    const std::string doc = local_meta4(
        "<file name=\"cout.bin\">"
        "<url>" + server().url("/cslow.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(42, doc, options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread worker([&] { handler_->download(task, &listener); });
    // 等待结果先记下不提前退出:cancel+join 必须先收 worker
    const bool progressed = listener.wait_progress(1);

    handler_->cancel(task);  // 对齐 TaskManager:handler 负责置 Cancelled
    worker.join();

    ASSERT_TRUE(progressed);

    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
    EXPECT_FALSE(
        fs::exists((dir_.path() / "cout.bin.metalink-part").string()));
    EXPECT_FALSE(fs::exists((dir_.path() / "cout.bin").string()));
}


//==============================================================================
// Metalink V2 多源桥接(阶段2:V2EngineHost 开启)
//
// 门禁/桥接/回落全链。V2 行为断言需哈希能力(门禁⑤),非 OpenSSL
// 构建以 FALCON_TEST_METALINK_V2_HASH 排除;门禁拒绝类用例(单镜
// 像/无哈希)不依赖哈希能力,恒编译。
//==============================================================================

#include <falcon/protocols/file_hash.hpp>
#include <falcon/protocols/v2_engine_host.hpp>

#if defined(FALCON_USE_OPENSSL) || defined(FALCON_HAS_OPENSSL)
#define FALCON_TEST_METALINK_V2_HASH 1
#endif

namespace {

using falcon::FileHasher;
using falcon::HashAlgorithm;
using falcon::V2EngineHost;
using falcon::testscripts::RecordedRequest;

/// 32 字节确定内容(4 段 × 8B:min_segment_size=8 + 4 连接)
std::string v2_test_body() {
    std::string body;
    body.reserve(32);
    for (std::size_t i = 0; i < 32; ++i) {
        body.push_back(static_cast<char>('A' + (i % 26)));
    }
    return body;
}

std::string sha256_hex(const std::string& data) {
    return FileHasher::calculate(data.data(), data.size(),
                                 HashAlgorithm::SHA256);
}

/// V2 由 GET 响应头判定分段能力:显式 Accept-Ranges 才会分段
FakeResponse v2_range_response(const std::string& body) {
    FakeResponse resp;
    resp.status = 200;
    resp.body = body;
    resp.support_range = true;
    resp.headers = {{"Accept-Ranges", "bytes"}};
    return resp;
}

/// path 上是否收到过带 Range 的 GET
bool path_received_range(const std::vector<RecordedRequest>& requests,
                         const std::string& path) {
    for (const auto& r : requests) {
        if (r.path == path && r.range.rfind("bytes=", 0) == 0) return true;
    }
    return false;
}

/// path 上以 bytes=<start>- 开头的 Range 请求次数
std::size_t count_range_with_start(const std::vector<RecordedRequest>& requests,
                                   const std::string& path,
                                   std::size_t start) {
    const std::string prefix = "bytes=" + std::to_string(start) + "-";
    std::size_t n = 0;
    for (const auto& r : requests) {
        if (r.path == path && r.range.rfind(prefix, 0) == 0) ++n;
    }
    return n;
}

/// V2 桥接夹具:MetalinkHandlerTest 基建 + V2EngineHost 开关生命周期
class MetalinkV2BridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& host = V2EngineHost::instance();
        host.shutdown_and_join();  // 干净起点(其他用例可能已启动引擎)
        host.set_v2_http_enabled(true);
        falcon::EngineConfigV2 config;
        config.command_wait_timeout_seconds = 10;  // 兜底防挂死拖 120s
        host.configure(config);

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
        auto& host = V2EngineHost::instance();
        host.shutdown_and_join();
        host.set_v2_http_enabled(false);
    }

    ScriptedHttpServer& server() { return *server_; }

    std::string local_meta4(const std::string& inner) {
        const std::string path =
            (dir_.path() /
             ("v2doc" + std::to_string(doc_count_++) + ".meta4"))
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

#ifdef FALCON_TEST_METALINK_V2_HASH

/// 双镜像多源分段:V2 完成 → 校验通过 → 发布;事件序列无影子
TEST_F(MetalinkV2BridgeTest, V2OnTwoMirrorsDistributesSegments) {
    const std::string body = v2_test_body();
    server().set_response("/va.bin",
                          v2_range_response(body));
    server().set_response("/vb.bin",
                          v2_range_response(body));
    const std::string doc = local_meta4(
        "<file name=\"out.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/va.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/vb.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(101, doc, options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, &listener);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    const std::string final_path = (dir_.path() / "out.bin").string();
    EXPECT_EQ(read_file(final_path), body);
    // 无影子:parent 事件序列仍是恰两次状态变化(终态由 handler 驱动)
    {
        std::lock_guard<std::mutex> lock(listener.mutex_);
        ASSERT_EQ(listener.statuses.size(), 2u);
        EXPECT_EQ(listener.statuses[1].second, TaskStatus::Completed);
    }
    // 两镜像各收到 Range GET(混源分段生效,而非串行单镜像)
    const auto reqs = server().requests();
    EXPECT_TRUE(path_received_range(reqs, "/va.bin"));
    EXPECT_TRUE(path_received_range(reqs, "/vb.bin"));
    // part 与 V2 残留全部消失(组完成时引擎已改名发布 + 删控制文件)
    EXPECT_FALSE(fs::exists(final_path + ".metalink-part"));
    EXPECT_FALSE(fs::exists(final_path + ".metalink-part.falcon.tmp"));
    EXPECT_FALSE(fs::exists(final_path + ".metalink-part.falcon.ctrl"));
}

/// 段级换源:uris[1] 上的段 1(起点 8)传 4B 后断连 → 换到 uris[0]
/// 从断点(12)续传,组不连坐失败
TEST_F(MetalinkV2BridgeTest, V2OnSegmentFailureSwapsMirror) {
    const std::string body = v2_test_body();
    server().set_response("/vok.bin",
                          v2_range_response(body));
    server().set_response("/vfail.bin",
                          v2_range_response(body));
    server().set_abort_after("/vfail.bin", 4, 8);  // 一次性:仅段 1 的连接
    const std::string doc = local_meta4(
        "<file name=\"out2.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/vok.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/vfail.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(102, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "out2.bin").string()), body);
    const auto reqs = server().requests();
    // /vfail.bin 对起点 8 恰 1 次(断连后换源,绝不同镜像重试)
    EXPECT_EQ(count_range_with_start(reqs, "/vfail.bin", 8), 1u);
    // /vok.bin 收到断点续传 Range(起点 12 = 8 + 4)
    EXPECT_GE(count_range_with_start(reqs, "/vok.bin", 12), 1u);
}

/// 整文件哈希不符回落:A、B(V2 混源)内容错误 → 校验失败清残留 →
/// 阶段1 串行重下(坏镜像逐个失败)→ C 提供正确内容发布
TEST_F(MetalinkV2BridgeTest, V2OnHashMismatchFallsBackToSerial) {
    const std::string body_good = v2_test_body();
    std::string body_bad;
    for (std::size_t i = 0; i < 32; ++i) {
        body_bad.push_back(static_cast<char>('a' + (i % 26)));
    }
    server().set_response("/vbad1.bin",
                          v2_range_response(body_bad));
    server().set_response("/vbad2.bin",
                          v2_range_response(body_bad));
    server().set_response("/vgood.bin",
                          v2_range_response(body_good));
    const std::string doc = local_meta4(
        "<file name=\"out3.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body_good) + "</hash>"
        "<url priority=\"1\">" + server().url("/vbad1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/vbad2.bin") + "</url>"
        "<url priority=\"3\">" + server().url("/vgood.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(103, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "out3.bin").string()), body_good);
    // 回落清理彻底:混源临时文件与断点不残留(否则阶段1 会按污染
    // 数据"续传"),part 经阶段1 成功后 rename 发布,亦消失
    EXPECT_FALSE(
        fs::exists((dir_.path() / "out3.bin.metalink-part").string()));
    EXPECT_FALSE(fs::exists(
        (dir_.path() / "out3.bin.metalink-part.falcon.tmp").string()));
    EXPECT_FALSE(fs::exists(
        (dir_.path() / "out3.bin.metalink-part.falcon.ctrl").string()));
}

/// 1 ftp + 2 http:桥接只取 http 镜像(ftp handler 未注册也不出错)
TEST_F(MetalinkV2BridgeTest, V2OnFtpMirrorIgnoredByBridge) {
    const std::string body = v2_test_body();
    server().set_response("/fh1.bin",
                          v2_range_response(body));
    server().set_response("/fh2.bin",
                          v2_range_response(body));
    const std::string doc = local_meta4(
        "<file name=\"out6.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">ftp://192.0.2.55/f.bin</url>"
        "<url priority=\"2\">" + server().url("/fh1.bin") + "</url>"
        "<url priority=\"3\">" + server().url("/fh2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(106, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "out6.bin").string()), body);
    const auto reqs = server().requests();
    EXPECT_TRUE(path_received_range(reqs, "/fh1.bin") ||
                path_received_range(reqs, "/fh2.bin"));
}

/// 暂停/恢复:桥接期间 pause → 引擎组暂停(临时文件是断点挂点,
/// part 本体不存在)→ resume 重入 download() 组对齐续跑至完成
TEST_F(MetalinkV2BridgeTest, V2PauseThenResume) {
    const std::string body = v2_test_body();
    server().set_response("/vp1.bin",
                          v2_range_response(body));
    server().set_response("/vp2.bin",
                          v2_range_response(body));
    server().set_slow_body("/vp1.bin", 100000, 2);  // 每 2B 停 100ms
    server().set_slow_body("/vp2.bin", 100000, 2);
    const std::string doc = local_meta4(
        "<file name=\"out7.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/vp1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/vp2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(107, doc, options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread worker([&] { handler_->download(task, &listener); });
    // 等真实字节落账(桥接上报进度),避免暂停落在组尚未开拉的窗口
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (task->downloaded_bytes() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 等待超时也不提前退出:pause+join 必须先收 worker,进度断言后置
    handler_->pause(task);  // 对齐 TaskManager:handler 负责置 Paused
    worker.join();

    ASSERT_GT(task->downloaded_bytes(), 0u);
    ASSERT_EQ(task->status(), TaskStatus::Paused);
    const std::string part =
        (dir_.path() / "out7.bin.metalink-part").string();
    // 引擎只写临时名:part 本体不存在,断点挂点保留供 resume 续跑
    EXPECT_FALSE(fs::exists(part));
    EXPECT_TRUE(fs::exists(part + ".falcon.tmp"));

    // resume 前置位 Downloading 是 TaskManager 职责(http handler 同约定)
    task->set_status(TaskStatus::Downloading);
    handler_->resume(task, &listener);  // download() 重入,组对齐续跑
    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "out7.bin").string()), body);
    EXPECT_FALSE(fs::exists(part));
    EXPECT_FALSE(fs::exists(part + ".falcon.tmp"));
    EXPECT_FALSE(fs::exists(part + ".falcon.ctrl"));
}

/// V2 组失败回落:两镜像 GET 对 start>0 的 Range 一律 500 且重试
/// 预算 0 → 组快速 FAILED → 清混源残留 → 阶段1 回落。镜像 1 的
/// HEAD 探测切换 GET 剧本(无 Range 平凡响应),回落串行单连接
/// 整文件下载成功发布(进程开关开启时阶段1 经 V2 适配器,同样以
/// HEAD 为下载入口,分段判定随新 GET 剧本关闭)
TEST_F(MetalinkV2BridgeTest, V2GroupFailedFallsBackToSerial) {
    const std::string body = v2_test_body();
    FakeResponse liar = v2_range_response(body);
    liar.fail_nonzero_range = true;
    server().set_response("/gf1.bin", liar);
    server().set_response("/gf2.bin", liar);
    // 串行阶段剧本:HEAD 即切换(镜像 1 首个 HEAD 后 GET 变平凡响应)
    FakeResponse plain;
    plain.status = 200;
    plain.body = body;
    server().set_head_response("/gf1.bin", plain, /*flip_get=*/true);
    const std::string doc = local_meta4(
        "<file name=\"gfout.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/gf1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/gf2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    options.max_retries = 0;  // 段失败无重试预算,组快速 FAILED
    auto task = make_task(230, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "gfout.bin").string()), body);
    // 回落前残留已清:混源临时/控制文件不残留,串行按干净 part 重下
    const std::string part = (dir_.path() / "gfout.bin.metalink-part").string();
    EXPECT_FALSE(fs::exists(part + ".falcon.tmp"));
    EXPECT_FALSE(fs::exists(part + ".falcon.ctrl"));
}

/// 同 id 终态组重注入:首次 V2 完成后同任务再次 download → 终态组
/// 被提前回收(不等 10s purge 周期)→ 重注入后再完成
TEST_F(MetalinkV2BridgeTest, V2SameIdReinjectAfterTerminal) {
    const std::string body = v2_test_body();
    server().set_response("/ri1.bin", v2_range_response(body));
    server().set_response("/ri2.bin", v2_range_response(body));
    const std::string doc = local_meta4(
        "<file name=\"riout.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/ri1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/ri2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(231, doc, options);
    task->set_status(TaskStatus::Downloading);
    handler_->download(task, nullptr);
    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "riout.bin").string()), body);

    task->set_status(TaskStatus::Downloading);  // 模拟重新入队
    handler_->download(task, nullptr);
    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "riout.bin").string()), body);
}

/// 暂停后文档镜像变更:resume 重入时 PAUSED 组的 uris 与新文档不符
/// → 旧组断点作废重建,按新镜像列表下载至完成
TEST_F(MetalinkV2BridgeTest, V2ResumeWithChangedMirrors) {
    const std::string body = v2_test_body();
    server().set_response("/cm1.bin", v2_range_response(body));
    server().set_response("/cm2.bin", v2_range_response(body));
    server().set_slow_body("/cm1.bin", 100000, 2);
    server().set_slow_body("/cm2.bin", 100000, 2);
    const std::string doc_path = local_meta4(
        "<file name=\"cmout.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/cm1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/cm2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(232, doc_path, options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread worker([&] { handler_->download(task, &listener); });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (task->downloaded_bytes() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 等待超时也不提前退出:pause+join 必须先收 worker,进度断言后置
    handler_->pause(task);
    worker.join();
    ASSERT_GT(task->downloaded_bytes(), 0u);
    ASSERT_EQ(task->status(), TaskStatus::Paused);

    // 重写文档:镜像列表换成全新路径,uris 对比必不相等
    server().set_response("/cm3.bin", v2_range_response(body));
    server().set_response("/cm4.bin", v2_range_response(body));
    {
        std::ofstream out(doc_path, std::ios::binary | std::ios::trunc);
        out << "<?xml version=\"1.0\"?><metalink>"
            << "<file name=\"cmout.bin\">"
            << "<size>32</size>"
            << "<hash type=\"sha-256\">" << sha256_hex(body) << "</hash>"
            << "<url priority=\"1\">" << server().url("/cm3.bin") << "</url>"
            << "<url priority=\"2\">" << server().url("/cm4.bin") << "</url>"
            << "</file></metalink>";
    }

    task->set_status(TaskStatus::Downloading);
    handler_->resume(task, &listener);  // download() 重入:旧组作废重建
    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "cmout.bin").string()), body);
    const auto reqs = server().requests();
    EXPECT_TRUE(path_received_range(reqs, "/cm3.bin"));
    EXPECT_TRUE(path_received_range(reqs, "/cm4.bin"));
}

/// V2 侧组被外部暂停(绕过 handler 直接引擎 pause_task):桥接发现
/// 组 PAUSED → parent 对齐 Paused 挂起;resume 后组对齐续跑至完成
TEST_F(MetalinkV2BridgeTest, V2BridgeGroupPausedExternally) {
    const std::string body = v2_test_body();
    server().set_response("/pe1.bin", v2_range_response(body));
    server().set_response("/pe2.bin", v2_range_response(body));
    server().set_slow_body("/pe1.bin", 100000, 2);
    server().set_slow_body("/pe2.bin", 100000, 2);
    const std::string doc = local_meta4(
        "<file name=\"peout.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/pe1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/pe2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(233, doc, options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread worker([&] { handler_->download(task, &listener); });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (task->downloaded_bytes() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_GT(task->downloaded_bytes(), 0u);

    // 不经 metalink pause():组 PAUSED 而 parent 仍是 Downloading
    V2EngineHost::instance().engine()->pause_task(task->id());
    worker.join();

    ASSERT_EQ(task->status(), TaskStatus::Paused);
    const std::string part = (dir_.path() / "peout.bin.metalink-part").string();
    EXPECT_FALSE(fs::exists(part));
    EXPECT_TRUE(fs::exists(part + ".falcon.tmp"));

    server().clear_slow_body("/pe1.bin");
    server().clear_slow_body("/pe2.bin");
    task->set_status(TaskStatus::Downloading);
    handler_->resume(task, &listener);
    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "peout.bin").string()), body);
}

/// 桥接期间取消:cancel 置 Cancelled + 转发引擎 + 清 V2 残留,桥接
/// 以 kSuspended 收口,part/成品/混源挂点全部不残留
TEST_F(MetalinkV2BridgeTest, V2CancelDuringBridge) {
    const std::string body = v2_test_body();
    server().set_response("/cc1.bin", v2_range_response(body));
    server().set_response("/cc2.bin", v2_range_response(body));
    server().set_slow_body("/cc1.bin", 100000, 2);
    server().set_slow_body("/cc2.bin", 100000, 2);
    const std::string doc = local_meta4(
        "<file name=\"ccout.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/cc1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/cc2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(234, doc, options);
    RecordingListener listener;
    task->set_listener(&listener);
    task->set_status(TaskStatus::Downloading);

    std::thread worker([&] { handler_->download(task, &listener); });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (task->downloaded_bytes() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 等待超时也不提前退出:cancel+join 必须先收 worker,进度断言后置
    handler_->cancel(task);  // 对齐 TaskManager:handler 负责置 Cancelled
    worker.join();

    ASSERT_GT(task->downloaded_bytes(), 0u);
    ASSERT_EQ(task->status(), TaskStatus::Cancelled);
    const std::string part = (dir_.path() / "ccout.bin.metalink-part").string();
    EXPECT_FALSE(fs::exists(part));
    EXPECT_FALSE(fs::exists(part + ".falcon.tmp"));
    EXPECT_FALSE(fs::exists(part + ".falcon.ctrl"));
    EXPECT_FALSE(fs::exists((dir_.path() / "ccout.bin").string()));
}

/// 同 id 已有非暂停/终态组(外部注入的并发冲突):桥接判状态异常
/// 回落阶段1,串行照常完成,不向 worker 抛
TEST_F(MetalinkV2BridgeTest, V2ConflictingActiveGroupFallsBackToSerial) {
    const std::string body = v2_test_body();
    server().set_response("/xf1.bin", v2_range_response(body));
    server().set_response("/xf2.bin", v2_range_response(body));
    // 外部组:同任务 id,挂在慢镜像上保持 ACTIVE(陈旧残留先清,防止
    // add 门禁即时 FAILED 走错分支)
    std::error_code stale_ec;
    fs::remove("/tmp/falcon-conflict.bin", stale_ec);
    fs::remove("/tmp/falcon-conflict.bin.falcon.tmp", stale_ec);
    fs::remove("/tmp/falcon-conflict.bin.falcon.ctrl", stale_ec);
    const std::string foreign_body(4096, 'z');
    server().set_response("/foreign.bin",
                          FakeResponse{200, "OK", {}, foreign_body, false});
    server().set_slow_body("/foreign.bin", 100000, 2);
    auto engine = V2EngineHost::instance().engine();  // 提前启动引擎
    DownloadTask::Ptr conflict =
        make_task(235, server().url("/foreign.bin"), DownloadOptions{});
    engine->add_download_as(conflict->id(), {server().url("/foreign.bin")},
                            DownloadOptions{}, "/tmp/falcon-conflict.bin");

    const std::string doc = local_meta4(
        "<file name=\"xfout.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/xf1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/xf2.bin") + "</url>"
        "</file>");
    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(235, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "xfout.bin").string()), body);
    engine->cancel_task(conflict->id());  // 收尾外部组(桥接已 cancel,幂等)
    std::error_code rm_ec;
    fs::remove("/tmp/falcon-conflict.bin.falcon.tmp", rm_ec);
    fs::remove("/tmp/falcon-conflict.bin.falcon.ctrl", rm_ec);
}

#endif  // FALCON_TEST_METALINK_V2_HASH

/// 单镜像(V2 开):门禁 http 镜像 ≥2 不过 → 阶段1 串行,行为不变
TEST_F(MetalinkV2BridgeTest, V2OnSingleMirrorUsesStage1) {
    server().set_response("/single.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    const std::string doc = local_meta4(
        "<file name=\"out4.bin\">"
        "<url>" + server().url("/single.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(104, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "out4.bin").string()), kMirror1Body);
    // 阶段1 单连接:全新下载不带 Range
    for (const auto& r : server().requests()) {
        if (r.path == "/single.bin") EXPECT_TRUE(r.range.empty());
    }
}

/// 无整文件哈希(V2 开):门禁拒绝无校验混源 → 阶段1 首镜像成功即止
TEST_F(MetalinkV2BridgeTest, V2OnNoHashFallsBack) {
    server().set_response("/nh1.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    server().set_response("/nh2.bin",
                          FakeResponse{200, "OK", {}, kMirror2Body, true});
    const std::string doc = local_meta4(
        "<file name=\"out5.bin\">"
        "<url priority=\"1\">" + server().url("/nh1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/nh2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    auto task = make_task(105, doc, options);
    task->set_status(TaskStatus::Downloading);

    handler_->download(task, nullptr);

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "out5.bin").string()), kMirror1Body);
    // 串行语义:首镜像成功,次镜像零请求
    EXPECT_EQ(server().count_requests("/nh2.bin"), 0u);
}

} // namespace
/// curl 专属能力门禁:cookie/HTTP 认证/Referer 任一存在 → 桥接前置
/// 回退阶段1 串行,引擎全程未被拉起(try_engine 恒空)
TEST_F(MetalinkV2BridgeTest, V2GateRejectsCurlSpecificOptions) {
    server().set_response("/gt1.bin",
                          FakeResponse{200, "OK", {}, kMirror1Body, true});
    server().set_response("/gt2.bin",
                          FakeResponse{200, "OK", {}, kMirror2Body, true});

    // 文档无需哈希:能力门禁在镜像/哈希检查之前返回
    auto make_doc = [&](const char* fname) {
        return local_meta4(
            "<file name=\"" + std::string(fname) + "\">"
            "<url priority=\"1\">" + server().url("/gt1.bin") + "</url>"
            "<url priority=\"2\">" + server().url("/gt2.bin") + "</url>"
            "</file>");
    };
    auto run_case = [&](falcon::TaskId id, void (*mutate)(DownloadOptions&),
                        const char* fname) {
        DownloadOptions options;
        options.output_directory = dir_.string();
        mutate(options);
        auto task = make_task(id, make_doc(fname), options);
        task->set_status(TaskStatus::Downloading);
        handler_->download(task, nullptr);
        EXPECT_EQ(task->status(), TaskStatus::Completed);
        EXPECT_EQ(read_file((dir_.path() / fname).string()), kMirror1Body);
    };

    run_case(240, [](DownloadOptions& o) { o.cookie_file = "cookies.txt"; },
             "gt-cookie.bin");
    run_case(241,
             [](DownloadOptions& o) {
                 o.http_username = "user";
                 o.http_password = "pass";
             },
             "gt-auth.bin");
    run_case(242, [](DownloadOptions& o) { o.referer = "http://ref.example/"; },
             "gt-referer.bin");

    // socks 代理:门禁判 Unsupported 回退阶段1;V1 curl 走死代理 →
    // 两镜像皆不可达,干净失败(门禁行本身已被评估并命中)
    {
        DownloadOptions options;
        options.output_directory = dir_.string();
        options.proxy = "socks5://127.0.0.1:1080";  // 无 socks 服务
        options.max_retries = 0;  // 连接拒绝无退避,快速失败
        auto task = make_task(243, make_doc("gt-socks.bin"), options);
        task->set_status(TaskStatus::Downloading);
        EXPECT_THROW(handler_->download(task, nullptr), std::exception);
    }

    // 四条门禁路径全部前置回退:宿主从未启动引擎
    EXPECT_EQ(V2EngineHost::instance().try_engine(), nullptr);
}

/// 桥接轮询中宿主停机:try_engine 返回 null → kFailed → 清残留回落
/// 阶段1 串行循环 → Completed
TEST_F(MetalinkV2BridgeTest, V2EngineShutdownDuringBridgeFallsBackToSerial) {
    const std::string body = v2_test_body();
    server().set_response("/sd1.bin", v2_range_response(body));
    server().set_response("/sd2.bin", v2_range_response(body));
    server().set_slow_body("/sd1.bin", 150000, 4);  // 撑开桥接轮询窗口
    const std::string doc = local_meta4(
        "<file name=\"sdout.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/sd1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/sd2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(251, doc, options);
    task->set_status(TaskStatus::Downloading);

    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        V2EngineHost::instance().shutdown_and_join();
    });
    handler_->download(task, nullptr);
    killer.join();

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "sdout.bin").string()), body);
    // 回落串行清掉了 V2 残留
    const std::string final_path = (dir_.path() / "sdout.bin").string();
    EXPECT_FALSE(fs::exists(final_path + ".metalink-part.falcon.tmp"));
    EXPECT_FALSE(fs::exists(final_path + ".metalink-part.falcon.ctrl"));
}

/// 桥接轮询中组被移除(外部 cancel_task):find_group null → kFailed
/// → 回落阶段1 串行循环 → Completed
TEST_F(MetalinkV2BridgeTest, V2GroupRemovedDuringBridgeFallsBackToSerial) {
    const std::string body = v2_test_body();
    server().set_response("/rm1.bin", v2_range_response(body));
    server().set_response("/rm2.bin", v2_range_response(body));
    server().set_slow_body("/rm1.bin", 150000, 4);
    const std::string doc = local_meta4(
        "<file name=\"rmout.bin\">"
        "<size>32</size>"
        "<hash type=\"sha-256\">" + sha256_hex(body) + "</hash>"
        "<url priority=\"1\">" + server().url("/rm1.bin") + "</url>"
        "<url priority=\"2\">" + server().url("/rm2.bin") + "</url>"
        "</file>");

    DownloadOptions options;
    options.output_directory = dir_.string();
    options.max_connections = 4;
    options.min_segment_size = 8;
    auto task = make_task(252, doc, options);
    task->set_status(TaskStatus::Downloading);

    std::thread remover([&] {
        // 等桥接把组注入引擎（避免 cancel 早于注入的时序竞争），
        // 5s 上限防桥接异常时用例悬挂
        auto& host = V2EngineHost::instance();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            auto engine = host.try_engine();
            if (engine && engine->request_group_man() &&
                engine->request_group_man()->find_group(task->id())) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (auto engine = host.try_engine()) {
            engine->cancel_task(task->id());  // remove_group → 组消失
        }
    });
    handler_->download(task, nullptr);
    remover.join();

    ASSERT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_EQ(read_file((dir_.path() / "rmout.bin").string()), body);
}
