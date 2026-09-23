/**
 * @file bittorrent_dht_test.cpp
 * @brief libtorrent 模式 DHT 会话配置与回环发现测试
 * @author Falcon Team
 * @date 2026-09-23
 *
 * 产品 BT 路径的 DHT 由 libtorrent session 原生承载（此前处于
 * 「隐式默认」状态——从未显式配置）。本文件钉住三件事：
 *
 * 1. 会话配置单一事实源（make_session_settings）显式声明 DHT 语义
 *    ——enable_dht=true + 公共引导节点表，防上游版本静默改写默认
 * 2. handler 运行时契约：构造后 DHT 运行、configure_private_mode
 *    关停（既有 DhtStartStopOnEphemeralPort /
 *    ConfigurePrivateModeStopsDht 在 libtorrent 模式均为 skip，
 *    本用例补齐库模式等价覆盖）
 * 3. 三 session 回环 DHT 真实性（两跳传播）：bootstrap 目标在
 *    libtorrent 中被设计性排除在路由表之外（routing_table 的
 *    m_router_nodes 集合对 router 端点直接拒绝入表）——「直连
 *    bootstrap 对方入表」结构性不成立。正确观测点是第三方节点经
 *    引导中转传播入表：C 先引导到 A（C 进入 A 的路由表），B 再
 *    引导到 A 时 A 的响应 nodes 字段携带 C，C 进入 B 的路由表
 *    （C 不是 B 的 router，无排除）。回环不触网——A 的引导表
 *    清空为纯被动节点
 *
 * 非库模式（FALCON_USE_LIBTORRENT 未定义）session/DHT 配置面不存
 * 在，仅保留 skip 占位（自研 DhtClient 行为由 dht_node_test 覆盖）。
 */

#include <gtest/gtest.h>

#ifdef FALCON_USE_LIBTORRENT

#include <falcon/plugins/bittorrent/bittorrent_plugin.hpp>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace falcon;
using namespace falcon::protocols;

namespace {

/// 50ms 步进轮询；cond() 为 true 即返回 true，超时后做最后一次判定
template <typename Cond>
bool waitFor(Cond&& cond, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return cond();
}

/// 回环测试 session 设置：make_session_settings 基础上钉死监听
/// （127.0.0.1 随机端口，不触外网接口）与引导节点表（bootstrap）；
/// dht_log 开启 DHT 模块逐行日志（诊断轮用）
libtorrent::settings_pack makeLoopbackSettings(const std::string& bootstrap,
                                               bool dht_log = false) {
    auto pack = BitTorrentHandler::make_session_settings();
    pack.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    pack.set_str(lt::settings_pack::dht_bootstrap_nodes, bootstrap);
    pack.set_int(lt::settings_pack::alert_mask,
                 lt::alert_category::status | lt::alert_category::dht |
                     lt::alert_category::error |
                     (dht_log ? lt::alert_category::dht_log
                              : lt::alert_category_t{}));
    return pack;
}

/// 就绪态（nid 非零 + DHT UDP 端口已知）的一次性采集
struct DhtReady {
    lt::sha1_hash nid{};
    int port = 0;
    lt::udp::endpoint dht_endpoint{};
};

/// 等 session 的 DHT 就绪。nid 与 listen 端口**同一循环**收集——
/// pop_alerts 消费即弃，listen_succeeded_alert 若在 DHT 就绪等待
/// 中被弹出而未记录就永远拿不到端口（dht_stats_alert 是
/// post_dht_stats 的 API 响应，DHT 实例不存在则无 alert——alert
/// 到达本身即「DHT 在运行」的证据）。DHT 绑定在 listen socket 上
bool waitForReady(lt::session& ses, DhtReady& out,
                  std::chrono::milliseconds timeout) {
    return waitFor(
        [&] {
            ses.post_dht_stats();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::vector<lt::alert*> alerts;
            ses.pop_alerts(&alerts);
            for (auto* al : alerts) {
                if (auto* ds = lt::alert_cast<lt::dht_stats_alert>(al)) {
                    out.nid = ds->nid;
                    out.dht_endpoint = ds->local_endpoint;
                } else if (auto* ls =
                               lt::alert_cast<lt::listen_succeeded_alert>(
                                   al)) {
                    out.port = ls->port;
                }
            }
            return !out.nid.is_all_zeros() && out.port > 0;
        },
        timeout);
}

/// 路由表计数（主表 + replacement cache 分列——find_node 只返回
/// 主表 confirmed 节点，replacement 不参与引导响应，防误判）
struct RoutingCounts {
    int nodes = 0;
    int replacements = 0;
};

/// post_dht_stats 并弹出全部 alert：统计路由表计数，其余 alert 的
/// 类型名可选汇入 diag（顺带消费，防 alert 队列膨胀）
RoutingCounts drainStats(lt::session& ses, std::string* diag = nullptr) {
    RoutingCounts counts;
    std::vector<lt::alert*> alerts;
    ses.post_dht_stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ses.pop_alerts(&alerts);
    for (auto* al : alerts) {
        if (auto* ds = lt::alert_cast<lt::dht_stats_alert>(al)) {
            for (auto const& bucket : ds->routing_table) {
                counts.nodes += bucket.num_nodes;
                counts.replacements += bucket.num_replacements;
            }
        } else if (diag) {
            *diag += std::string(al->what()) + " ";
        }
    }
    return counts;
}

}  // namespace

//==============================================================================
// 会话配置单一事实源
//==============================================================================

