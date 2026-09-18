/**
 * @file download_engine_v2_multi_source_test.cpp
 * @brief V2 引擎原生多源分段下载测试（Metalink 阶段2 引擎层）
 *
 * 覆盖：多镜像段分发、单 URL 行为回归、段级换源重试、初始连接
 * 镜像轮转、跨引擎断点恢复与恢复段 If-Range 规则、超时清理与段
 * 重试交互。
 *
 * 基建复用 scripted_http_server.hpp；引擎直驱模式同 resume 测试。
 */

#include <gtest/gtest.h>

#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/request_group.hpp>

#include "scripted_http_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using falcon::Bytes;
using falcon::DownloadOptions;
using falcon::DownloadEngineV2;
using falcon::EngineConfigV2;
using falcon::RequestGroup;
using falcon::RequestGroupStatus;
using falcon::TaskId;
using falcon::testscripts::FakeResponse;
using falcon::testscripts::ScriptedHttpServer;
using falcon::testscripts::TempDir;

std::string make_body(std::size_t size) {
    std::string body;
    body.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<char>('a' + (i % 26)));
    }
    return body;
}

std::string read_file_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// 支持 Range 的响应（显式 Accept-Ranges 头——V2 由 GET 响应头判定
/// 分段能力，FakeResponse 默认不带）
FakeResponse range_response(const std::string& body) {
    FakeResponse resp;
    resp.status = 200;
    resp.body = body;
    resp.support_range = true;
    resp.headers = {{"Accept-Ranges", "bytes"},
                    {"ETag", "\"etag-ms\""},
                    {"Last-Modified", "Mon, 01 Jan 2026 00:00:00 GMT"}};
    return resp;
}

/// 多源下载选项：小段阈值驱动分段
DownloadOptions multi_source_options(const std::string& out_path,
                                     std::size_t connections = 4,
                                     std::size_t min_segment = 16,
                                     std::size_t max_retries = 3) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = connections;
    options.min_segment_size = min_segment;
    options.max_retries = max_retries;
    return options;
}

/// Range 请求起点集合（从记录的 Range 头解析 "bytes=<start>-..."）
std::set<std::size_t> range_starts_of(
    const std::vector<falcon::testscripts::RecordedRequest>& requests,
    const std::string& path) {
    std::set<std::size_t> starts;
    for (const auto& r : requests) {
        if (r.path != path || r.range.rfind("bytes=", 0) != 0) continue;
        const std::string spec = r.range.substr(6);
        const auto dash = spec.find('-');
        if (dash == std::string::npos) continue;
        starts.insert(std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10));
    }
    return starts;
}

/// 特定 path 上指定起点的 Range 请求次数
std::size_t count_range_requests(
    const std::vector<falcon::testscripts::RecordedRequest>& requests,
    const std::string& path, std::size_t start) {
    std::size_t n = 0;
    for (const auto& r : requests) {
        if (r.path != path || r.range.rfind("bytes=", 0) != 0) continue;
        const std::string spec = r.range.substr(6);
        const auto dash = spec.find('-');
        if (dash == std::string::npos) continue;
        if (std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10) == start) ++n;
    }
    return n;
}

/// 跑一次下载直到任务组终态；返回是否在时限内观察到终态
bool run_until_terminal(DownloadEngineV2& engine, RequestGroup* group,
                        int timeout_seconds) {
    std::thread runner([&engine] { engine.run(); });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto st = group->status();
        if (st == RequestGroupStatus::COMPLETED || st == RequestGroupStatus::FAILED) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!finished) {
        engine.force_shutdown();
    }
    runner.join();
    return finished;
}

class MultiSourceDownload : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<TempDir>();
        body_ = make_body(64);  // 64B：min_segment=16 + 4 连接 → 恰 4 段
    }

    std::unique_ptr<TempDir> dir_;
    std::string body_;
};

