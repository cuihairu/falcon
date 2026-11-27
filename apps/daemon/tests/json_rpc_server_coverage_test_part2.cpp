// JSON-RPC server coverage tests.
//
// Complements json_rpc_server_test.cpp with exhaustive coverage of the
// HTTP layer (methods / paths / CORS / fragmented requests) and the
// JSON-RPC dispatch layer (every aria2.* method, parameter validation,
// error paths, notifications and system.multicall).

#include "rpc/json_rpc_server.hpp"

#include <csignal>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <falcon/detail/injection.hpp>
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
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {
/// 本文件回环基建含「写已断连 socket」剧本（客户端失败收口主动 abort
/// 连接、服务器按剧本 drop 短 body/超限请求、超时清理关 fd 后仍有在途
/// 写）——POSIX 默认对 socket 写的 SIGPIPE 做进程级终止，任一写命中
/// RST 即杀死整个测试进程（RangeLiarServer / TlsTestServer /
/// ProxyTestServer 同族红面先例）。进程级忽略让写失败以 EPIPE 返回
/// 值出现（各 send_all 的 n<=0 分支既有吞错路径正常收口）；Windows 无
/// SIGPIPE 语义无需处理
struct SigpipeImmunity {
    SigpipeImmunity() {
#if !defined(_WIN32)
        ::signal(SIGPIPE, SIG_IGN);
#endif
    }
};

const SigpipeImmunity falcon_test_sigpipe_immunity;

using json = nlohmann::json;

#ifdef _WIN32
using recv_send_size_t = int;

TEST_F(JsonRpcCoverageTest, AddUriParsesAllTypedOptions) {
    start_server();

    const std::string dir = ::testing::TempDir();
    json opts = {
        {"dir", dir},
        {"out", "falcon-cov-a.bin"},
        {"user-agent", "FalconCov/1.0"},
        {"referer", "http://referer.example/"},
        {"load-cookies", dir + "cov-cookies.txt"},
        {"save-cookies", dir + "cov-jar.txt"},
        {"http-user", "covuser"},
        {"http-passwd", "covpass"},
        {"all-proxy", "http://127.0.0.1:9"},
        {"all-proxy-user", "puser"},
        {"all-proxy-passwd", "ppass"},
        {"check-certificate", false},
        {"max-tries", "7"},
        {"retry-wait", 3},
        {"max-connection-per-server", "5"},
        {"max-download-limit", 2048},
        {"p2sp-share", "true"},
        {"header", json::array({"X-Custom-A: 1", "X-Custom-B:2", "no-colon-here", 42})},
    };
    auto parsed = call("aria2.addUri",
                       json::array({json::array({"test://local/a.bin"}), opts}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();

    json opts2 = {
        {"dir", dir},
        {"out", "falcon-cov-b.bin"},
        {"check-certificate", "false"},
        {"max-tries", 9},
        {"retry-wait", "2"},
        {"max-connection-per-server", 6},
        {"max-download-limit", "4096"},
        {"p2sp-share", "false"},
        {"header", "X-Custom-C: 3"},
    };
    parsed = call("aria2.addUri",
                  json::array({json::array({"test://local/b.bin"}), opts2}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, AddUriP2spShareStrictValues) {
    start_server();

    const std::string dir = ::testing::TempDir();
    // 合法三态只允许 "true"/"false" 字符串，且经 getOption 原样回显
    for (const char* const val : {"true", "false"}) {
        json opts = {{"dir", dir},
                     {"out", std::string("falcon-p2sp-") + val + ".bin"},
                     {"p2sp-share", std::string(val)}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/p2sp.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
        const std::string gid = parsed["result"].get<std::string>();
        auto got = call("aria2.getOption", json::array({gid}));
        ASSERT_TRUE(got.contains("result")) << got.dump();
        EXPECT_EQ(got["result"]["p2sp-share"], std::string(val));
    }

    // 非字符串 / 非法字面量 / 缺省以外的形态 => -32602
    for (const json& bad : {json(true), json("yes"), json("TRUE"), json(1)}) {
        json opts = {{"dir", dir}, {"p2sp-share", bad}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/p2sp-bad.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
        EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    }
}

TEST_F(JsonRpcCoverageTest, AddUriSplitContinueOverwriteRenamingOptions) {
    start_server();

    const std::string dir = ::testing::TempDir();
    // split/continue/allow-overwrite/auto-file-renaming：addUri 从"白名单
    // 广告但零映射（静默丢弃）"收口为真实映射。数值键接受字符串/整数双
    // 形态，布尔键接受 "true"/"false" 字符串/JSON 布尔双形态；getOption
    // 以 aria2 字符串值形态回显
    struct OptionCase {
        const char* key;
        json value;
        const char* echo;
    };
    int n = 0;
    for (const auto& c : {OptionCase{"split", json("3"), "3"},
                          OptionCase{"split", json(4), "4"},
                          OptionCase{"continue", json("false"), "false"},
                          OptionCase{"continue", json(true), "true"},
                          OptionCase{"allow-overwrite", json("true"), "true"},
                          OptionCase{"allow-overwrite", json(false), "false"},
                          OptionCase{"auto-file-renaming", json(true), "true"},
                          OptionCase{"auto-file-renaming", json("false"), "false"}}) {
        json opts = {{"dir", dir},
                     {"out", "falcon-cov-opt-" + std::to_string(n++) + ".bin"},
                     {c.key, c.value}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/opt.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
        const std::string gid = parsed["result"].get<std::string>();
        auto got = call("aria2.getOption", json::array({gid}));
        ASSERT_TRUE(got.contains("result")) << got.dump();
        EXPECT_EQ(got["result"][c.key], std::string(c.echo)) << c.key;
    }

    // 非法形态 => -32602：布尔键拒绝数字与其余字面量，数值键拒绝负数/
    // 布尔/垃圾文本
    for (const auto& [key, bad] :
         {std::pair<const char*, json>{"split", json("abc")},
          {"split", json(true)},
          {"split", json(-2)},
          {"continue", json("yes")},
          {"continue", json(1)},
          {"allow-overwrite", json(0)},
          {"auto-file-renaming", json(1)}}) {
        json opts = {{"dir", dir}, {"out", "falcon-cov-opt-bad.bin"}, {key, bad}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/opt-bad.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("error")) << key << ": " << parsed.dump();
        EXPECT_EQ(parsed["error"]["code"], -32602) << key << ": " << parsed.dump();
    }

    // 回归钉子：既有数值键的垃圾值此前走裸 std::stoull 抛
    // invalid_argument，被 dispatch 兜底成 -32603 Internal error——错误码
    // 与"参数值非法"的语义不符，钉死为 -32602
    for (const char* const key : {"max-tries", "retry-wait", "timeout",
                                  "max-connection-per-server",
                                  "max-download-limit"}) {
        json opts = {{"dir", dir}, {key, "abc"}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/opt-garbage.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("error")) << key << ": " << parsed.dump();
        EXPECT_EQ(parsed["error"]["code"], -32602) << key << ": " << parsed.dump();
    }
}

TEST_F(JsonRpcCoverageTest, AddUriFileAllocationConditionalGetOptions) {
    start_server();

    const std::string dir = ::testing::TempDir();
    // file-allocation / conditional-get：与 split/continue 同族的"aria2
    // 客户端长期发送、Falcon 静默忽略"收口。file-allocation 与 CLI 同
    // stance 裸字符串透传（非法值由引擎侧 parse_file_allocation 按 none
    // 处理，此处只拒绝非字符串形状）；conditional-get 走布尔双形态解析
    struct OptionCase {
        const char* key;
        json value;
        const char* echo;
    };
    int n = 0;
    for (const auto& c : {OptionCase{"file-allocation", json("prealloc"), "prealloc"},
                          OptionCase{"file-allocation", json("trunc"), "trunc"},
                          OptionCase{"conditional-get", json("true"), "true"},
                          OptionCase{"conditional-get", json(false), "false"}}) {
        json opts = {{"dir", dir},
                     {"out", "falcon-cov-alloc-" + std::to_string(n++) + ".bin"},
                     {c.key, c.value}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/alloc.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("result")) << c.key << ": " << parsed.dump();
        const std::string gid = parsed["result"].get<std::string>();
        auto got = call("aria2.getOption", json::array({gid}));
        ASSERT_TRUE(got.contains("result")) << got.dump();
        EXPECT_EQ(got["result"][c.key], std::string(c.echo)) << c.key;
    }

    // 非法形态 => -32602：布尔键拒绝数字与其余字面量，file-allocation
    // 拒绝非字符串形状
    for (const auto& [key, bad] :
         {std::pair<const char*, json>{"conditional-get", json("yes")},
          {"conditional-get", json(1)},
          {"file-allocation", json(5)}}) {
        json opts = {{"dir", dir}, {"out", "falcon-cov-alloc-bad.bin"}, {key, bad}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/alloc-bad.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("error")) << key << ": " << parsed.dump();
        EXPECT_EQ(parsed["error"]["code"], -32602) << key << ": " << parsed.dump();
    }
}

TEST_F(JsonRpcCoverageTest, AddUriMinSplitSizeAndCheckCertificateOptions) {
    start_server();

    const std::string dir = ::testing::TempDir();
    // min-split-size：aria2 同名键，之前 addUri 零映射（aria2 客户端
    // 长期发送即被静默丢弃）。接受裸字节数与 K/M/G/T 后缀，严格全串
    // 数字形态（无前缀解析、无溢出放行）；check-certificate 收紧为
    // 布尔双形态解析——旧实现把任何非 "false" 的字符串当 true，垃圾
    // 值静默放行
    struct OptionCase {
        const char* key;
        json value;
        const char* echo;
    };
    int n = 0;
    for (const auto& c : {OptionCase{"min-split-size", json("1M"), "1048576"},
                          OptionCase{"min-split-size", json("524288"), "524288"},
                          OptionCase{"min-split-size", json("2g"), "2147483648"},
                          OptionCase{"check-certificate", json(true), "true"},
                          OptionCase{"check-certificate", json("false"), "false"}}) {
        json opts = {{"dir", dir},
                     {"out", "falcon-cov-split-" + std::to_string(n++) + ".bin"},
                     {c.key, c.value}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/split.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("result")) << c.key << ": " << c.value.dump() << " => "
                                               << parsed.dump();
        const std::string gid = parsed["result"].get<std::string>();
        auto got = call("aria2.getOption", json::array({gid}));
        ASSERT_TRUE(got.contains("result")) << got.dump();
        EXPECT_EQ(got["result"][c.key], std::string(c.echo)) << c.key;
    }

    // 非法形态 => -32602：min-split-size 拒绝非字符串形状、非数字、
    // 裸后缀与溢出；check-certificate 拒绝其余字面量与数字
    for (const auto& [key, bad] :
         {std::pair<const char*, json>{"min-split-size", json(5)},
          {"min-split-size", json("abc")},
          {"min-split-size", json("12abc")},
          {"min-split-size", json("M")},
          {"min-split-size", json("1.5M")},
          {"min-split-size", json("999999999999999999999G")},
          {"check-certificate", json("yes")},
          {"check-certificate", json(1)}}) {
        json opts = {{"dir", dir}, {"out", "falcon-cov-split-bad.bin"}, {key, bad}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/split-bad.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("error")) << key << ": " << bad.dump() << " => "
                                              << parsed.dump();
        EXPECT_EQ(parsed["error"]["code"], -32602) << key << ": " << parsed.dump();
    }
}

// seed-ratio / seed-time 双形态解析：合法字符串与数字都接受，非法字符串
// 经 stod 抛异常 → 钳 0（不 500）。数值键直接取 double，负值钳 0。
TEST_F(JsonRpcCoverageTest, AddUriSeedRatioAndSeedTimeParseErrorsClampToZero) {
    start_server();

    const std::string dir = ::testing::TempDir();
    // 非法字符串：std::stod 抛异常 → catch 置 -1.0 → max(0,-1)=0
    for (const char* const bad : {"not-a-number", "12abc", "abc12"}) {
        json opts = {{"dir", dir},
                     {"out", "falcon-seed-bad.bin"},
                     {"seed-ratio", std::string(bad)},
                     {"seed-time", std::string(bad)}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/seedbad.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("result")) << bad << ": " << parsed.dump();
    }

    // 合法字符串与数字双形态均接受。
    for (const json& ratio : {json("1.5"), json(2.5)}) {
        json opts = {{"dir", dir},
                     {"out", "falcon-seed-ok.bin"},
                     {"seed-ratio", ratio},
                     {"seed-time", "30"}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/seedok.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("result")) << ratio.dump() << ": " << parsed.dump();
    }

    // 负值钳 0（字符串 "-2" 经 stod → -2 → max 0）。
    json opts = {{"dir", dir},
                 {"out", "falcon-seed-neg.bin"},
                 {"seed-ratio", "-2"},
                 {"seed-time", -1}};
    auto parsed = call("aria2.addUri",
                       json::array({json::array({"test://local/seedneg.bin"}), opts}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
}


// ===========================================================================
// gid validation / task control
// ===========================================================================

TEST_F(JsonRpcCoverageTest, GidValidationErrors) {
    start_server();

    for (const char* const bad : {"not-a-gid", "zzzz", "0", "0000000000000000",
                                  "12345678901234567", "ffffffffffffff", "-1"}) {
        auto parsed = call("aria2.tellStatus", json::array({std::string(bad)}));
        EXPECT_EQ(parsed["error"]["code"], 2) << "gid=" << bad;
    }

    // Valid hex but unknown task.
    auto parsed = call("aria2.tellStatus", json::array({"ffffffffffffffff"}));
    EXPECT_EQ(parsed["error"]["code"], 2);

    // Missing / wrong-typed params.
    parsed = call("aria2.pause", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.pause", json::array({123}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.unpause", json::array({json::array({"x"})}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.remove", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, GidAcceptsHexPrefixes) {
    start_server();
    auto task = engine_.add_task("test://local/prefix");
    ASSERT_TRUE(task);

    for (const std::string& gid : {gid_of(task->id()),
                                   "0x" + gid_of(task->id()),
                                   "0X" + gid_of(task->id())}) {
        auto parsed = call("aria2.tellStatus", json::array({gid}));
        EXPECT_TRUE(parsed.contains("result")) << parsed.dump() << " gid=" << gid;
    }
}

TEST_F(JsonRpcCoverageTest, PausePendingTaskSucceeds) {
    start_server();
    auto task = engine_.add_task("test://local/pauseme");
    ASSERT_TRUE(task);
    const std::string gid = gid_of(task->id());

    auto parsed = call("aria2.pause", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], gid);

    auto status = call("aria2.tellStatus", json::array({gid}));
    ASSERT_TRUE(status.contains("result"));
    EXPECT_EQ(status["result"]["status"], "paused");
}

TEST_F(JsonRpcCoverageTest, UnpauseNotPausedTaskFails) {
    start_server();
    auto task = engine_.add_task("test://local/notpaused");
    ASSERT_TRUE(task);  // stays Pending: never started, never paused

    auto parsed = call("aria2.unpause", json::array({gid_of(task->id())}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 1);
}

TEST_F(JsonRpcCoverageTest, UnpausePausedTaskSucceeds) {
    start_server();
    auto task = engine_.add_task("test://local/resumeme");
    ASSERT_TRUE(task);
    ASSERT_TRUE(engine_.pause_task(task->id()));
    const std::string gid = gid_of(task->id());

    auto parsed = call("aria2.unpause", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], gid);
}

TEST_F(JsonRpcCoverageTest, RemoveTaskSucceeds) {
    start_server();
    auto task = engine_.add_task("test://local/removeme");
    ASSERT_TRUE(task);
    const std::string gid = gid_of(task->id());

    auto parsed = call("aria2.remove", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], gid);

    auto status = call("aria2.tellStatus", json::array({gid}));
    ASSERT_TRUE(status.contains("result"));
    EXPECT_EQ(status["result"]["status"], "removed");
}

TEST_F(JsonRpcCoverageTest, StatusMappingForAllTaskStates) {
    start_server();

    struct Case {
        falcon::TaskStatus status;
        const char* expected;
    };
    std::vector<Case> cases = {
        {falcon::TaskStatus::Pending, "waiting"},
        {falcon::TaskStatus::Preparing, "active"},
        {falcon::TaskStatus::Downloading, "active"},
        {falcon::TaskStatus::Paused, "paused"},
        {falcon::TaskStatus::Completed, "complete"},
        {falcon::TaskStatus::Cancelled, "removed"},
        {falcon::TaskStatus::Failed, "error"},
    };

    for (const auto& c : cases) {
        auto task = engine_.add_task("test://local/state");
        ASSERT_TRUE(task);
        task->set_status(c.status);
        auto parsed = call("aria2.tellStatus", json::array({gid_of(task->id())}));
        ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
        EXPECT_EQ(parsed["result"]["status"], c.expected)
            << "status " << static_cast<int>(c.status);
    }
}

// ===========================================================================
// aria2.tellActive / tellWaiting / tellStopped
// ===========================================================================

TEST_F(JsonRpcCoverageTest, TellActiveEmpty) {
    start_server();
    auto parsed = call("aria2.tellActive", json::array());
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_TRUE(parsed["result"].is_array());
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});
}

TEST_F(JsonRpcCoverageTest, TellActiveReturnsActiveTasks) {
    start_server();
    auto task = engine_.add_task("test://local/active");
    ASSERT_TRUE(task);
    task->set_status(falcon::TaskStatus::Downloading);

    auto parsed = call("aria2.tellActive", json::array());
    ASSERT_TRUE(parsed.contains("result"));
    ASSERT_EQ(parsed["result"].size(), std::size_t{1});
    EXPECT_EQ(parsed["result"][0]["gid"], gid_of(task->id()));
    EXPECT_EQ(parsed["result"][0]["status"], "active");
}

TEST_F(JsonRpcCoverageTest, TellWaitingInvalidParams) {
    start_server();
    auto parsed = call("aria2.tellWaiting", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.tellWaiting", json::array({0}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.tellWaiting", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, TellWaitingWithIntParams) {
    start_server();
    std::vector<falcon::DownloadTask::Ptr> tasks;
    for (int i = 0; i < 3; ++i) {
        auto t = engine_.add_task("test://local/w" + std::to_string(i));
        ASSERT_TRUE(t);
        tasks.push_back(t);
    }

    auto parsed = call("aria2.tellWaiting", json::array({0, 2}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});

    parsed = call("aria2.tellWaiting", json::array({2, 5}));
    ASSERT_TRUE(parsed.contains("result"));
    ASSERT_EQ(parsed["result"].size(), std::size_t{1});

    parsed = call("aria2.tellWaiting", json::array({0, 100}));
    ASSERT_EQ(parsed["result"].size(), std::size_t{3});

    // Negative offset/num clamp to zero.
    parsed = call("aria2.tellWaiting", json::array({-5, -5}));
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});

    // Offset beyond the end.
    parsed = call("aria2.tellWaiting", json::array({50, 10}));
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});
}

TEST_F(JsonRpcCoverageTest, TellWaitingWithStringParams) {
    start_server();
    std::vector<falcon::DownloadTask::Ptr> tasks;
    for (int i = 0; i < 3; ++i) {
        auto t = engine_.add_task("test://local/ws" + std::to_string(i));
        ASSERT_TRUE(t);
        tasks.push_back(t);
    }

    auto parsed = call("aria2.tellWaiting", json::array({"0", "2"}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});

    parsed = call("aria2.tellWaiting", json::array({"1", "10"}));
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});
}

TEST_F(JsonRpcCoverageTest, TellWaitingIncludesPausedTasks) {
    start_server();
    auto pending = engine_.add_task("test://local/wp");
    auto paused = engine_.add_task("test://local/wq");
    ASSERT_TRUE(pending && paused);
    ASSERT_TRUE(engine_.pause_task(paused->id()));

    auto parsed = call("aria2.tellWaiting", json::array({0, 10}));
    ASSERT_TRUE(parsed.contains("result"));
    EXPECT_EQ(parsed["result"].size(), std::size_t{2});
}

TEST_F(JsonRpcCoverageTest, TellStoppedInvalidParams) {
    start_server();
    auto parsed = call("aria2.tellStopped", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.tellStopped", json::array({0}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
    parsed = call("aria2.tellStopped", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, TellStoppedReturnsCancelledTasks) {
    start_server();
    std::vector<falcon::DownloadTask::Ptr> tasks;
    for (int i = 0; i < 3; ++i) {
        auto t = engine_.add_task("test://local/s" + std::to_string(i));
        ASSERT_TRUE(t);
        tasks.push_back(t);
    }
    ASSERT_TRUE(engine_.cancel_task(tasks[0]->id()));
    ASSERT_TRUE(engine_.cancel_task(tasks[2]->id()));

    auto parsed = call("aria2.tellStopped", json::array({0, 10}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"].size(), std::size_t{2});

    parsed = call("aria2.tellStopped", json::array({1, 1}));
    ASSERT_EQ(parsed["result"].size(), std::size_t{1});

    parsed = call("aria2.tellStopped", json::array({"0", "10"}));
    ASSERT_EQ(parsed["result"].size(), std::size_t{2});

    parsed = call("aria2.tellStopped", json::array({0, 0}));
    EXPECT_EQ(parsed["result"].size(), std::size_t{0});
}

// ===========================================================================
// Concurrency smoke test
// ===========================================================================

TEST_F(JsonRpcCoverageTest, ConcurrentRequestsAreHandled) {
    start_server();

    std::atomic<int> ok{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([this, &ok] {
            for (int i = 0; i < 5; ++i) {
                auto full = raw_request(port(), make_http_post(
                    json{{"jsonrpc", "2.0"}, {"id", i}, {"method", "aria2.getVersion"},
                         {"params", json::array()}}
                        .dump()));
                if (full.has_value()) {
                    auto body = extract_body(*full);
                    if (body.has_value() &&
                        json::parse(*body)["result"]["version"] == "0.1.0") {
                        ok.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto& t : workers) t.join();
    EXPECT_EQ(ok.load(), 20);
}

// ===========================================================================
// Batch control / session / global options (no storage required)
// ===========================================================================

TEST_F(JsonRpcCoverageTest, SessionInfoAndSaveSession) {
    start_server();
    auto parsed = call("aria2.getSessionInfo", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_FALSE(parsed["result"]["sessionId"].get<std::string>().empty());

    parsed = call("aria2.saveSession", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
}

TEST_F(JsonRpcCoverageTest, PauseAllAndUnpauseAllReturnOk) {
    start_server();
    auto parsed = call("aria2.pauseAll", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");

    parsed = call("aria2.unpauseAll", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
}

TEST_F(JsonRpcCoverageTest, ForceShutdownInvokesHandler) {
    start_server();
    std::atomic<bool> requested{false};
    server_->set_shutdown_handler([&requested] { requested.store(true); });

    auto parsed = call("aria2.forceShutdown", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
    EXPECT_TRUE(requested.load());
}

TEST_F(JsonRpcCoverageTest, GlobalOptionRoundTrip) {
    start_server();
    auto parsed = call("aria2.changeGlobalOption",
                       json::array({json{{"max-overall-download-limit", "2048"},
                                          {"max-concurrent-downloads", "2"}}}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
    EXPECT_EQ(engine_.get_global_speed_limit(), falcon::BytesPerSecond{2048});
    EXPECT_EQ(engine_.get_max_concurrent_tasks(), std::size_t{2});

    parsed = call("aria2.getGlobalOption", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"]["max-overall-download-limit"], "2048");
    EXPECT_EQ(parsed["result"]["max-concurrent-downloads"], "2");

    // "none" 取消限速
    parsed = call("aria2.changeGlobalOption",
                  json::array({json{{"max-overall-download-limit", "none"}}}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(engine_.get_global_speed_limit(), falcon::BytesPerSecond{0});

    // 非法数值
    parsed = call("aria2.changeGlobalOption",
                  json::array({json{{"max-overall-download-limit", "not-a-number"}}}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();

    // 未知键
    parsed = call("aria2.changeGlobalOption",
                  json::array({json{{"no-such-option", "1"}}}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();

    // params 形状错误
    parsed = call("aria2.changeGlobalOption", json::array({"only-one"}));
    EXPECT_EQ(parsed["error"]["code"], -32602);
}

TEST_F(JsonRpcCoverageTest, GetFilesGetUrisGetOptionOnEngineTask) {
    start_server();
    // fixture 的 StubHandler 只认 test://；不 start_task，保持 Pending 即可查询
    auto task = engine_.add_task("test://direct.bin");
    ASSERT_NE(task, nullptr);
    const std::string gid = gid_of(task->id());

    auto parsed = call("aria2.getFiles", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    ASSERT_EQ(parsed["result"].size(), std::size_t{1});
    EXPECT_EQ(parsed["result"][0]["selected"], "true");
    EXPECT_EQ(parsed["result"][0]["uris"][0]["uri"], "test://direct.bin");

    parsed = call("aria2.getUris", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"][0]["uri"], "test://direct.bin");

    parsed = call("aria2.getOption", json::array({gid}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_TRUE(parsed["result"].contains("dir"));
    EXPECT_TRUE(parsed["result"].contains("max-download-limit"));

    // 未知 gid
    parsed = call("aria2.getFiles", json::array({"00000000000ffffffd"}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();
    parsed = call("aria2.getOption", json::array({"00000000000ffffffd"}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();
    parsed = call("aria2.getUris", json::array({"00000000000ffffffd"}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, RemoveDownloadResultUnknownGidIsError) {
    start_server();
    auto parsed = call("aria2.removeDownloadResult", json::array({"00000000000ffffffc"}));
    EXPECT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 2);
}

// ===========================================================================
// Batch P: parameter validation and edge paths across the method dispatch.
// ===========================================================================

// A handler whose download() blocks until the test releases it, so the task
// stays Downloading (active) for the duration of an RPC call.
class HangingHandler final : public falcon::IProtocolHandler {
public:
    std::string protocol_name() const override { return "hang"; }

    std::vector<std::string> supported_schemes() const override { return {"hang"}; }

    bool can_handle(const std::string& url) const override {
        return url.rfind("hang://", 0) == 0;
    }

    falcon::FileInfo get_file_info(const std::string& url,
                                   const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "hang.bin";
        info.total_size = 100;
        info.supports_resume = true;
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->update_progress(50, 100, 0);
        task->set_status(falcon::TaskStatus::Downloading);
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(10),
                     [this] { return released_.load(); });
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

    void release() {
        released_.store(true);
        cv_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> released_{false};
};

// Release the hanging worker and drive the task to a terminal state so the
// engine can be torn down without a stuck worker thread.
static void release_and_finish(falcon::DownloadEngine& engine,
                               HangingHandler& handler, falcon::TaskId id) {
    handler.release();
    engine.cancel_task(id);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto task = engine.get_task(id);
        if (!task || task->is_finished()) break;
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static bool wait_for_status(falcon::DownloadEngine& engine, falcon::TaskId id,
                            falcon::TaskStatus status) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto task = engine.get_task(id);
        if (task && task->status() == status) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

TEST_F(JsonRpcCoverageTest, ChangePriorityParameterValidation) {
    start_server();
    // 形状错误：空对象 / 单元素 / priority 非数值
    auto parsed = call("aria2.changePriority", json::object());
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    parsed = call("aria2.changePriority", json::array({"00000000000000ff"}));
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    parsed = call("aria2.changePriority", json::array({"00000000000000ff", true}));
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    // gid 非法
    parsed = call("aria2.changePriority", json::array({"zzz", 1}));
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    // 合法 gid，引擎无任务
    parsed = call("aria2.changePriority", json::array({"00000000000000ff", 1}));
    EXPECT_EQ(parsed["error"]["code"], 2) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, ChangeGlobalOptionIntegerValue) {
    start_server();
    // 整数值形式的 max-concurrent-downloads（字符串形式由既有用例覆盖）
    auto parsed = call("aria2.changeGlobalOption",
                       json::array({json{{"max-concurrent-downloads", 6}}}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
    EXPECT_EQ(engine_.get_max_concurrent_tasks(), std::size_t{6});
}

TEST_F(JsonRpcCoverageTest, TellStatusAndGetFilesParameterValidation) {
    start_server();
    auto parsed = call("aria2.tellStatus", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    for (const char* method : {"aria2.getFiles", "aria2.getUris", "aria2.getOption"}) {
        parsed = call(method, json::array());
        EXPECT_EQ(parsed["error"]["code"], -32602) << method << ": " << parsed.dump();
    }
    // 合法 gid 但引擎与 storage 均无记录
    parsed = call("aria2.getFiles", json::array({"00000000000000ff"}));
    EXPECT_EQ(parsed["error"]["code"], 2) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, PauseFamilyUnknownTaskVariants) {
    start_server();
    // 非法 gid
    for (const char* method : {"aria2.pause", "aria2.forcePause"}) {
        auto parsed = call(method, json::array({"zzz"}));
        EXPECT_EQ(parsed["error"]["code"], 2) << method << ": " << parsed.dump();
    }
    // 合法 gid 但引擎无任务
    for (const char* method :
         {"aria2.pause", "aria2.forcePause", "aria2.unpause", "aria2.remove",
          "aria2.forceRemove"}) {
        auto parsed = call(method, json::array({"00000000000000ff"}));
        EXPECT_EQ(parsed["error"]["code"], 2) << method << ": " << parsed.dump();
    }
}

TEST_F(JsonRpcCoverageTest, RemoveDownloadResultParameterAndNeverExisted) {
    start_server();
    auto parsed = call("aria2.removeDownloadResult", json::array());
    EXPECT_EQ(parsed["error"]["code"], -32602) << parsed.dump();
    // 合法 gid 但引擎与 storage 从未有过该任务
    parsed = call("aria2.removeDownloadResult", json::array({"00000000000000ff"}));
    EXPECT_EQ(parsed["error"]["code"], 2) << parsed.dump();
}

TEST_F(JsonRpcCoverageTest, RemoveDownloadResultCompletedTaskRemoved) {
    start_server();
    auto task = engine_.add_task("test://remove-me.bin");
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine_.start_task(task->id()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task->is_finished()) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto parsed = call("aria2.removeDownloadResult", json::array({gid_of(task->id())}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
}

TEST_F(JsonRpcCoverageTest, RemoveDownloadResultActiveTaskRejected) {
    auto hanging = std::make_unique<HangingHandler>();
    HangingHandler* handler = hanging.get();
    engine_.register_handler(std::move(hanging));
    start_server();
    auto task = engine_.add_task("hang://active.bin");
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine_.start_task(task->id()));
    ASSERT_TRUE(wait_for_status(engine_, task->id(), falcon::TaskStatus::Downloading));

    auto parsed = call("aria2.removeDownloadResult", json::array({gid_of(task->id())}));
    ASSERT_TRUE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["error"]["code"], 1);
    EXPECT_EQ(parsed["error"]["message"], "Task cannot be removed while active");

    release_and_finish(engine_, *handler, task->id());
}

TEST_F(JsonRpcCoverageTest, GetOptionEchoesCustomHeaders) {
    start_server();
    falcon::DownloadOptions opts;
    opts.headers["X-Custom-A"] = "1";
    opts.headers["X-Custom-B"] = "two";
    auto task = engine_.add_task("test://hdr.bin", opts);
    ASSERT_NE(task, nullptr);
    auto parsed = call("aria2.getOption", json::array({gid_of(task->id())}));
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"]["header"]["X-Custom-A"], "1");
    EXPECT_EQ(parsed["result"]["header"]["X-Custom-B"], "two");
}

TEST_F(JsonRpcCoverageTest, AddUriTimeoutOptionRoundTrip) {
    start_server();
    const std::string dir = ::testing::TempDir();
    // 字符串与整数双形态解析（aria2 addUri options 惯例），经 getOption
    // 回显验证值真实落进 DownloadOptions——此前 timeout 键零解析
    // （2026-09-30 P0 批次遗留：超时走引擎默认，per-download 不可设）
    for (const auto& [val, want] :
         {std::pair<json, std::string>{json("25"), "25"}, {json(40), "40"}}) {
        json opts = {{"dir", dir},
                     {"out", "falcon-timeout-rt-" + want + ".bin"},
                     {"timeout", val}};
        auto parsed = call("aria2.addUri",
                           json::array({json::array({"test://local/timeout.bin"}), opts}));
        ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
        const std::string gid = parsed["result"].get<std::string>();
        auto got = call("aria2.getOption", json::array({gid}));
        ASSERT_TRUE(got.contains("result")) << got.dump();
        EXPECT_EQ(got["result"]["timeout"], want);
    }
}

TEST_F(JsonRpcCoverageTest, ForceShutdownWithoutHandlerIgnored) {
    start_server();
    auto parsed = call("aria2.forceShutdown", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_EQ(parsed["result"], "OK");
    // 无 shutdown handler：仅告警，服务器照常服务
    parsed = call("system.listMethods", json::array());
    ASSERT_TRUE(parsed.contains("result")) << parsed.dump();
    EXPECT_FALSE(parsed["result"].empty());
}

#ifndef _WIN32
TEST(JsonRpcLifecycleTest, StartFailsWhenPortAlreadyBound) {
    int blocker = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(blocker, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(blocker, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(blocker, 1), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(blocker, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    const uint16_t taken = ntohs(addr.sin_port);

    falcon::DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = taken;
    cfg.bind_address = "127.0.0.1";
    falcon::daemon::rpc::JsonRpcServer server(&engine, cfg);
    EXPECT_FALSE(server.start());
    server.stop();
    ::close(blocker);
}
#endif


} // namespace