TEST(BitTorrentDhtSettings, MakeSessionSettingsExplicitlyEnablesDht) {
    const auto pack = BitTorrentHandler::make_session_settings();

    EXPECT_TRUE(pack.get_bool(lt::settings_pack::enable_dht));

    // 公共引导节点表：节点清单与 libtorrent 内置默认一致，但必须
    // 显式写出（契约是「我方显式声明」而非「依赖上游默认」）
    const std::string bootstrap =
        pack.get_str(lt::settings_pack::dht_bootstrap_nodes);
    EXPECT_NE(bootstrap.find("dht.libtorrent.org:25401"), std::string::npos);
    EXPECT_NE(bootstrap.find("router.bittorrent.com:6881"), std::string::npos);
    EXPECT_NE(bootstrap.find("dht.transmissionbt.com:6881"), std::string::npos);
    EXPECT_NE(bootstrap.find("router.utorrent.com:6881"), std::string::npos);
    // 逗号分隔四节点（不多不少）
    EXPECT_EQ(std::count(bootstrap.begin(), bootstrap.end(), ','), 3);
}

//==============================================================================
// handler 运行时契约（libtorrent 模式）
//==============================================================================

TEST(BitTorrentDhtHandler, DhtRunsByDefaultAndPrivateModeStopsIt) {
    BitTorrentHandler handler;
    // 回环随机端口：默认 6881 可能被占用，listen 失败会连带 DHT 不起
    handler.set_listen_interfaces("127.0.0.1:0");

    // session DHT 模块异步启动
    ASSERT_TRUE(waitFor([&] { return handler.isDhtRunning(); },
                        std::chrono::seconds(15)));

    handler.configure_private_mode();
    EXPECT_TRUE(waitFor([&] { return !handler.isDhtRunning(); },
                        std::chrono::seconds(15)));
}

//==============================================================================
// 三 session 回环 bootstrap（回环不触网：A 引导表清空 = 纯被动）
//==============================================================================

TEST(BitTorrentDhtLoopback, ThirdNodeEntersRoutingTableViaBootstrapRelay) {
    // A：无引导（纯被动 DHT 节点），回环随机端口
    lt::session a(makeLoopbackSettings(""));
    DhtReady ra;
    ASSERT_TRUE(waitForReady(a, ra, std::chrono::seconds(15)))
        << "A 的 DHT 未就绪（nid 空或未监听成功）";
    const std::string aEp =
        "127.0.0.1:" + std::to_string(ra.dht_endpoint.port());

    // C：引导到 A。bootstrap 目标（A）被 libtorrent 设计性排除在
    // C 的路由表之外（router 不入表），但 C 与 A 的查询/应答交互
    // 使 C 进入 **A** 的路由表——这是 A 引导响应携带 nodes 的前提
    // （find_node 只返回主表 confirmed 节点）
    lt::session c(makeLoopbackSettings(aEp));
    DhtReady rc;
    ASSERT_TRUE(waitForReady(c, rc, std::chrono::seconds(15)))
        << "C 的 DHT 未就绪";

    std::string diagA;
    const bool cInA = waitFor(
        [&] {
            // drainStats 顺带消费 A 的其余 alert，防 alert 队列膨胀
            return drainStats(a, &diagA).nodes >= 1;
        },
        std::chrono::seconds(60));
    ASSERT_TRUE(cInA) << "C 引导后未进入 A 的路由表（A 主表 nodes 恒 0）"
                      << "；A 侧 alert: " << diagA;

    // B：引导到 A。A 的引导响应 nodes 字段携带 confirmed 的 C →
    // B 的 bootstrap 遍历向 C 发起查询 → C 应答 → reply 路径
    // node_seen → **C 进入 B 的路由表**（C 不是 B 的 router，无
    // 排除）。dht_log 开 B 侧 DHT 模块逐行日志（诊断观测面）
    lt::session b(makeLoopbackSettings(aEp, /*dht_log=*/true));

    std::string dhtLogB;
    const bool cInB = waitFor(
        [&] {
            std::vector<lt::alert*> alerts;
            b.post_dht_stats();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            b.pop_alerts(&alerts);
            for (auto* al : alerts) {
                if (auto* ds = lt::alert_cast<lt::dht_stats_alert>(al)) {
                    for (auto const& bucket : ds->routing_table) {
                        if (bucket.num_nodes > 0) return true;
                    }
                } else if (auto* dl =
                               lt::alert_cast<lt::dht_log_alert>(al)) {
                    dhtLogB += "[log/" +
                               std::to_string(int(dl->module)) + "] " +
                               dl->message() + "\n";
                } else if (auto* dp =
                               lt::alert_cast<lt::dht_pkt_alert>(al)) {
                    dhtLogB +=
                        std::string(dp->direction ==
                                            lt::dht_pkt_alert::incoming
                                        ? "[pkt<-] "
                                        : "[pkt->] ") +
                        dp->message() + "\n";
                }
            }
            return false;
        },
        std::chrono::seconds(60));
    ASSERT_TRUE(cInB) << "两跳传播失败：B 的路由表 60s 内为空"
                      << "；A 主表: "
                      << drainStats(a).nodes
                      << "；B 侧: nodes=" << drainStats(b).nodes
                      << ",repl=" << drainStats(b).replacements
                      << "\n=== B 的 DHT 日志 ===\n" << dhtLogB;

    // 收口：三 session 各自独立，析构顺序无关
}

#else  // !FALCON_USE_LIBTORRENT

TEST(BitTorrentDhtSettings, DisabledWithoutLibtorrent) {
    GTEST_SKIP() << "DHT 会话配置面仅在 FALCON_USE_LIBTORRENT 构建下存在";
}

#endif  // FALCON_USE_LIBTORRENT
