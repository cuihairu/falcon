/**
 * @file port_mapper_test_part2.cpp
 * @brief NAT 端口映射门面测试（fake 后端单测 + 回环真库数据面）
 * @author Falcon Team
 * @date 2026-09-23
 *
 * 四层覆盖：
 * 1. fake backend 纯单测（无网络）：回退序 / RAII 析构删除 / move
 *    语义 / 全灭错误聚合 / 默认与空配置工厂——门面逻辑的确定性收口
 * 2. NAT-PMP 回环真库（libnatpmp 数据面）：initnatpmp 的 forcegw
 *    测试口强制网关 127.0.0.1，本机 UDP 5351 mock 网关按 NAT-PMP
 *    线格式应答——全链不触网，真实收发真实协议编解码
 * 3. UPnP 无 IGD 环境的优雅失败（miniupnpc 数据面）：host 若真有
 *    IGD 则 skip（无法构造无 IGD 场景），否则断言有界耗时内干净
 *    失败且 error 可读
 * 4. UPnP SOAP 段 mock IGD 直调（detail 接缝）：回环 TCP mock IGD
 *    应答描述文档与 SOAP envelope/Fault，parserootdesc + GetUPNPUrls
 *    装配真实结构后直调 detail::upnp_add_mapping / upnp_remove_
 *    mapping——miniupnpc 真实编解码全链验证（SSDP 组播发现不经
 *    回环接口，留在 UpnpBackend 内不入本层）
 *
 * 对应后端库未启用的构建下，各真库用例编译为 skip 占位（对齐
 * bittorrent_dht_test 的两模式姿态）。
 */

#include <csignal>
#include <gtest/gtest.h>

#include <falcon/protocols/net/port_mapper.hpp>

#if defined(FALCON_ENABLE_NAT_UPNP)
// 全局作用域包含（匿名 namespace 内包含会把这些 C 函数声明卷进
// 匿名 namespace 触发 static-未定义警告）
#include <miniupnpc/igd_desc_parse.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpdev.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
// Winsock（winsock2.h）无 socklen_t/ssize_t：长度参数与 recv 返回值均为 int
using sock_len = int;
using recv_ssize = int;
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_len = socklen_t;
using recv_ssize = ssize_t;
#endif

using namespace falcon;
using namespace falcon::net;

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

// 共享 socket 辅助（NAT-PMP mock 网关与 UPnP mock IGD 服务器共用；
// inline——后端全禁用的构建下保持零未用告警）

TEST(UpnpLoopback, DisabledWithoutUpnp) {
    GTEST_SKIP() << "UPnP 后端仅在 FALCON_ENABLE_NAT_UPNP 构建下存在";
}

#endif  // FALCON_ENABLE_NAT_UPNP

}  // namespace