/// 多镜像段分发：段 1..N-1 轮转 uris，两镜像各收到 Range GET
TEST_F(MultiSourceDownload, SegmentDistributionAcrossMirrors) {
    ScriptedHttpServer server;
    server.start();
    server.set_response("/a", range_response(body_));
    server.set_response("/b", range_response(body_));

    const std::string out_path = dir_->string() + "/dist.bin";
    // 不设 overwrite_existing：显式授权覆盖会关闭断点追踪（设计内门禁），
    // 换源重试与断点续传都需要追踪；TempDir 每用例唯一，无残留文件
    auto options = multi_source_options(out_path);

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->uris().size(), 2u);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body_);

    // 两镜像各收到带 Range 的 GET；段 1..N-1 的起点集合恰为计划
    //（段 0 由无 Range 的初始 GET 承载，复用该连接按计划截取）
    const auto reqs = server.requests();
    const auto starts_a = range_starts_of(reqs, "/a");
    const auto starts_b = range_starts_of(reqs, "/b");
    EXPECT_FALSE(starts_a.empty());
    EXPECT_FALSE(starts_b.empty());
    std::set<std::size_t> all;
    all.insert(starts_a.begin(), starts_a.end());
    all.insert(starts_b.begin(), starts_b.end());
    const std::set<std::size_t> expected = {16, 32, 48};
    EXPECT_EQ(all, expected);
}

/// 单 URL 行为回归：所有 Range GET 落在同一 path，逐字节同旧实现
TEST_F(MultiSourceDownload, SingleUrlBehaviorUnchanged) {
    ScriptedHttpServer server;
    server.start();
    server.set_response("/only", range_response(body_));

    const std::string out_path = dir_->string() + "/single.bin";
    auto options = multi_source_options(out_path);

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{server.url("/only")}, options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body_);

    // 全部带 Range 的 GET 都落在唯一 path 上（段 0 走初始 GET 无 Range）
    const auto reqs = server.requests();
    const auto starts = range_starts_of(reqs, "/only");
    EXPECT_EQ(starts, (std::set<std::size_t>{16, 32, 48}));
    for (const auto& r : reqs) {
        if (r.range.rfind("bytes=", 0) == 0) {
            EXPECT_EQ(r.path, "/only");
        }
    }
}

/// 段级换源（传输中断路径）：/a 对段 2（起点 32）发出 8 字节后断连，
/// 段 2 短传失败 → 镜像轮转换到 /b，从断点 (32+8=40) 续传；成品必须
/// 逐字节一致，且 /a 对该 Range 不再被重试（恰 1 次请求）
TEST_F(MultiSourceDownload, SegmentFailoverMidTransfer) {
    ScriptedHttpServer server;
    server.start();
    server.set_response("/a", range_response(body_));
    server.set_response("/b", range_response(body_));
    server.set_abort_after("/a", 8, 32);  // 一次性：段 2 连接发 8B 后断连

    const std::string out_path = dir_->string() + "/failover_mid.bin";
    auto options = multi_source_options(out_path);

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body_);

    // /a 收到起点 32 的 Range 恰 1 次（断连后换源，绝不重试同一镜像）
    const auto reqs = server.requests();
    EXPECT_EQ(count_range_requests(reqs, "/a", 32), 1u);
    // /b 收到从断点续传的 Range：起点 = 32 + 8 = 40（段 1/3 正常
    // 轮转落在 /b 的 16/48 之外，40 是换源续传的精确特征）
    const auto starts_b = range_starts_of(reqs, "/b");
    EXPECT_GT(starts_b.count(40), 0u);
    // 任何镜像都不该收到从 0 起的重传（换源走断点而非整段重来）
    EXPECT_EQ(starts_b.count(0), 0u);
}

/// 段级换源（响应阶段失败路径）：/b 对非零起点 Range 一律 500，
/// 落在 /b 的段（轮转分配的段 1、3）在响应校验失败后换到 /a 完成
TEST_F(MultiSourceDownload, SegmentFailoverResponseStage) {
    ScriptedHttpServer server;
    server.start();
    server.set_response("/a", range_response(body_));
    auto b_resp = range_response(body_);
    b_resp.fail_nonzero_range = true;  // Range 请求回 500
    server.set_response("/b", b_resp);

    const std::string out_path = dir_->string() + "/failover_resp.bin";
    auto options = multi_source_options(out_path);

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body_);

    // /b 的 Range 请求全部失败（500），换源后 /a 承接全部段起点
    const auto reqs = server.requests();
    const auto starts_a = range_starts_of(reqs, "/a");
    EXPECT_TRUE(starts_a.count(16) > 0 || starts_a.count(48) > 0);
    EXPECT_EQ(read_file_content(out_path), body_);
}

