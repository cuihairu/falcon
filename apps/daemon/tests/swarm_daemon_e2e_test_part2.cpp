// ============================================================================
// daemon P2SP 阶段 1 增量 4 真二进制 e2e（SwarmDaemonE2E，POSIX-only）
//
// fork + execv 真二进制（FALCON_SWARMD_BIN / FALCON_DAEMON_BIN 编译定义）：
//   - 规则 (a) 完成→查询→删文件撤销：真 falcon-daemon 从真 HTTP 服务器下载
//     完成（share.enabled=true, hash_delay_s=0）→ 公告到达真 falcon-swarmd
//     → 进程内 SwarmClient query 命中且元数据一致（sha256/name/size + node
//     source node_id == daemon 报的 node_id）→ 删除成品文件 → 文件消失
//     retract → 落空（sources 空）+ 撤销后 announce 再次 query 仍空
//   - 规则 (b) Rendezvous 被杀无感：A/B 两真 daemon 共享一个 Rendezvous，
//     下载中（慢发窗口）SIGKILL rendezvous → 两端下载照常完成，成品逐字节
//     一致（数据面零 Rendezvous 依赖）
//   - 规则 (c) 默认关（share.enabled 缺省 false）：整条链路在默认配置下
//     零公告——本用例断言默认 daemon 的 falcon.swarm.status.enabled == false
//     且 query 落空
//
// 环境隔离：每用例 setenv("HOME", <mkdtemp>) —— daemon 的
// ~/.config/falcon/{swarm_key.pem,tasks.db} 落临时目录不再污染仓库工作树；
// 子进程经 fork/execv 继承环境（home 前缀）。
//
// gcda 铁律：子进程恒 SIGTERM 优雅收尾（SIGKILL 仅兜底），杀 Rendezvous
// 的规则 (b) 用例按「Rendezvous 无覆盖产出」豁免（swarmd 覆盖由自身
// e2e 承担）。
// ============================================================================

// daemon tests 子目录在根 CMakeLists 中先于 swarmd 子目录处理——本目标
// 在 daemon tests CMakeLists 注册（无条件、POSIX-only），宏与 swarm 头
// 路径由根 CMakeLists 在 swarmd 目标之后事后补注入（与 daemon/CLI 的
// FALCON_HAS_SWARM 补链同款形态）。宏缺席（无 CURL / swarmd 未构建）时
// 退化为 skip 占位，套件不假绿也不阻断构建。

#ifdef FALCON_SWARM_E2E_ENABLED

#include "client/swarm_client.hpp"
#include "common/swarm_crypto.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#error "This test is POSIX-only (guarded by CMake NOT WIN32)"
#endif

using namespace falcon::swarm;

namespace {

// ---- 子进程管理（沿 swarmd e2e 骨架）-------------------------------------

struct Proc {
    pid_t pid = -1;
    int out_fd = -1;
    int err_fd = -1;
    bool exited = false;
    int exit_status = 0;

