// FTP Handler Unit Tests
//
// 用 mock FTP 服务器（同目录 mock_ftp_server.hpp，文本控制协议 +
// EPSV 数据通道）对 FtpHandler 做真实回环测试：get_file_info（SIZE
// 探测）、download 全流程（RETR 落盘 + rename 发布）、断点续传
// （REST 偏移）、重试语义（一次性/永久命令失败）、暂停中止
// （progress_callback 返回 1 → CURLE_ABORTED_BY_CALLBACK）与
// curl 选项传播（代理指向无人监听端口即连接失败，证明设置生效）。
//
// 本文件旧版为 55 个自说自话的占位测试（断言字符串字面量，不触达
// 产品代码），覆盖率批次 F 全部删除重写。

#include "plugins/ftp/ftp_handler.hpp"
#include <falcon/protocol_registry.hpp>
#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include "mock_ftp_server.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace falcon;
using namespace falcon::protocols;

namespace fs = std::filesystem;

int uniqueSuffix() {
    static int counter = 0;
    return static_cast<int>(
        std::chrono::steady_clock::now().time_since_epoch().count() % 1000000) +
        (counter++);
}

/// 每用例独立临时目录（RAII 清理）
class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("falcon_ftp_test_" + std::to_string(uniqueSuffix()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }
    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

private:
    fs::path path_;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

DownloadTask::Ptr makeTask(int id, const std::string& url,
                           const std::string& output_path,
                           const falcon::DownloadOptions& options = {}) {
    auto task = std::make_shared<falcon::DownloadTask>(id, url, options);
    task->set_output_path(output_path);
    return task;
}

std::string ftpUrl(int port, const std::string& path) {
    return "ftp://127.0.0.1:" + std::to_string(port) + path;
}

class FtpHandlerTest : public ::testing::Test {
protected:
    void SetUp() override { handler_ = std::make_unique<falcon::protocols::FtpHandler>(); }

    falcon::protocols::FtpHandler* handler() { return handler_.get(); }

private:
    std::unique_ptr<falcon::protocols::FtpHandler> handler_;
};

//==============================================================================
// 协议识别与元数据
//==============================================================================

TEST_F(FtpHandlerTest, ProtocolNameAndSchemes) {
    EXPECT_EQ(handler()->protocol_name(), "ftp");
    const auto schemes = handler()->supported_schemes();
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "ftp") != schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "ftps") != schemes.end());
    EXPECT_EQ(handler()->priority(), 50);
    EXPECT_TRUE(handler()->supports_resume());
}

TEST_F(FtpHandlerTest, CanHandleRecognizesFtpAndFtpsOnly) {
    EXPECT_TRUE(handler()->can_handle("ftp://example.com/file.txt"));
    EXPECT_TRUE(handler()->can_handle("ftps://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("http://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("https://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle("ftpx://example.com/file.txt"));
    EXPECT_FALSE(handler()->can_handle(""));
}

TEST_F(FtpHandlerTest, ProtocolRegistryLoadsFtpHandler) {
    // 引用 describe_builtin_protocols 把 builtin_protocol_handlers.cpp.o
    // 强符号拉进链接（否则 core 的 weak 空 stub 生效，registry 为空——
    // 本测试旧版因此长期 GTEST_SKIP）。
    const auto described = falcon::describe_builtin_protocols();
    EXPECT_FALSE(described.empty());

    falcon::ProtocolRegistry registry;
    registry.load_builtin_handlers();
    const auto schemes = registry.supported_schemes();
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "ftp") != schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "ftps") != schemes.end());
}

//==============================================================================
// get_file_info（SIZE 探测路径）
//==============================================================================

TEST_F(FtpHandlerTest, GetFileInfoReadsSizeFromServer) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_size("/file.bin", 12345);

    auto info = handler()->get_file_info(ftpUrl(server.port(), "/file.bin"), {});
    EXPECT_EQ(info.url, ftpUrl(server.port(), "/file.bin"));
    EXPECT_EQ(info.total_size, 12345u);
    EXPECT_TRUE(info.supports_resume);
    EXPECT_TRUE(server.any_command("SIZE"));
    server.stop();
}

