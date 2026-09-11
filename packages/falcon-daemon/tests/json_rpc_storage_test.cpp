// JSON-RPC × TaskStorage 集成测试。
//
// 覆盖持久化层与 RPC 查询/删除的联动：
// - 重启后只存在于数据库中的终态历史对 tellStatus/tellStopped/getGlobalStat 可见
// - 引擎内存态优先，storage 仅作回落
// - removeDownloadResult / purgeDownloadResult 同时清理引擎与数据库
// - pauseAll 的暂停状态同步落库
// - forceShutdown 触发停机回调

#include "rpc/json_rpc_server.hpp"
#include "storage/task_storage.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <falcon/download_engine.hpp>
#include <falcon/download_task.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

using json = nlohmann::json;

#ifdef _WIN32
using recv_send_size_t = int;
static int socket_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
static void ensure_winsock_started() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
using recv_send_size_t = ssize_t;
static int socket_close(int fd) { return ::close(fd); }
static void ensure_winsock_started() {}
#endif

struct ScopedFd {
    int fd = -1;
    ~ScopedFd() {
        if (fd >= 0) socket_close(fd);
    }
    ScopedFd() = default;
    explicit ScopedFd(int f) : fd(f) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

static std::optional<std::string> exchange_loopback(uint16_t port, const std::string& payload) {
    ensure_winsock_started();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    ScopedFd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd.fd < 0) return std::nullopt;

#ifdef _WIN32
    DWORD timeout_ms = 5000;
    ::setsockopt(fd.fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(fd.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    for (int attempt = 0; attempt < 100; ++attempt) {
        if (::connect(fd.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;
        if (attempt == 99) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::size_t sent = 0;
    while (sent < payload.size()) {
        const auto chunk_len = static_cast<
#ifdef _WIN32
            int
#else
            std::size_t
#endif
            >(payload.size() - sent);
        const recv_send_size_t n = ::send(fd.fd, payload.data() + sent, chunk_len, 0);
        if (n <= 0) return std::nullopt;
        sent += static_cast<std::size_t>(n);
    }

    std::string buf;
    char tmp[4096];
    while (true) {
        const recv_send_size_t n = ::recv(fd.fd, tmp, sizeof(tmp), 0);
        if (n == 0) break;
        if (n < 0) return std::nullopt;
        buf.append(tmp, tmp + n);
        if (buf.size() > 4 * 1024 * 1024) return std::nullopt;
    }
    return buf;
}

static std::string make_http_post(const std::string& body) {
    std::string http;
    http += "POST /jsonrpc HTTP/1.1\r\n";
    http += "Host: 127.0.0.1\r\n";
    http += "Content-Type: application/json\r\n";
    http += "Connection: close\r\n";
    http += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    http += body;
    return http;
}

static std::string gid_of(falcon::TaskId id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    return std::string(buf);
}

// download() 会阻塞到 release/取消或任务进入 Paused/终态，让测试可以
// 确定性地构造"正在下载"的任务；pause/resume/cancel 同步改状态。
class BlockingHandler final : public falcon::IProtocolHandler {
public:
    std::string protocol_name() const override { return "test"; }

    std::vector<std::string> supported_schemes() const override { return {"test"}; }

    bool can_handle(const std::string& url) const override {
        return url.rfind("test://", 0) == 0;
    }

    falcon::FileInfo get_file_info(const std::string& url,
                                   const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "blocking.bin";
        info.total_size = 1000;
        info.supports_resume = true;
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        running_.fetch_add(1);
        while (!task->is_finished() && task->status() != falcon::TaskStatus::Paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        running_.fetch_sub(1);
    }

    void pause(falcon::DownloadTask::Ptr task) override {
        task->set_status(falcon::TaskStatus::Paused);
    }

    void resume(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->set_status(falcon::TaskStatus::Downloading);
    }

    void cancel(falcon::DownloadTask::Ptr task) override {
        task->set_status(falcon::TaskStatus::Cancelled);
    }

    bool supports_resume() const override { return true; }

    void wait_until_idle() {
        while (running_.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

private:
    std::atomic<int> running_{0};
};

class JsonRpcStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_.register_handler(std::make_unique<BlockingHandler>());

        falcon::daemon::TaskStorageConfig storage_cfg;  // :memory:
        storage_ = std::make_unique<falcon::daemon::TaskStorage>(storage_cfg);
        ASSERT_TRUE(storage_->initialize());

        cfg_.listen_port = 0;
        cfg_.secret.clear();
        cfg_.allow_origin_all = false;
        cfg_.bind_address = "127.0.0.1";

        server_ = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(
            &engine_, cfg_, storage_.get());
        ASSERT_TRUE(server_->start());
        ASSERT_NE(server_->port(), 0);
    }

    void TearDown() override {
        // 解除所有阻塞中的 download()（cancel 同步置终态），再收掉服务器
        ReleaseBlockingTasks();
        if (server_) server_->stop();
    }

    void ReleaseBlockingTasks() {
        for (const auto& task : engine_.get_all_tasks()) {
            if (task && !task->is_finished()) task->cancel();
        }
        for (int i = 0; i < 2500; ++i) {
            bool all_done = true;
            for (const auto& task : engine_.get_all_tasks()) {
                if (task && !task->is_finished()) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    json call(const std::string& method, json params) {
        json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", std::move(params)}};
        auto full = exchange_loopback(server_->port(), make_http_post(req.dump()));
        EXPECT_TRUE(full.has_value());
        if (!full.has_value()) return json();
        const std::string sep = "\r\n\r\n";
        auto pos = full->find(sep);
        EXPECT_NE(pos, std::string::npos);
        if (pos == std::string::npos) return json();
        return json::parse(full->substr(pos + sep.size()));
    }

    // 等待任务进入活动态（避免 addUri 返回后任务尚未被工作线程启动的竞态）
    void wait_until_active(falcon::TaskId id) {
        auto task = engine_.get_task(id);
        ASSERT_TRUE(task);
        for (int i = 0; i < 2500 && !task->is_active(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_TRUE(task->is_active());
    }

    // 向 storage 直接插入一条记录（绕过 RPC，模拟重启前遗留的数据）
    void seed_record(falcon::TaskId id, falcon::TaskStatus status,
                     const std::string& url, falcon::Bytes downloaded) {
        falcon::daemon::TaskRecord record;
        record.id = id;
        record.url = url;
        record.output_path = "/downloads/seed_" + std::to_string(static_cast<long long>(id)) + ".bin";
        record.status = status;
        record.progress = 1.0;
        record.total_bytes = 1000;
        record.downloaded_bytes = downloaded;
        record.created_at = std::chrono::system_clock::now();
        record.updated_at = std::chrono::system_clock::now();
        ASSERT_EQ(storage_->create_task(record), id);
    }

    falcon::DownloadEngine engine_;
    std::unique_ptr<falcon::daemon::TaskStorage> storage_;
    falcon::daemon::rpc::JsonRpcServerConfig cfg_;
    std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> server_;
};

// ---------------------------------------------------------------------------
// 查询回落：重启后仅存于数据库的历史任务
// ---------------------------------------------------------------------------

TEST_F(JsonRpcStorageTest, TellStatusFallsBackToStorageRecord) {
    seed_record(42, falcon::TaskStatus::Completed, "test://seed-42", 1000);

    json resp = call("aria2.tellStatus", json::array({gid_of(42)}));
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"]["status"], "complete");
    EXPECT_EQ(resp["result"]["completedLength"], "1000");
    EXPECT_EQ(resp["result"]["files"][0]["path"], "/downloads/seed_42.bin");
}

TEST_F(JsonRpcStorageTest, TellStatusPrefersEngineState) {
    // 引擎任务与历史记录互不干扰：查询引擎任务返回引擎侧数据
    seed_record(42, falcon::TaskStatus::Completed, "test://seed-42", 1000);

    json add = call("aria2.addUri",
                    json::array({json::array({"test://live"}),
                                 json{{"dir", "/tmp"}, {"out", "engine.bin"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();

    json resp = call("aria2.tellStatus", json::array({add["result"].get<std::string>()}));
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_NE(resp["result"]["files"][0]["path"].get<std::string>().find("engine.bin"),
              std::string::npos);
}

TEST_F(JsonRpcStorageTest, TellStatusUnknownGidIsError) {
    json resp = call("aria2.tellStatus", json::array({"00000000000fffffff"}));
    ASSERT_TRUE(resp.contains("error")) << resp.dump();
    EXPECT_EQ(resp["error"]["code"], 2);
}

TEST_F(JsonRpcStorageTest, TellStoppedIncludesStorageHistory) {
    seed_record(42, falcon::TaskStatus::Completed, "test://seed-42", 1000);
    seed_record(43, falcon::TaskStatus::Failed, "test://seed-43", 400);

    json resp = call("aria2.tellStopped", json::array({0, 10}));
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    ASSERT_EQ(resp["result"].size(), 2u);

    bool saw_complete = false, saw_error = false;
    for (const auto& entry : resp["result"]) {
        if (entry["status"] == "complete") saw_complete = true;
        if (entry["status"] == "error") saw_error = true;
    }
    EXPECT_TRUE(saw_complete);
    EXPECT_TRUE(saw_error);
}

TEST_F(JsonRpcStorageTest, GetGlobalStatCountsStorageHistory) {
    seed_record(42, falcon::TaskStatus::Completed, "test://seed-42", 1000);

    json resp = call("aria2.getGlobalStat", json::array());
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"]["numStopped"].get<std::string>(), "1");
    EXPECT_EQ(resp["result"]["numActive"].get<std::string>(), "0");
}

TEST_F(JsonRpcStorageTest, TellWaitingDeduplicatesEngineAndStorage) {
    // 引擎中的暂停任务与 storage 中同 id 的记录只应出现一次
    json add = call("aria2.addUri",
                    json::array({json::array({"test://wait-me"}),
                                 json{{"dir", "/tmp"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();

    json pause = call("aria2.pause", json::array({add["result"].get<std::string>()}));
    ASSERT_TRUE(pause.contains("result")) << pause.dump();

    json resp = call("aria2.tellWaiting", json::array({0, 10}));
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].size(), 1u);
    EXPECT_EQ(resp["result"][0]["status"], "paused");
}

// ---------------------------------------------------------------------------
// 元数据查询：引擎任务与 storage 记录两条路径
// ---------------------------------------------------------------------------

TEST_F(JsonRpcStorageTest, GetOptionEchoesTaskOptions) {
    // addUri 阶段会真实创建输出目录，用平台临时目录保证各环境可写
    const std::string dir =
        (std::filesystem::temp_directory_path() / "falcon_rpc_getopt").string();
    json add = call("aria2.addUri",
                    json::array({json::array({"test://opts"}),
                                 json{{"dir", dir}, {"out", "named.bin"},
                                      {"max-download-limit", "4096"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();

    json resp = call("aria2.getOption", json::array({add["result"].get<std::string>()}));
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"]["dir"], dir);
    EXPECT_EQ(resp["result"]["out"], "named.bin");
    EXPECT_EQ(resp["result"]["max-download-limit"], "4096");
}

TEST_F(JsonRpcStorageTest, GetFilesFromStorageRecord) {
    seed_record(42, falcon::TaskStatus::Completed, "test://seed-42", 1000);

    json resp = call("aria2.getFiles", json::array({gid_of(42)}));
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    ASSERT_EQ(resp["result"].size(), 1u);
    EXPECT_EQ(resp["result"][0]["path"], "/downloads/seed_42.bin");
    EXPECT_EQ(resp["result"][0]["selected"], "true");
    EXPECT_EQ(resp["result"][0]["uris"][0]["uri"], "test://seed-42");
}

TEST_F(JsonRpcStorageTest, GetUrisReturnsTaskUrl) {
    seed_record(42, falcon::TaskStatus::Completed, "test://seed-42", 1000);

    json resp = call("aria2.getUris", json::array({gid_of(42)}));
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    ASSERT_EQ(resp["result"].size(), 1u);
    EXPECT_EQ(resp["result"][0]["uri"], "test://seed-42");
}

// ---------------------------------------------------------------------------
// 删除联动：引擎与数据库一起清理
// ---------------------------------------------------------------------------

TEST_F(JsonRpcStorageTest, RemoveDownloadResultDeletesRecord) {
    seed_record(42, falcon::TaskStatus::Completed, "test://seed-42", 1000);

    json resp = call("aria2.removeDownloadResult", json::array({gid_of(42)}));
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"], "OK");
    EXPECT_FALSE(storage_->task_exists(42));

    json gone = call("aria2.tellStatus", json::array({gid_of(42)}));
    EXPECT_TRUE(gone.contains("error"));
}

TEST_F(JsonRpcStorageTest, RemoveDownloadResultUnknownGidIsError) {
    json resp = call("aria2.removeDownloadResult", json::array({"00000000000ffffffe"}));
    ASSERT_TRUE(resp.contains("error")) << resp.dump();
    EXPECT_EQ(resp["error"]["code"], 2);
}

TEST_F(JsonRpcStorageTest, PurgeDownloadResultClearsEngineAndStorage) {
    seed_record(42, falcon::TaskStatus::Completed, "test://seed-42", 1000);

    // 制造一个引擎侧任务（阻塞中的非终态任务不会被 purge 波及）
    json add = call("aria2.addUri",
                    json::array({json::array({"test://keep-active"}),
                                 json{{"dir", "/tmp"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();

    json resp = call("aria2.purgeDownloadResult", json::array());
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"], "OK");

    // 终态历史全部清除
    json stopped = call("aria2.tellStopped", json::array({0, 10}));
    ASSERT_TRUE(stopped.contains("result")) << stopped.dump();
    EXPECT_EQ(stopped["result"].size(), 0u);
    EXPECT_EQ(storage_->count_tasks_by_status(falcon::TaskStatus::Completed) +
                  storage_->count_tasks_by_status(falcon::TaskStatus::Failed) +
                  storage_->count_tasks_by_status(falcon::TaskStatus::Cancelled), 0);
}

// ---------------------------------------------------------------------------
// 批量控制与会话
// ---------------------------------------------------------------------------

TEST_F(JsonRpcStorageTest, PauseAllPersistsPausedStatus) {
    json add = call("aria2.addUri",
                    json::array({json::array({"test://pause-me"}),
                                 json{{"dir", "/tmp"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();
    const auto tid = static_cast<falcon::TaskId>(
        std::stoull(add["result"].get<std::string>(), nullptr, 16));

    // 等 addUri 的任务真正进入活动态，保证 pauseAll 的"受影响任务"收集到它
    wait_until_active(tid);

    json resp = call("aria2.pauseAll", json::array());
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"], "OK");

    auto record = storage_->get_task(tid);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, falcon::TaskStatus::Paused);
}

TEST_F(JsonRpcStorageTest, ForcePauseAndForceRemoveAlias) {
    json add = call("aria2.addUri",
                    json::array({json::array({"test://force"}),
                                 json{{"dir", "/tmp"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();
    const std::string gid = add["result"].get<std::string>();

    json fp = call("aria2.forcePause", json::array({gid}));
    ASSERT_TRUE(fp.contains("result")) << fp.dump();
    EXPECT_EQ(fp["result"], gid);

    json fr = call("aria2.forceRemove", json::array({gid}));
    ASSERT_TRUE(fr.contains("result")) << fr.dump();
    EXPECT_EQ(fr["result"], gid);
}

TEST_F(JsonRpcStorageTest, ForceShutdownTriggersHandler) {
    std::atomic<bool> shutdown_requested{false};
    server_->set_shutdown_handler([&shutdown_requested] { shutdown_requested.store(true); });

    json resp = call("aria2.forceShutdown", json::array());
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"], "OK");
    EXPECT_TRUE(shutdown_requested.load());
}

TEST_F(JsonRpcStorageTest, SessionInfoAndSaveSession) {
    json info = call("aria2.getSessionInfo", json::array());
    ASSERT_TRUE(info.contains("result")) << info.dump();
    EXPECT_FALSE(info["result"]["sessionId"].get<std::string>().empty());

    json save = call("aria2.saveSession", json::array());
    ASSERT_TRUE(save.contains("result")) << save.dump();
    EXPECT_EQ(save["result"], "OK");
}

// ---------------------------------------------------------------------------
// 全局选项
// ---------------------------------------------------------------------------

TEST_F(JsonRpcStorageTest, GlobalOptionRoundTrip) {
    json change = call("aria2.changeGlobalOption",
                       json::array({json{{"max-overall-download-limit", "12345"},
                                          {"max-concurrent-downloads", "3"}}}));
    ASSERT_TRUE(change.contains("result")) << change.dump();
    EXPECT_EQ(change["result"], "OK");

    json after = call("aria2.getGlobalOption", json::array());
    ASSERT_TRUE(after.contains("result")) << after.dump();
    EXPECT_EQ(after["result"]["max-overall-download-limit"], "12345");
    EXPECT_EQ(after["result"]["max-concurrent-downloads"], "3");

    EXPECT_EQ(engine_.get_global_speed_limit(), falcon::BytesPerSecond{12345});
    EXPECT_EQ(engine_.get_max_concurrent_tasks(), std::size_t{3});

    // "none" 表示取消限制
    json none = call("aria2.changeGlobalOption",
                     json::array({json{{"max-overall-download-limit", "none"}}}));
    ASSERT_TRUE(none.contains("result")) << none.dump();
    EXPECT_EQ(engine_.get_global_speed_limit(), falcon::BytesPerSecond{0});

    // 未知键报错
    json unknown = call("aria2.changeGlobalOption",
                        json::array({json{{"unsupported-key", "1"}}}));
    EXPECT_TRUE(unknown.contains("error")) << unknown.dump();
}

} // namespace