/// 段级重试预算耗尽：两镜像对 Range 一律 500 且 max_retries=0，
/// 首次失败即无重试资格 → 组 FAILED、错误消息非空、断点控制文件留存
TEST_F(MultiSourceDownload, SegmentRetryExhausted) {
    ScriptedHttpServer server;
    server.start();
    auto fail_resp = range_response(body_);
    fail_resp.fail_nonzero_range = true;
    server.set_response("/a", fail_resp);
    server.set_response("/b", fail_resp);

    const std::string out_path = dir_->string() + "/exhausted.bin";
    auto options = multi_source_options(out_path, 4, 16, 0);  // 预算 0

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_FALSE(group->error_message().empty());
    // 断点控制文件留存（失败收口的 save_resume 兜底），成品不出现
    EXPECT_TRUE(std::filesystem::exists(out_path + ".falcon.ctrl"));
    EXPECT_FALSE(std::filesystem::exists(out_path));
}

/// 初始连接镜像轮转（B3）：主镜像连接拒绝（端口 1 不可达）→ 换下一
/// 镜像重试成功。小文件单连接——多源不分段时初始连接同样可换源
TEST_F(MultiSourceDownload, InitialConnectionMirrorRotation) {
    ScriptedHttpServer server;
    server.start();
    server.set_response("/ok", range_response(body_));

    const std::string out_path = dir_->string() + "/init_rotate.bin";
    // 单连接 + 大段阈值 → 不分段；重试延迟 0 加速
    auto options = multi_source_options(out_path, 1, 1 << 20, 3);
    options.retry_delay_seconds = 0;

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{"http://127.0.0.1:1/x", server.url("/ok")},
        options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body_);

    // 可达镜像收到无 Range 的初始 GET（单连接，无分段请求）
    const auto reqs = server.requests();
    ASSERT_FALSE(reqs.empty());
    EXPECT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].method, "GET");
    EXPECT_EQ(reqs[0].path, "/ok");
    EXPECT_TRUE(reqs[0].range.empty());
}

/// Range 起点解析（无 Range 返回 -1）
long long parse_range_start(const std::string& range) {
    if (range.rfind("bytes=", 0) != 0) return -1;
    const std::string spec = range.substr(6);
    const auto dash = spec.find('-');
    if (dash == std::string::npos) return -1;
    return std::strtoll(spec.substr(0, dash).c_str(), nullptr, 10);
}