TEST_F(FtpHandlerTest, GetFileInfoUnknownFileThrows) {
    falcon::test::MockFtpServer server;
    server.start();
    // 未 set_file_size：SIZE 应答 550 → curl CURLE_REMOTE_FILE_NOT_FOUND
    // → get_file_info 抛出（download 循环里此路径被 catch(...) 吞掉，
    // 进度仍可在未知总长下工作——与实现注释一致）

    EXPECT_THROW(handler()->get_file_info(ftpUrl(server.port(), "/missing.bin"), {}),
                 falcon::NetworkException);
    server.stop();
}

TEST_F(FtpHandlerTest, GetFileInfoConnectionRefusedThrows) {
    falcon::test::MockFtpServer server;
    server.start();
    const int dead_port = server.port();
    server.stop(); // 端口已释放，connect 立即被拒

    EXPECT_THROW(handler()->get_file_info(ftpUrl(dead_port, "/file.bin"), {}),
                 falcon::NetworkException);
}

//==============================================================================
// download 全流程
//==============================================================================

TEST_F(FtpHandlerTest, DownloadEndToEnd) {
    falcon::test::MockFtpServer server;
    server.start();
    const std::string content = "FTP payload: hello falcon\n";
    server.set_file_content("/dir/file.bin", content);

    TempDir dir;
    const std::string out = dir.file("file.bin");
    auto task = makeTask(101, ftpUrl(server.port(), "/dir/file.bin"), out);

    handler()->download(task, nullptr);

    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
    EXPECT_EQ(readFile(out), content);
    // 临时文件已 rename 为成品
    EXPECT_FALSE(fs::exists(out + ".falcon.tmp"));
    EXPECT_TRUE(server.any_command("RETR"));
    server.stop();
}

TEST_F(FtpHandlerTest, DownloadAppliesCurlOptionsAndStillSucceeds) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_content("/file.bin", "opts");

    falcon::DownloadOptions options;
    options.verify_ssl = false;
    options.speed_limit = 0; // 明确无限制
    options.timeout_seconds = 15;

    TempDir dir;
    const std::string out = dir.file("file.bin");
    auto task = makeTask(102, ftpUrl(server.port(), "/file.bin"), out, options);

    handler()->download(task, nullptr);
    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
    EXPECT_EQ(readFile(out), "opts");
    server.stop();
}

TEST_F(FtpHandlerTest, ProxyOptionTakesEffectOnUnreachableProxy) {
    falcon::test::MockFtpServer proxy_gone;
    proxy_gone.start();
    const int dead_port = proxy_gone.port();
    proxy_gone.stop(); // 端口已释放，作为必败代理

    falcon::test::MockFtpServer server; // 文件服务器保持在线：直连本会成功
    server.start();
    server.set_file_content("/file.bin", "direct-hit");

    falcon::DownloadOptions options;
    options.proxy = "http://127.0.0.1:" + std::to_string(dead_port);
    options.proxy_username = "u";
    options.proxy_password = "p"; // 代理凭据分支一并生效（PROXYUSERPWD）
    options.max_retries = 0;
    options.retry_delay_seconds = 0;

    TempDir dir;
    auto task = makeTask(103, ftpUrl(server.port(), "/file.bin"), dir.file("file.bin"),
                         options);
    // 若代理设置未生效，curl 会直连成功使本用例变红
    EXPECT_THROW(handler()->download(task, nullptr), falcon::NetworkException);
    EXPECT_EQ(server.count_command("RETR"), 0u); // 流量从未直连到达
    server.stop();
}