    std::string out;
    std::string err;
};

bool proc_start(Proc& p, const char* bin,
                const std::vector<std::string>& args) {
    int out_pipe[2];
    int err_pipe[2];
    if (::pipe(out_pipe) != 0) return false;
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
            ::close(fd);
        }
        return false;
    }

    if (pid == 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        if (out_pipe[1] > STDERR_FILENO) ::close(out_pipe[1]);
        if (err_pipe[1] > STDERR_FILENO) ::close(err_pipe[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(bin, argv.data());
        ::_exit(127);  // exec failed
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    p.pid = pid;
    p.out_fd = out_pipe[0];
    p.err_fd = err_pipe[0];
    ::fcntl(p.out_fd, F_SETFL, ::fcntl(p.out_fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(p.err_fd, F_SETFL, ::fcntl(p.err_fd, F_GETFL) | O_NONBLOCK);
    return true;
}


SwarmKeyMaterial memory_key() {
    SwarmKeyMaterial m;
    auto kp = SwarmCrypto::generate_keypair();
    m.private_seed = std::move(kp.private_seed);
    m.public_key_der = std::move(kp.public_key_der);
    m.node_id = SwarmCrypto::fingerprint(m.public_key_der);
    m.pubkey_hex = SwarmCrypto::bytes_to_hex(m.public_key_der.data(),
                                             m.public_key_der.size());
    return m;
}

// ---- 可复用夹具：Rendezvous + daemon 配置 ---------------------------------

// 写 daemon.json：p2sp.share.enabled + rendezvous + rpc。
// ttl_s 越小续租/消失检查越快（renewal = clamp(ttl/3, poll, cap)）——
// 规则 (a) 用 9s（≈3s retract 周期）避免 30s 等待依赖默认 1d TTL
std::string write_daemon_conf(const std::string& dir, const char* name,
                              bool share_enabled, std::uint16_t rdv_port,
                              const std::string& server_token,
                              const std::string& group_token,
                              long ttl_s = 86400,
                              const std::string& advertise_addr = "") {
    nlohmann::json conf;
    conf["rpc"] = {{"enabled", true},
                   {"host", "127.0.0.1"},
                   {"port", 0}};
    conf["p2sp"]["share"] = {{"enabled", share_enabled},
                             {"mode", "standard"},
                             {"ttl_s", ttl_s},
                             {"hash_delay_s", 0},
                             {"announce_mirrors", false}};
    conf["p2sp"]["rendezvous"] = {{"host", "127.0.0.1"},
                                  {"port", static_cast<int>(rdv_port)},
                                  {"server_token", server_token},
                                  {"group_token", group_token}};
    // 缺省参数不写键——既有用例的配置 JSON 逐键不变
    if (!advertise_addr.empty()) {
        conf["p2sp"]["rendezvous"]["advertise_addr"] = advertise_addr;
    }
    return write_temp_file_at(dir, name, conf.dump(2));
}

}  // namespace

// ============================================================================
// 规则 (a)：完成 → 查询命中(元数据一致) → 删文件 → 落空
// ============================================================================
TEST(SwarmDaemonE2E, AnnounceQueryThenDeleteRetracts) {
    const std::string home = make_temp_dir("falcon_daemon_e2e_a");
    ASSERT_FALSE(home.empty());
    ::setenv("HOME", home.c_str(), 1);

    const std::string body = "Falcon P2SP daemon e2e payload A\n";
    const std::string expect_sha = SwarmCrypto::sha256_hex(body);

    HttpMirror mirror;
    ASSERT_TRUE(mirror.start(body));

    const std::uint16_t rdv_port = pick_free_port();
    const std::string server_token = "tok-a";
    const std::string group_token = "gtok-a";

    // 真 falcon-swarmd（Rendezvous）——端口可连即就绪（stdout 管道全缓冲，
    // banner 探测不可靠）
    Proc rdv;
    ASSERT_TRUE(proc_start(rdv, FALCON_SWARMD_BIN,
                           {"--no-conf", "--swarm-host", "127.0.0.1",
                            "--swarm-port", std::to_string(rdv_port),
                            "--server-token", server_token,
                            "--group-token", group_token}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(rdv_port); }, 10000));

    // 真 falcon-daemon（share.enabled + rendezvous 指向上面）
    const std::string dir = home + "/dl";
    ::mkdir(dir.c_str(), 0755);
    const std::string conf = write_daemon_conf(
        home, "daemon_a.json", true, rdv_port, server_token, group_token,
        /*ttl_s=*/9);
    ASSERT_FALSE(conf.empty());

    const std::uint16_t daemon_port = pick_free_port();
    Proc daemon;
    // 注意：--no-conf 会连显式 --conf-path 一并忽略——必须不带 --no-conf
    ASSERT_TRUE(proc_start(
        daemon, FALCON_DAEMON_BIN,
        {"--enable-rpc", "--rpc-listen-host", "127.0.0.1",
         "--rpc-listen-port", std::to_string(daemon_port), "--task-db",
         home + "/tasks_a.db", "--conf-path", conf}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(daemon_port); }, 15000));

    // 等 daemon 侧 SwarmAnnouncer 注册进 Rendezvous
    auto status_registered = [&] {
        auto r = rpc_call(daemon_port, "falcon.swarm.status",
                          nlohmann::json::array());
        return r.value("result", nlohmann::json::object())
                   .value("registered", false);
    };
    ASSERT_TRUE(wait_until(status_registered, 20000))
        << "daemon never registered; stderr=" << daemon.err;

    auto status0 = rpc_call(daemon_port, "falcon.swarm.status",
                            nlohmann::json::array())["result"];
    const std::string daemon_node_id = status0.value("node_id", std::string());
    ASSERT_FALSE(daemon_node_id.empty());
    EXPECT_TRUE(status0.value("enabled", false));

    // 触发下载（HTTP GET → 完成后公告）
    const std::string out_path = dir + "/payload.bin";
    auto add = rpc_call(daemon_port, "aria2.addUri",
                        nlohmann::json::array(
                            {nlohmann::json::array({mirror.url()}),
                             nlohmann::json{{"dir", dir},
                                            {"allow-overwrite", "true"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();
    const std::string gid = add["result"].get<std::string>();
    ASSERT_EQ(wait_task_terminal(daemon_port, gid, 20000), "complete")
        << "daemon.err=" << daemon.err;
    ASSERT_EQ(read_file_if_exists(out_path), body);

    // Rendezvous 侧：等公告到达（用独立节点 SwarmClient query 命中）
    auto cfg = SwarmClientConfig{};
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<int>(rdv_port);
    cfg.server_token = server_token;
    cfg.group_token = group_token;
    cfg.rpc_timeout_ms = std::chrono::milliseconds(2000);
    cfg.reconnect_delay = std::chrono::milliseconds(100);
    SwarmClient qnode(cfg, memory_key());
    std::string qerr;
    ASSERT_TRUE(qnode.start(&qerr)) << qerr;

    // rdv 默认 query 限频 120/min（§9.1）——20ms 轮询 2.4s 即烧穿预算，
    // 此后本 key 恒 -32002，查询永不稳定 ok。600ms 间隔 ≤100/min 保持在限内
    std::string qnode_last_err;
    auto query_hit = [&] {
        nlohmann::json result;
        auto err = qnode.query(expect_sha, &result);
        if (!err.ok()) {
            qnode_last_err =
                std::to_string(err.code) + ": " + err.message;
            return false;
        }
        const auto srcs = result.value("sources", nlohmann::json::array());
        return !srcs.empty();
    };
    ASSERT_TRUE(wait_until(query_hit, 20000, 600))
        << "announcement never reached rendezvous; last query err: "
        << qnode_last_err;

    // 元数据一致性：sha256 / name / size / node source node_id
    nlohmann::json hit;
    ASSERT_TRUE(qnode.query(expect_sha, &hit).ok());
    EXPECT_EQ(hit.value("sha256", std::string()), expect_sha);
    EXPECT_EQ(hit.value("name", std::string()), "payload.bin");
    EXPECT_EQ(hit.value("size", std::uint64_t(0)),
              static_cast<std::uint64_t>(body.size()));
    const auto& srcs = hit["sources"];
    ASSERT_FALSE(srcs.empty());
    EXPECT_EQ(srcs[0].value("type", std::string()), "node");
    EXPECT_EQ(srcs[0].value("node_id", std::string()), daemon_node_id);

    // 删文件 → SwarmAnnouncer retract → 落空
    ASSERT_EQ(::unlink(out_path.c_str()), 0);
    auto query_empty = [&] {
        nlohmann::json result;
        auto err = qnode.query(expect_sha, &result);
        if (!err.ok()) {
            qnode_last_err =
                std::to_string(err.code) + ": " + err.message;
            return false;
        }
        return result.value("sources", nlohmann::json::array()).empty();
    };
    EXPECT_TRUE(wait_until(query_empty, 30000, 600))
        << "retract after delete never propagated; last query err: "
        << qnode_last_err;

    qnode.stop();
    proc_terminate(daemon);
    proc_terminate(rdv);
    EXPECT_EQ(exit_code_of(daemon), 0) << daemon.err;
    EXPECT_EQ(exit_code_of(rdv), 0) << rdv.err;
}

TEST(SwarmDaemonE2E, RendezvousKilledMidDownloadIsTransparent) {
    const std::string home_a = make_temp_dir("falcon_daemon_e2e_b_a");
    ASSERT_FALSE(home_a.empty());

    const std::uint16_t rdv_port_num = pick_free_port();
    const std::string rdv_port = std::to_string(rdv_port_num);
    const std::string server_token = "tok-b";
    const std::string group_token = "gtok-b";

    Proc rdv;
    ASSERT_TRUE(proc_start(
        rdv, FALCON_SWARMD_BIN,
        {"--no-conf", "--swarm-host", "127.0.0.1", "--swarm-port", rdv_port,
         "--server-token", server_token, "--group-token", group_token}));
    ASSERT_TRUE(
        wait_until([&] { return tcp_connect(rdv_port_num); }, 10000));

    const std::string body(256 * 1024, 'B');  // 256 KiB
    HttpMirror mirror;
    ASSERT_TRUE(mirror.start(body));
    // 慢发拉开下载窗（256KiB / 8KB = 32 块 × 40ms ≈ 1.3s），确保
    // "进行中" 与 "杀 Rendezvous" 时序可排序
    mirror.set_slow(8 * 1024, 40);

    // A 与 B 各自独立 HOME + task db（模拟两台节点）
    struct NodeProc {
        std::string home;
        Proc proc;
        std::string dir;
        std::string out;
    };
    NodeProc a, b;
    a.home = home_a;
    b.home = make_temp_dir("falcon_daemon_e2e_b_b");

    auto spawn_node = [&](NodeProc& n, const char* name,
                          std::uint16_t port) {
        ::mkdir((n.home + "/dl").c_str(), 0755);
        n.dir = n.home + "/dl";
        n.out = n.dir + "/payload.bin";
        const std::string conf = write_daemon_conf(
            n.home, name, true, static_cast<std::uint16_t>(
                                   std::stoi(rdv_port)),
            server_token, group_token);
        EXPECT_FALSE(conf.empty());
        // setenv HOME 影响本进程；fork/execv 继承 → 但 A/B 用各自 conf 已
        // 固定路径，HOME 仅决定 swarm_key.pem/tasks.db 落点。
        // 不带 --no-conf（否则显式 --conf-path 也被忽略，conf 不生效）
        return proc_start(n.proc, FALCON_DAEMON_BIN,
                          {"--enable-rpc", "--rpc-listen-host",
                           "127.0.0.1", "--rpc-listen-port",
                           std::to_string(port), "--task-db",
                           n.home + "/tasks.db", "--conf-path", conf});
    };

    // HOME 需在 spawn 前设到该节点目录（子进程继承）
    ::setenv("HOME", a.home.c_str(), 1);
    const std::uint16_t a_port = pick_free_port();
    ASSERT_TRUE(spawn_node(a, "daemon_ba.json", a_port));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(a_port); }, 15000));

    ::setenv("HOME", b.home.c_str(), 1);
    const std::uint16_t b_port = pick_free_port();
    ASSERT_TRUE(spawn_node(b, "daemon_bb.json", b_port));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(b_port); }, 15000));

    auto registered = [&](std::uint16_t port) {
        auto r = rpc_call(port, "falcon.swarm.status", nlohmann::json::array());
        return r.value("result", nlohmann::json::object())
            .value("registered", false);
    };
    ASSERT_TRUE(wait_until([&] { return registered(a_port); }, 20000));
    ASSERT_TRUE(wait_until([&] { return registered(b_port); }, 20000));

    // 两端各起下载
    auto start_dl = [&](NodeProc& n, std::uint16_t port) {
        return rpc_call(port, "aria2.addUri",
                        nlohmann::json::array(
                            {nlohmann::json::array({mirror.url()}),
                             nlohmann::json{{"dir", n.dir},
                                            {"allow-overwrite", "true"}}}));
    };
    const auto add_a = start_dl(a, a_port);
    const auto add_b = start_dl(b, b_port);
    ASSERT_TRUE(add_a.contains("result")) << add_a.dump();
    ASSERT_TRUE(add_b.contains("result")) << add_b.dump();
    const std::string gid_a = add_a["result"].get<std::string>();
    const std::string gid_b = add_b["result"].get<std::string>();

    // 下载进行中杀掉 Rendezvous（硬杀，模拟崩溃）
    ASSERT_TRUE(wait_until(
        [&] {
            auto r = rpc_call(a_port, "aria2.tellActive", nlohmann::json::array());
            return !r.value("result", nlohmann::json::array()).empty();
        },
        15000));
    proc_kill9(rdv);

    // 数据面无 Rendezvous 依赖：两端照常完成，成品逐字节一致
    ASSERT_EQ(wait_task_terminal(a_port, gid_a, 30000), "complete")
        << "daemon A err=" << a.proc.err;
    ASSERT_EQ(wait_task_terminal(b_port, gid_b, 30000), "complete")
        << "daemon B err=" << b.proc.err;

    EXPECT_EQ(read_file_if_exists(a.out), body);
    EXPECT_EQ(read_file_if_exists(b.out), body);

    proc_terminate(a.proc);
    proc_terminate(b.proc);
    EXPECT_EQ(exit_code_of(a.proc), 0) << a.proc.err;
    EXPECT_EQ(exit_code_of(b.proc), 0) << b.proc.err;
    // rendezvous 已硬杀（无 flush 产出预期）
}