/// 跨引擎断点恢复（B6 + 恢复段 If-Range 规则）：阶段 1 两镜像对非零
/// Range 一律 500 且预算 0 → 组 FAILED、断点固化；阶段 2 新引擎加载
/// 控制文件续跑，未完成段按轮转分配——主镜像段带 If-Range（ETag
/// 归属者的内容变更防护），非主镜像段绝不附带
TEST_F(MultiSourceDownload, ResumeAcrossRestartMultiSource) {
    ScriptedHttpServer server;
    server.start();
    auto fail_resp = range_response(body_);
    fail_resp.fail_nonzero_range = true;
    server.set_response("/a", fail_resp);
    server.set_response("/b", fail_resp);

    const std::string out_path = dir_->string() + "/resume_restart.bin";
    auto options = multi_source_options(out_path, 4, 16, 0);  // 预算 0

    // 阶段 1：段 0 走无 Range 初始 GET（不受 fail_nonzero_range 影响，
    // ETag 在响应头阶段即入组，先于段调度必进控制文件）；轮转落
    // /b 的段 1/3 与落 /a 的段 2 全部响应阶段失败 → 组 FAILED
    {
        DownloadEngineV2 engine;
        const TaskId id = engine.add_download(
            std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
        ASSERT_GT(id, 0u);
        auto* group = engine.request_group_man()->find_group(id);
        ASSERT_NE(group, nullptr);
        ASSERT_TRUE(run_until_terminal(engine, group, 20));
        EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    }
    EXPECT_TRUE(std::filesystem::exists(out_path + ".falcon.ctrl"));
    EXPECT_FALSE(std::filesystem::exists(out_path));

    // 阶段 2 基线：镜像恢复可用，只考察此后的增量请求
    const std::size_t requests_before = server.requests().size();
    server.set_response("/a", range_response(body_));
    server.set_response("/b", range_response(body_));
    {
        DownloadEngineV2 engine;
        const TaskId id = engine.add_download(
            std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
        ASSERT_GT(id, 0u);
        auto* group = engine.request_group_man()->find_group(id);
        ASSERT_NE(group, nullptr);
        ASSERT_TRUE(run_until_terminal(engine, group, 20));
        ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    }
    EXPECT_EQ(read_file_content(out_path), body_);
    EXPECT_FALSE(std::filesystem::exists(out_path + ".falcon.ctrl"));

    // 增量请求断言：恢复段轮转后段 2（起点 32）落主镜像 /a 且带
    // If-Range == 控制文件中的 ETag；段 1/3（起点 16/48）落 /b，
    // 非主镜像不得附带 If-Range
    bool a_resume_with_ifrange = false;
    const auto& all_reqs = server.requests();
    for (std::size_t i = requests_before; i < all_reqs.size(); ++i) {
        const auto& r = all_reqs[i];
        if (r.path == "/a" && parse_range_start(r.range) == 32) {
            const auto it = r.headers.find("if-range");
            a_resume_with_ifrange =
                it != r.headers.end() && it->second == "\"etag-ms\"";
        }
        if (r.path == "/b" && parse_range_start(r.range) >= 0) {
            EXPECT_EQ(r.headers.count("if-range"), 0u)
                << "非主镜像恢复段不得带 If-Range: " << r.range;
        }
    }
    EXPECT_TRUE(a_resume_with_ifrange)
        << "主镜像恢复段必须带 If-Range（内容变更防护）";
}

/// 超时清理与段重试交互（C1）：/b 响应头立发但 3s 才发首个 body 块，
/// 任务级超时 1s 触发清理——落在这两段的命令被冲刷断点并段级换源
/// 接管，组不因单段超时连坐 FAILED，最终 COMPLETED 且成品一致
TEST_F(MultiSourceDownload, TimeoutSweepsSegmentNotGroup) {
    ScriptedHttpServer server;
    server.start();
    server.set_response("/a", range_response(body_));
    server.set_response("/b", range_response(body_));
    server.set_slow_body("/b", 3'000'000, 32);  // 头后 3s 无数据

    const std::string out_path = dir_->string() + "/timeout_seg.bin";
    auto options = multi_source_options(out_path);
    options.timeout_seconds = 1;  // 任务级超时（覆盖引擎 120s 兜底）

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    // 关键断言：段超时被段级换源吸收，组终态必须是 COMPLETED
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body_);

    // 落在 /b 的段（轮转分配的段 1、3）各恰 1 次请求——超时后换到
    // /a，绝不在 /b 上重试
    std::size_t b_requests = 0;
    for (const auto& r : server.requests()) {
        if (r.path == "/b") ++b_requests;
    }
    EXPECT_EQ(b_requests, 2u);
}


/// 段级重定向携带段号：轮转落 /b 的段遇 302 → 跟随连接必须携带该段
/// 的 Range（重定向后的镜像承接该段），/redir 正常 206 → COMPLETED
TEST_F(MultiSourceDownload, SegmentRedirectCarriesSegment) {
    ScriptedHttpServer server;
    server.start();
    server.set_response("/a", range_response(body_));
    FakeResponse redirect;
    redirect.status = 302;
    redirect.status_text = "Found";
    redirect.headers = {{"Location", "/redir"}};
    server.set_response("/b", redirect);
    server.set_response("/redir", range_response(body_));

    const std::string out_path = dir_->string() + "/seg_redirect.bin";
    auto options = multi_source_options(out_path);

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body_);

    // /b 上的段全部 302 到 /redir：跟随请求必须带同起点的 Range
    const auto reqs = server.requests();
    const auto starts_b = range_starts_of(reqs, "/b");
    ASSERT_FALSE(starts_b.empty());
    const auto starts_redir = range_starts_of(reqs, "/redir");
    EXPECT_EQ(starts_redir, starts_b);
    // 302 后不在 /b 重试：每个起点恰 1 次请求
    for (const auto start : starts_b) {
        EXPECT_EQ(count_range_requests(reqs, "/b", start), 1u);
    }
}

/// 段重定向后跟随目标失败：failed_url 为重定向后 URL（不在 uris
/// 列表），轮转换源回落主镜像 /a 重下该段 → COMPLETED
TEST_F(MultiSourceDownload, SegmentRedirectFallbackToPrimaryMirror) {
    ScriptedHttpServer server;
    server.start();
    server.set_response("/a", range_response(body_));
    FakeResponse redirect;
    redirect.status = 302;
    redirect.status_text = "Found";
    redirect.headers = {{"Location", "/redir"}};
    server.set_response("/b", redirect);
    FakeResponse broken;
    broken.status = 500;
    broken.status_text = "Internal Server Error";
    server.set_response("/redir", broken);

    const std::string out_path = dir_->string() + "/seg_redir_fallback.bin";
    auto options = multi_source_options(out_path, 4, 16, 1);  // 每段预算 1

    DownloadEngineV2 engine;
    const TaskId id = engine.add_download(
        std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    ASSERT_TRUE(run_until_terminal(engine, group, 20));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content(out_path), body_);

    // 落 /b 的段各恰 1 次 302（失败后不回 /b）、跟随 /redir 各恰 1 次
    // 500；换源轮转的 failed_url 不在 uris → 回落主镜像 /a 接管
    const auto reqs = server.requests();
    const auto starts_b = range_starts_of(reqs, "/b");
    ASSERT_FALSE(starts_b.empty());
    EXPECT_EQ(range_starts_of(reqs, "/redir"), starts_b);
    for (const auto start : starts_b) {
        EXPECT_EQ(count_range_requests(reqs, "/a", start), 1u)
            << "重定向后 URL 不在镜像列表，该段必须回落主镜像: " << start;
    }
}