TEST_F(FtpHandlerTest, DownloadReportsProgressDuringSlowTransfer) {
    falcon::test::MockFtpServer server;
    server.start();
    // 2KB 分块慢发 ≈ 512ms：越过 progress_callback 的 200ms 节流窗，
    // update_progress 至少记账一次（downloaded/total/speed 更新）
    server.set_file_content("/file.bin", std::string(2048, 'y'));
    server.set_chunk_delay_us(8'000, 32);

    falcon::DownloadOptions options;
    options.speed_limit = 1024 * 1024; // 限速设置生效性（黑盒：不影响小文件）
    options.max_retries = 0;

    TempDir dir;
    const std::string out = dir.file("file.bin");
    auto task = makeTask(112, ftpUrl(server.port(), "/file.bin"), out, options);

    handler()->download(task, nullptr);

    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
    EXPECT_EQ(readFile(out), std::string(2048, 'y'));
    EXPECT_GT(task->downloaded_bytes(), 0u); // 进度记账真实发生
    EXPECT_EQ(task->total_bytes(), 2048u);
    server.stop();
}

TEST_F(FtpHandlerTest, DownloadThrowsWhenRenameFails) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_content("/file.bin", "data");

    TempDir dir;
    // 成品路径预置为**目录**：temp 文件可写，但下载完成后 rename
    // (temp → 目录) 必败 → FileIOException，绝不假报完成
    const std::string out = dir.file("occupied");
    fs::create_directories(out);
    auto task = makeTask(113, ftpUrl(server.port(), "/file.bin"), out);

    EXPECT_THROW(handler()->download(task, nullptr), falcon::FileIOException);
    // download() 不设状态（状态归 TaskManager）：异常收口后仍是初始 Pending，
    // 绝不变 Completed
    EXPECT_EQ(task->status(), falcon::TaskStatus::Pending);
    EXPECT_TRUE(fs::exists(out + ".falcon.tmp")); // temp 保留（成品未发布）
    server.stop();
}

//==============================================================================
// 断点续传（REST）
//==============================================================================

TEST_F(FtpHandlerTest, DownloadResumesFromExistingTempFile) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_content("/file.bin", "AABBCCDD");

    TempDir dir;
    const std::string out = dir.file("file.bin");
    writeFile(out + ".falcon.tmp", "AA"); // 已落盘 2 字节

    falcon::DownloadOptions options;
    options.resume_enabled = true;
    auto task = makeTask(104, ftpUrl(server.port(), "/file.bin"), out, options);

    handler()->download(task, nullptr);

    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
    // 服务器侧收到 REST 2，续传尾部数据以 app 模式拼接
    EXPECT_TRUE(server.any_command("REST 2"));
    EXPECT_EQ(readFile(out), "AABBCCDD");
    EXPECT_FALSE(fs::exists(out + ".falcon.tmp"));
    server.stop();
}

//==============================================================================
// 重试语义
//==============================================================================

TEST_F(FtpHandlerTest, DownloadRetriesAfterTransientRetrFailure) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_content("/file.bin", "retry-ok");
    server.fail_command_once("RETR"); // 第一次 RETR 550，第二次放行

    falcon::DownloadOptions options;
    options.max_retries = 1;
    options.retry_delay_seconds = 1; // 指数退避真实睡眠：2^0 × 1s

    TempDir dir;
    const std::string out = dir.file("file.bin");
    auto task = makeTask(105, ftpUrl(server.port(), "/file.bin"), out, options);

    const auto started = std::chrono::steady_clock::now();
    handler()->download(task, nullptr);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
    EXPECT_EQ(readFile(out), "retry-ok");
    EXPECT_EQ(server.count_command("RETR"), 2u);
    EXPECT_GE(elapsed_ms, 900); // 重试间隔真实等待过
    server.stop();
}

TEST_F(FtpHandlerTest, DownloadThrowsAfterRetriesExhausted) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_content("/file.bin", "data"); // SIZE 探测通过，RETR 阶段恒败
    server.fail_command("RETR");

    falcon::DownloadOptions options;
    options.max_retries = 1;
    options.retry_delay_seconds = 0;

    TempDir dir;
    auto task = makeTask(106, ftpUrl(server.port(), "/file.bin"), dir.file("file.bin"),
                         options);

    EXPECT_THROW(handler()->download(task, nullptr), falcon::NetworkException);
    // 首连 + 1 次重试 = 恰好 2 次 RETR
    EXPECT_EQ(server.count_command("RETR"), 2u);
    server.stop();
}