// ============================================================================
// 规则 (c)：默认关（share.enabled=false）→ 零公告
// ============================================================================
TEST(SwarmDaemonE2E, DefaultOffAnnouncesNothing) {
    const std::string home = make_temp_dir("falcon_daemon_e2e_c");
    ASSERT_FALSE(home.empty());
    ::setenv("HOME", home.c_str(), 1);

    const std::string body = "default off payload C\n";
    const std::string expect_sha = SwarmCrypto::sha256_hex(body);

    HttpMirror mirror;
    ASSERT_TRUE(mirror.start(body));

    const std::uint16_t rdv_port = pick_free_port();
    Proc rdv;
    ASSERT_TRUE(proc_start(rdv, FALCON_SWARMD_BIN,
                           {"--no-conf", "--swarm-host", "127.0.0.1",
                            "--swarm-port", std::to_string(rdv_port)}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(rdv_port); }, 10000));

    // share.enabled=false（默认关）
    const std::string dir = home + "/dl";
    ::mkdir(dir.c_str(), 0755);
    const std::string conf =
        write_daemon_conf(home, "daemon_c.json", false, rdv_port, "", "");
    ASSERT_FALSE(conf.empty());

    const std::uint16_t daemon_port = pick_free_port();
    Proc daemon;
    ASSERT_TRUE(proc_start(
        daemon, FALCON_DAEMON_BIN,
        {"--enable-rpc", "--rpc-listen-host", "127.0.0.1",
         "--rpc-listen-port", std::to_string(daemon_port), "--task-db",
         home + "/tasks_c.db", "--conf-path", conf}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(daemon_port); }, 15000));

    // 默认关：status.enabled == false
    auto st = rpc_call(daemon_port, "falcon.swarm.status",
                       nlohmann::json::array());
    ASSERT_TRUE(st.contains("result"));
    EXPECT_FALSE(st["result"].value("enabled", true));

    // 下载完成后仍无公告（query 命中空）
    const std::string out_path = dir + "/payload.bin";
    const auto add = rpc_call(daemon_port, "aria2.addUri",
                              nlohmann::json::array(
                                  {nlohmann::json::array({mirror.url()}),
                                   nlohmann::json{{"dir", dir},
                                                  {"allow-overwrite", "true"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();
    ASSERT_EQ(wait_task_terminal(daemon_port, add["result"].get<std::string>(),
                                 20000),
              "complete")
        << "daemon.err=" << daemon.err;
    ASSERT_EQ(read_file_if_exists(out_path), body);

    // 独立节点 query 落空
    auto cfg = SwarmClientConfig{};
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<int>(rdv_port);
    cfg.rpc_timeout_ms = std::chrono::milliseconds(2000);
    SwarmClient qnode(cfg, memory_key());
    std::string qerr;
    ASSERT_TRUE(qnode.start(&qerr)) << qerr;

    // 等查询通道本身可用（连上应答），再断言零公告——固定 sleep 对
    // qnode 首连时序是竞速
    nlohmann::json result;
    ASSERT_TRUE(wait_until(
        [&] { return qnode.query(expect_sha, &result).ok(); }, 10000,
        600)) << "query channel never became usable";
    EXPECT_TRUE(result.value("sources", nlohmann::json::array()).empty());

    qnode.stop();
    proc_terminate(daemon);
    proc_terminate(rdv);
    EXPECT_EQ(exit_code_of(daemon), 0) << daemon.err;
    EXPECT_EQ(exit_code_of(rdv), 0) << rdv.err;
}

#else  // !FALCON_SWARM_E2E_ENABLED

#include <gtest/gtest.h>

TEST(SwarmDaemonE2E, DisabledWithoutSwarmBuild) {
    GTEST_SKIP() << "swarm daemon e2e requires falcon_swarm_client + "
                    "falcon-swarmd (FALCON_SWARM_E2E_ENABLED)";
}

#endif  // FALCON_SWARM_E2E_ENABLED