/// 恢复段响应阶段超时：阶段 1 留断点，阶段 2 主镜像 /a 对段 0 的
/// 恢复请求收下连接却不回响应头 → 任务级超时清理命中恢复段的
/// 响应命令 → 段级换源接管（非主镜像 /b 无 If-Range 完成收尾）
TEST_F(MultiSourceDownload, ResumeResponseStageTimeoutSwitchesMirror) {
    ScriptedHttpServer server;
    server.start();
    auto fail_resp = range_response(body_);
    fail_resp.fail_nonzero_range = true;
    server.set_response("/a", fail_resp);
    server.set_response("/b", fail_resp);

    const std::string out_path = dir_->string() + "/resume_resp_timeout.bin";
    auto options = multi_source_options(out_path, 4, 16, 0);  // 预算 0

    // 阶段 1：全部段响应失败 → FAILED，断点留存
    {
        DownloadEngineV2 engine;
        const TaskId id = engine.add_download(
            std::vector<std::string>{server.url("/a"), server.url("/b")}, options);
        ASSERT_GT(id, 0u);
        auto* group = engine.request_group_man()->find_group(id);
        ASSERT_NE(group, nullptr);
        ASSERT_TRUE(run_until_terminal(engine, group, 20));
        EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    }
    ASSERT_TRUE(std::filesystem::exists(out_path + ".falcon.ctrl"));

    // 阶段 2：段 0 已由无 Range 初始 GET 承载完成，第一个未完成段
    // 是段 1（起点 16），恢复初始连接按段号轮转落 /b——对该请求
    // 黑洞（收连接不回响应头），任务级超时 1s → 响应命令超时清理
    // → 段级换源（换到主镜像 /a，携带 If-Range）；/a 其余段正常
    server.set_response("/a", range_response(body_));
    server.set_response("/b", range_response(body_));
    server.set_black_hole("/b", 16);
    const std::size_t requests_before = server.requests().size();
    auto resume_options = options;
    resume_options.timeout_seconds = 1;
    resume_options.max_retries = 1;  // 超时换源需要预算
    {
        DownloadEngineV2 engine;
        const TaskId id = engine.add_download(
            std::vector<std::string>{server.url("/a"), server.url("/b")},
            resume_options);
        ASSERT_GT(id, 0u);
        auto* group = engine.request_group_man()->find_group(id);
        ASSERT_NE(group, nullptr);
        ASSERT_TRUE(run_until_terminal(engine, group, 25));
        ASSERT_TRUE(run_until_terminal(engine, group, 25));
        ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    }
    EXPECT_EQ(read_file_content(out_path), body_);
    EXPECT_FALSE(std::filesystem::exists(out_path + ".falcon.ctrl"));

    // 段 1（起点 16）的恢复请求在 /b 恰 1 次（黑洞，超时后不在 /b
    // 重试），换源收尾落在主镜像 /a（带 If-Range）；口径只数阶段
    // 2 增量——阶段 1 的 /b 16-31（500 失败）不计入
    const auto all_reqs = server.requests();
    std::size_t a16 = 0, b16 = 0;
    bool a16_ifrange = false;
    for (std::size_t i = requests_before; i < all_reqs.size(); ++i) {
        const auto& r = all_reqs[i];
        if (r.path == "/a" && parse_range_start(r.range) == 16) {
            ++a16;
            const auto it = r.headers.find("if-range");
            a16_ifrange = it != r.headers.end() && it->second == "\"etag-ms\"";
        }
        if (r.path == "/b" && parse_range_start(r.range) == 16) ++b16;
    }
    EXPECT_EQ(a16, 1u);
    EXPECT_EQ(b16, 1u);
    EXPECT_TRUE(a16_ifrange) << "换源到主镜像的恢复收尾必须带 If-Range";
}

} // namespace