TEST_F(FtpHandlerTest, DownloadFailsFastWhenSizeProbeFails) {
    falcon::test::MockFtpServer server;
    server.start();
    // 未注册文件：SIZE 恒 550。curl 在 RETR 之前先做 SIZE 探测，
    // 550 即判 "Remote file not found"——RETR 从未发出，下载循环
    // 两轮（get_file_info 抛出被吞 + 传输失败）后按重试耗尽抛出

    falcon::DownloadOptions options;
    options.max_retries = 1;
    options.retry_delay_seconds = 0;

    TempDir dir;
    auto task = makeTask(111, ftpUrl(server.port(), "/file.bin"), dir.file("file.bin"),
                         options);

    EXPECT_THROW(handler()->download(task, nullptr), falcon::NetworkException);
    EXPECT_EQ(server.count_command("RETR"), 0u);
    EXPECT_GE(server.count_command("SIZE"), 2u); // 每轮重试各探测一次
    EXPECT_FALSE(fs::exists(dir.file("file.bin")));
    server.stop();
}

//==============================================================================
// 错误与中止路径
//==============================================================================

TEST_F(FtpHandlerTest, DownloadFailsFastWhenOutputFileCannotOpen) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_content("/file.bin", "data");

    TempDir dir;
    // 输出路径落在不存在的目录里：ofstream 打开失败 → FileIOException，
    // 不产生任何 RETR（先于网络阶段失败）
    const std::string out = (dir.path() / "missing_sub" / "file.bin").string();
    auto task = makeTask(107, ftpUrl(server.port(), "/file.bin"), out);

    EXPECT_THROW(handler()->download(task, nullptr), falcon::FileIOException);
    EXPECT_EQ(server.count_command("RETR"), 0u);
    server.stop();
}

TEST_F(FtpHandlerTest, CancelledTaskSkipsDownload) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_content("/file.bin", "never");

    TempDir dir;
    auto task = makeTask(108, ftpUrl(server.port(), "/file.bin"), dir.file("file.bin"));
    handler()->cancel(task);
    EXPECT_EQ(task->status(), falcon::TaskStatus::Cancelled);

    handler()->download(task, nullptr); // 循环顶检查 Cancelled → 立即返回
    EXPECT_EQ(task->status(), falcon::TaskStatus::Cancelled);
    EXPECT_EQ(server.count_command("RETR"), 0u);
    EXPECT_FALSE(fs::exists(dir.file("file.bin")));
    server.stop();
}

TEST_F(FtpHandlerTest, PauseAbortsTransferMidFlight) {
    falcon::test::MockFtpServer server;
    server.start();
    // 8KB 分块慢发（32B/10ms ≈ 2.6s 窗口），给暂停留出时间
    server.set_file_content("/file.bin", std::string(8192, 'x'));
    server.set_chunk_delay_us(10'000, 32);

    TempDir dir;
    const std::string out = dir.file("file.bin");
    auto task = makeTask(109, ftpUrl(server.port(), "/file.bin"), out);

    std::thread pauser([&task] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        task->set_status(falcon::TaskStatus::Paused);
    });
    handler()->download(task, nullptr);
    pauser.join();

    // CURLE_ABORTED_BY_CALLBACK → download 静默返回：不 rename、不抛出、
    // 状态保持 Paused，临时文件保留为断点
    EXPECT_EQ(task->status(), falcon::TaskStatus::Paused);
    EXPECT_FALSE(fs::exists(out));
    EXPECT_TRUE(fs::exists(out + ".falcon.tmp"));
    server.stop();
}

TEST_F(FtpHandlerTest, ResumeRestartsPausedDownload) {
    falcon::test::MockFtpServer server;
    server.start();
    server.set_file_content("/file.bin", "resumable");

    TempDir dir;
    const std::string out = dir.file("file.bin");
    auto task = makeTask(110, ftpUrl(server.port(), "/file.bin"), out);
    handler()->pause(task);
    EXPECT_EQ(task->status(), falcon::TaskStatus::Paused);

    handler()->resume(task, nullptr); // set Downloading + download()
    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
    EXPECT_EQ(readFile(out), "resumable");
    server.stop();
}

TEST_F(FtpHandlerTest, PauseAndCancelRejectNullTask) {
    handler()->pause(nullptr);
    handler()->resume(nullptr, nullptr);
    handler()->cancel(nullptr);
    SUCCEED();
}

} // namespace
