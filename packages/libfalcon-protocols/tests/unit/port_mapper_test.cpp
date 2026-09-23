/**
 * @file port_mapper_test.cpp
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

// 共享 socket 辅助（NAT-PMP mock 网关与 UPnP mock IGD 服务器共用；
// inline——后端全禁用的构建下保持零未用告警）
inline void closeSocket(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

#if defined(_WIN32)
inline void ensure_winsock() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}
#else
inline void ensure_winsock() {}
#endif

//==============================================================================
// fake backend
//==============================================================================

struct BackendStats {
    std::atomic<int> add_calls{0};
    std::atomic<int> remove_calls{0};
};

/// 可编程 fake：add 成败、返回的外部端口、记录收到的请求
class FakeBackend : public IPortMappingBackend {
public:
    FakeBackend(std::string name, bool add_result, uint16_t external_port = 0)
        : name_(std::move(name)),
          add_result_(add_result),
          external_port_(external_port) {}

    std::string name() const override { return name_; }

    bool add(const PortMappingRequest& req,
             uint16_t& out_external_port,
             std::string& out_external_ip,
             std::string& error) override {
        ++stats.add_calls;
        last_add_req_ = req;
        if (!add_result_) {
            error = name_ + " unavailable (fake)";
            return false;
        }
        out_external_port = external_port_;
        out_external_ip = "203.0.113.7";
        return true;
    }

    bool remove(const PortMappingRequest&,
                uint16_t external_port,
                std::string&) override {
        ++stats.remove_calls;
        last_remove_port_ = external_port;
        return true;
    }

    BackendStats stats;
    PortMappingRequest last_add_req_;
    uint16_t last_remove_port_ = 0;

private:
    std::string name_;
    bool add_result_;
    uint16_t external_port_;
};

/// 单后端门面便捷构造（测试可读性）
PortMapper makeMapper(std::unique_ptr<IPortMappingBackend> backend) {
    std::vector<std::unique_ptr<IPortMappingBackend>> backends;
    backends.push_back(std::move(backend));
    return PortMapper(std::move(backends));
}

PortMappingRequest makeReq(uint16_t port) {
    PortMappingRequest req;
    req.internal_port = port;
    req.protocol = PortMappingProtocol::kTcp;
    req.description = "falcon-test";
    return req;
}

//==============================================================================
// 门面：回退序与 RAII 生命周期
//==============================================================================

TEST(PortMapperFacade, FirstBackendWinsAndCarriesResult) {
    auto first = std::make_unique<FakeBackend>("first", /*ok=*/true, 40001);
    auto* first_p = first.get();
    auto mapper = makeMapper(std::move(first));

    auto mapping = mapper.add(makeReq(12345));
    ASSERT_TRUE(mapping.valid());
    EXPECT_EQ(mapping.external_port(), 40001);
    EXPECT_EQ(mapping.external_ip(), "203.0.113.7");
    EXPECT_EQ(mapping.backend(), "first");
    EXPECT_TRUE(mapping.error().empty());
    // 请求原样到达后端
    EXPECT_EQ(first_p->last_add_req_.internal_port, 12345);
    EXPECT_EQ(first_p->stats.add_calls, 1);
    EXPECT_EQ(first_p->stats.remove_calls, 0);  // 未析构前不删除
}

TEST(PortMapperFacade, ReleaseRemovesMappingExactlyOnce) {
    auto first = std::make_unique<FakeBackend>("only", true, 40002);
    auto* first_p = first.get();
    auto mapper = makeMapper(std::move(first));

    {
        auto mapping = mapper.add(makeReq(12346));
        ASSERT_TRUE(mapping.valid());
        mapping.release();  // 显式提前释放
        EXPECT_EQ(first_p->stats.remove_calls, 1);
        EXPECT_FALSE(mapping.valid());
        // release 幂等
        mapping.release();
        EXPECT_EQ(first_p->stats.remove_calls, 1);
    }
    // release 后作用域析构不再二次删除
    EXPECT_EQ(first_p->stats.remove_calls, 1);
    EXPECT_EQ(first_p->last_remove_port_, 40002);
}

TEST(PortMapperFacade, DestructorRemovesMapping) {
    auto first = std::make_unique<FakeBackend>("only", true, 40003);
    auto* first_p = first.get();
    auto mapper = makeMapper(std::move(first));

    {
        auto mapping = mapper.add(makeReq(12347));
        ASSERT_TRUE(mapping.valid());
        EXPECT_EQ(first_p->stats.remove_calls, 0);
    }
    EXPECT_EQ(first_p->stats.remove_calls, 1);
}

TEST(PortMapperFacade, MoveTransfersRemoveResponsibility) {
    auto first = std::make_unique<FakeBackend>("only", true, 40004);
    auto* first_p = first.get();
    auto mapper = makeMapper(std::move(first));

    PortMapping target;
    {
        auto source = mapper.add(makeReq(12348));
        ASSERT_TRUE(source.valid());
        target = std::move(source);
        // move 后源为空：源析构不触发删除
        EXPECT_EQ(first_p->stats.remove_calls, 0);
    }
    EXPECT_EQ(first_p->stats.remove_calls, 0);  // move 语义的关键断言
    EXPECT_TRUE(target.valid());
    EXPECT_EQ(target.external_port(), 40004);

    target.release();
    EXPECT_EQ(first_p->stats.remove_calls, 1);
}

TEST(PortMapperFacade, FallsBackToNextBackendOnFailure) {
    auto first = std::make_unique<FakeBackend>("upnp", /*ok=*/false);
    auto second = std::make_unique<FakeBackend>("natpmp", /*ok=*/true, 40005);
    auto* first_p = first.get();
    auto* second_p = second.get();
    // 测试构造接口：显式后端序列（依序回退）
    std::vector<std::unique_ptr<IPortMappingBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    PortMapper mapper(std::move(backends));

    auto mapping = mapper.add(makeReq(12349));
    ASSERT_TRUE(mapping.valid());
    EXPECT_EQ(mapping.backend(), "natpmp");
    EXPECT_EQ(mapping.external_port(), 40005);
    EXPECT_EQ(first_p->stats.add_calls, 1);
    EXPECT_EQ(first_p->stats.remove_calls, 0);  // 失败者无映射可删
    EXPECT_EQ(second_p->stats.add_calls, 1);
    // 失败的后端不承担删除责任：析构只向成功者发 remove
}

TEST(PortMapperFacade, AllBackendsFailYieldsInvalidMappingWithErrors) {
    std::vector<std::unique_ptr<IPortMappingBackend>> backends;
    backends.push_back(std::make_unique<FakeBackend>("upnp", false));
    backends.push_back(std::make_unique<FakeBackend>("natpmp", false));
    PortMapper mapper(std::move(backends));

    auto mapping = mapper.add(makeReq(12350));
    EXPECT_FALSE(mapping.valid());
    EXPECT_EQ(mapping.external_port(), 0);
    // 错误聚合含最后失败者（可读性契约）
    EXPECT_NE(mapping.error().find("natpmp"), std::string::npos);
    EXPECT_NE(mapping.error().find("unavailable"), std::string::npos);
}

TEST(PortMapperFacade, ZeroExternalPortFallsBackToInternalPort) {
    // external_port=0 的请求约定：退化为 internal_port（IGD 普遍拒绝
    // 任意分配请求，aria2 同姿态）。门面不改写请求——改写在后端内，
    // fake 忠实记录请求验证门面透传
    auto first = std::make_unique<FakeBackend>("only", true, 7777);
    auto* first_p = first.get();
    auto mapper = makeMapper(std::move(first));

    PortMappingRequest req = makeReq(7777);
    req.external_port = 0;
    auto mapping = mapper.add(req);
    ASSERT_TRUE(mapping.valid());
    EXPECT_EQ(first_p->last_add_req_.external_port, 0);
    EXPECT_EQ(first_p->last_add_req_.internal_port, 7777);
}

TEST(PortMapperFacade, MoveConstructorTransfersOwnership) {
    // move 构造（既有用例覆盖的是 move 赋值）同样转移删除责任：
    // 源句柄清空后析构零副作用，仅 target 侧删除一次
    auto first = std::make_unique<FakeBackend>("only", true, 40006);
    auto* first_p = first.get();
    auto mapper = makeMapper(std::move(first));

    auto source = mapper.add(makeReq(12351));
    ASSERT_TRUE(source.valid());
    PortMapping target(std::move(source));
    EXPECT_FALSE(source.valid());  // 源已空（valid 为 false 且无删除责任）
    EXPECT_TRUE(target.valid());
    EXPECT_EQ(target.external_port(), 40006);
    target.release();
    EXPECT_EQ(first_p->stats.remove_calls, 1);
}

TEST(PortMapperFacade, DefaultConfigAssemblesBackendsAndEmptyConfigFailsCleanly) {
    // 生产默认构造：装配真实后端序列（构造零网络——网络动作只发生在
    // add 时；本用例不调真实后端的 add，仅验证装配路径）
    { PortMapper mapper; }

    // 显式关闭两后端：空序列 add 得 invalid 句柄 + 门面级默认错误
    PortMapper::Config config;
    config.enable_upnp = false;
    config.enable_natpmp = false;
    PortMapper mapper(config);
    auto mapping = mapper.add(makeReq(12352));
    EXPECT_FALSE(mapping.valid());
    EXPECT_EQ(mapping.external_port(), 0);
    EXPECT_EQ(mapping.error(), "no port mapping backend available");
}

#if defined(FALCON_ENABLE_NAT_NATPMP)

//==============================================================================
// NAT-PMP 回环真库（libnatpmp 数据面 + mock 网关）
//==============================================================================

uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void write_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

/// 本机 UDP 5351 mock NAT-PMP 网关：按线格式应答 public address 与
/// port mapping 两类请求。ver=0 + opcode|0x80 + resultcode=0 大端。
class MockNatPmpGateway {
public:
    /// 静默模式：吞掉请求不回包（驱动客户端的应答超时路径）
    void set_silent(bool on) { silent_.store(on); }

    bool start() {
        ensure_winsock();
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(5351);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
            0) {
            closeSocket(fd_);
            fd_ = -1;
            return false;
        }
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    ~MockNatPmpGateway() {
        if (fd_ >= 0) {
            running_ = false;
            // 发一个哑包唤醒 recvfrom
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(5351);
            const uint8_t poke = 0;
            (void)::sendto(fd_, reinterpret_cast<const char*>(&poke), 1, 0,
                           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            ::shutdown(fd_, SHUT_RDWR);
            closeSocket(fd_);
            if (thread_.joinable()) thread_.join();
        }
    }

    std::atomic<int> mapping_requests{0};

private:
    void loop() {
        uint8_t buf[64];
        while (running_.load()) {
            sockaddr_in src{};
            sock_len srclen = sizeof(src);
            const recv_ssize n =
                ::recvfrom(fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&src), &srclen);
            if (n < 2) continue;
            if (n == 1) break;  // 停机哑包

            const uint8_t opcode = buf[1];
            uint8_t resp[16] = {0};
            size_t resp_len = 0;
            if (opcode == 0) {
                // public address 应答：ver + opcode|128 + rc + epoch + ip
                resp_len = 12;
                resp[1] = 0x80;
                const uint32_t loopback = htonl(INADDR_LOOPBACK);
                std::memcpy(resp + 8, &loopback, 4);
            } else if (opcode == 1 || opcode == 2) {
                // port mapping 请求：ver + opcode + reserved(2) +
                // private(2) + public(2) + lifetime(4)
                ++mapping_requests;
                const uint16_t priv = read_be16(buf + 4);
                const uint16_t pub = read_be16(buf + 6);
                const uint32_t lifetime = read_be32(buf + 8);
                resp_len = 16;
                resp[1] = static_cast<uint8_t>(opcode | 0x80);
                write_be16(resp + 8, priv);
                write_be16(resp + 10, lifetime != 0 ? pub : 0);
                write_be32(resp + 12, lifetime);
            } else {
                continue;
            }
#ifdef _WIN32
            if (!silent_.load()) {
                (void)::sendto(fd_, reinterpret_cast<const char*>(resp),
                               static_cast<int>(resp_len), 0,
                               reinterpret_cast<const sockaddr*>(&src),
                               srclen);
            }
#else
            if (!silent_.load()) {
                (void)::sendto(fd_, reinterpret_cast<const char*>(resp),
                               resp_len, 0,
                               reinterpret_cast<const sockaddr*>(&src),
                               srclen);
            }
#endif
        }
    }

    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> silent_{false};
    std::thread thread_;
};

TEST(NatPmpLoopback, AddMappingViaMockGateway) {
    MockNatPmpGateway gateway;
    // 5351 被占（其他 NAT-PMP 客户端/守护进程）：本机环境限制，skip
    if (!gateway.start()) {
        GTEST_SKIP() << "UDP 5351 被占用，无法起 mock NAT-PMP 网关";
    }

    auto backend = make_natpmp_backend(htonl(INADDR_LOOPBACK),
                                       /*response_timeout_ms=*/3000);
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->name(), "natpmp");

    PortMappingRequest req = makeReq(51413);
    req.external_port = 0;  // 退化为 internal_port
    uint16_t external_port = 0;
    std::string external_ip;
    std::string error;
    ASSERT_TRUE(backend->add(req, external_port, external_ip, error))
        << "add failed: " << error;
    // libnatpmp 把网关应答的 mappedpublicport 回传
    EXPECT_EQ(external_port, 51413);
    EXPECT_EQ(gateway.mapping_requests.load(), 1);
    // NAT-PMP 协议无外部地址回报（接口契约：留空）
    EXPECT_TRUE(external_ip.empty());

    // 删除（lifetime=0）同样走真实线协议
    ASSERT_TRUE(backend->remove(req, external_port, error))
        << "remove failed: " << error;
    EXPECT_EQ(gateway.mapping_requests.load(), 2);
}

TEST(NatPmpLoopback, UdpProtocolRoundTrip) {
    MockNatPmpGateway gateway;
    if (!gateway.start()) {
        GTEST_SKIP() << "UDP 5351 被占用，无法起 mock NAT-PMP 网关";
    }

    auto backend = make_natpmp_backend(htonl(INADDR_LOOPBACK), 3000);
    ASSERT_NE(backend, nullptr);

    PortMappingRequest req;
    req.internal_port = 6881;
    req.protocol = PortMappingProtocol::kUdp;
    uint16_t external_port = 0;
    std::string external_ip;
    std::string error;
    ASSERT_TRUE(backend->add(req, external_port, external_ip, error))
        << "add failed: " << error;
    EXPECT_EQ(external_port, 6881);
}

TEST(NatPmpLoopback, AddTimesOutOnSilentGateway) {
    MockNatPmpGateway gateway;
    if (!gateway.start()) {
        GTEST_SKIP() << "UDP 5351 被占用，无法起 mock NAT-PMP 网关";
    }
    gateway.set_silent(true);  // 吞请求不回包

    // 短应答预算：超时路径按失败收口，绝不无限等待
    auto backend = make_natpmp_backend(htonl(INADDR_LOOPBACK), /*timeout=*/400);
    ASSERT_NE(backend, nullptr);

    PortMappingRequest req = makeReq(51420);
    uint16_t external_port = 0;
    std::string external_ip;
    std::string error;
    EXPECT_FALSE(backend->add(req, external_port, external_ip, error));
    EXPECT_NE(error.find("timeout"), std::string::npos);
    EXPECT_EQ(external_port, 0);
}

#else

TEST(NatPmpLoopback, DisabledWithoutNatPmp) {
    GTEST_SKIP() << "NAT-PMP 后端仅在 FALCON_ENABLE_NAT_NATPMP 构建下存在";
}

#endif  // FALCON_ENABLE_NAT_NATPMP

#if defined(FALCON_ENABLE_NAT_UPNP)

//==============================================================================
// UPnP 无 IGD 优雅失败（miniupnpc 数据面）
//==============================================================================

/// host 若真有 IGD（家庭路由器场景），无法在测试内构造「无 IGD」
/// 环境——skip 门禁。探测本身短超时有界。
bool hostHasRealIgd() {
    int err = 0;
    UPNPDev* devlist = upnpDiscover(/*timeout=*/500, nullptr, nullptr, 0, 0,
                                    2, &err);
    if (devlist == nullptr) return false;
    UPNPUrls urls{};
    IGDdatas data{};
    char lanaddr[64] = {0};
    char wanaddr[64] = {0};
    const int igd = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr,
                                     static_cast<int>(sizeof(lanaddr)),
                                     wanaddr,
                                     static_cast<int>(sizeof(wanaddr)));
    freeUPNPDevlist(devlist);
    // GetValidIGD 返回非 0 时 urls 已被内部 GetUPNPUrls 填充，必须释放
    // （值初始化全零 + FreeUPNPUrls 对 NULL 字段安全——不释放即 LSan
    //   报 5 处 URL 字段泄漏；生产侧由 UrlsGuard 承担同职责）
    if (igd != 0) FreeUPNPUrls(&urls);
    return igd == 1 || igd == 2;
}

TEST(UpnpLoopback, NoIgdFailsCleanlyWithinBudget) {
    if (hostHasRealIgd()) {
        GTEST_SKIP() << "host 存在真实 UPnP IGD，无法构造无 IGD 场景";
    }

    auto backend = make_upnp_backend(/*discover_timeout_ms=*/800);
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->name(), "upnp");

    const auto start = std::chrono::steady_clock::now();
    PortMappingRequest req = makeReq(51414);
    uint16_t external_port = 0;
    std::string external_ip;
    std::string error;
    EXPECT_FALSE(backend->add(req, external_port, external_ip, error));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // 错误可读 + 有界耗时（发现超时 800ms + 余量，绝不无限等待）
    EXPECT_FALSE(error.empty());
    EXPECT_LE(std::chrono::duration_cast<std::chrono::seconds>(elapsed)
                  .count(),
              10);
    EXPECT_EQ(external_port, 0);
}

//==============================================================================
// UPnP SOAP 段 mock IGD 直调（detail 接缝 + miniupnpc 真实编解码）
//==============================================================================

/// mock IGD 描述文档（标准三层嵌套；controlURL=/ctl/IPConn；无 URLBase
/// ——GetUPNPUrls 从 descURL 推导 base，随机端口无需烤进 XML）
constexpr char kDescXml[] = R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <friendlyName>MockIGD</friendlyName>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:Layer3Forwarding</serviceId>
        <controlURL>/ctl/L3F</controlURL>
        <eventSubURL>/evt/L3F</eventSubURL>
        <SCPDURL>/L3F.xml</SCPDURL>
      </service>
    </serviceList>
    <deviceList>
      <device>
        <deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
        <friendlyName>WANDevice</friendlyName>
        <serviceList>
          <service>
            <serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
            <serviceId>urn:upnp-org:serviceId:WANCommonInterfaceConfig</serviceId>
            <controlURL>/ctl/CIF</controlURL>
            <eventSubURL>/evt/CIF</eventSubURL>
            <SCPDURL>/CIF.xml</SCPDURL>
          </service>
        </serviceList>
        <deviceList>
          <device>
            <deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
            <friendlyName>WANConnectionDevice</friendlyName>
            <serviceList>
              <service>
                <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
                <serviceId>urn:upnp-org:serviceId:WANIPConnection</serviceId>
                <controlURL>/ctl/IPConn</controlURL>
                <eventSubURL>/evt/IPConn</eventSubURL>
                <SCPDURL>/IPConn.xml</SCPDURL>
              </service>
            </serviceList>
          </device>
        </deviceList>
      </device>
    </deviceList>
  </device>
</root>
)";

std::string soap_envelope(const char* response_element) {
    return std::string(
               "<?xml version=\"1.0\"?>"
               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/"
               "envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/"
               "soap/encoding/\"><s:Body><u:") +
           response_element +
           " xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"/>"
           "</s:Body></s:Envelope>";
}

std::string soap_fault(int code, const char* text) {
    return std::string(
               "<?xml version=\"1.0\"?>"
               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/"
               "envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/"
               "soap/encoding/\"><s:Body><s:Fault><detail><UPnPError "
               "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>") +
           std::to_string(code) +
           "</errorCode><errorDescription>" + text +
           "</errorDescription></UPnPError></detail></s:Fault></s:Body>"
           "</s:Envelope>";
}

/// 回环 TCP mock IGD：GET /desc.xml → 描述文档；POST /ctl/IPConn → 按
/// 配置应答 SOAP envelope / Fault。一连接一请求（miniupnpc 客户端兼容
/// Connection: close 语义）。miniupnpc 对 SOAP Fault 的判定基于 body
/// 内 errorCode 而非 HTTP 状态码（探针实证），Fault 应答用 HTTP 500
/// 忠实于真实网关。
class MockIgdServer {
public:
    bool start() {
        ensure_winsock();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
                0 ||
            ::listen(fd_, 4) < 0) {
            closeSocket(fd_);
            fd_ = -1;
            return false;
        }
        sock_len len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) !=
            0) {
            ::shutdown(fd_, SHUT_RDWR);
            closeSocket(fd_);
            fd_ = -1;
            return false;
        }
        port_ = ntohs(addr.sin_port);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    ~MockIgdServer() {
        if (fd_ >= 0) {
            running_ = false;
            // 先 shutdown 再 close（close 不唤醒阻塞在 accept 的线程）
            ::shutdown(fd_, SHUT_RDWR);
            closeSocket(fd_);
            if (thread_.joinable()) thread_.join();
        }
    }

    std::string desc_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/desc.xml";
    }

    /// add 的 SOAP 调用改为应答 Fault 718（ConflictInMappingEntry）
    void set_add_fault(bool on) { add_fault_.store(on); }
    /// remove 的 SOAP 调用应答成功 envelope（默认回 Fault 714）
    void set_remove_success(bool on) { remove_success_.store(on); }
    /// remove 的默认 Fault 714 换成其他错误码（默认 714 幂等成功语义）
    void set_remove_fault_code(int code) { remove_fault_code_.store(code); }

    /// 已收到的全部请求原文（GET 描述文档 + POST SOAP），测试线程读
    std::string snapshot_requests() {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }

private:
    void loop() {
        while (running_.load()) {
            sockaddr_in peer{};
            sock_len plen = sizeof(peer);
            const int conn = ::accept(fd_, reinterpret_cast<sockaddr*>(&peer),
                                      &plen);
            if (conn < 0) break;  // 停机 shutdown 唤醒
            handle(conn);
            closeSocket(conn);
        }
    }

    void handle(int conn) {
        std::string data;
        char buf[2048];
        while (true) {
            const recv_ssize n =
                ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) break;
            data.append(buf, static_cast<size_t>(n));
            if (request_complete(data)) break;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            requests_ += data;
        }

        if (data.rfind("GET ", 0) == 0) {
            send_all(conn, http_response(200, kDescXml));
        } else if (data.rfind("POST ", 0) == 0) {
            if (data.find("AddPortMapping") != std::string::npos) {
                send_all(conn, http_response(add_fault_.load() ? 500 : 200,
                                             add_fault_.load()
                                                 ? soap_fault(718, "ConflictInMappingEntry")
                                                 : soap_envelope("AddPortMappingResponse")));
            } else {  // DeletePortMapping
                if (remove_success_.load()) {
                    send_all(conn, http_response(
                                       200,
                                       soap_envelope("DeletePortMappingResponse")));
                } else {
                    const int code = remove_fault_code_.load();
                    send_all(conn, http_response(
                                       500, soap_fault(code, "ActionFailed")));
                }
            }
        }
    }

    /// 请求是否收完整（head 终结 + Content-Length 满足；无 Content-Length
    /// 视为收完——GET 请求）
    static bool request_complete(const std::string& data) {
        const size_t head_end = data.find("\r\n\r\n");
        if (head_end == std::string::npos) return false;
        std::string lower;
        lower.reserve(head_end);
        for (size_t i = 0; i < head_end; ++i) {
            lower.push_back(static_cast<char>(
                ::tolower(static_cast<unsigned char>(data[i]))));
        }
        const size_t key = lower.find("content-length:");
        if (key == std::string::npos) return true;
        const size_t value_start = data.find_first_not_of(
            " \t", key + sizeof("content-length:") - 1);
        if (value_start == std::string::npos ||
            value_start >= head_end) {
            return true;
        }
        const int length = std::atoi(data.c_str() + value_start);
        return static_cast<size_t>(length) <= data.size() - (head_end + 4);
    }

    static std::string http_response(int code, const std::string& body) {
        return "HTTP/1.1 " + std::to_string(code) +
               (code == 200 ? " OK" : " Internal Server Error") +
               "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
               "Connection: close\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    static void send_all(int conn, const std::string& payload) {
        size_t sent = 0;
        while (sent < payload.size()) {
#ifdef _WIN32
            const int n = ::send(conn, payload.data() + sent,
                                 static_cast<int>(payload.size() - sent), 0);
#else
            const recv_ssize n =
                ::send(conn, payload.data() + sent, payload.size() - sent, 0);
#endif
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    int fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<bool> add_fault_{false};
    std::atomic<bool> remove_success_{false};
    std::atomic<int> remove_fault_code_{714};
    std::mutex mu_;
    std::string requests_;
};

/// parserootdesc + GetUPNPUrls 从 mock 描述文档装配真实结构。GetUPNPUrls
/// 在 urls 内分配堆串（controlURL 等），RAII 析构 FreeUPNPUrls 回收
/// （miniupnpc 对全零结构安全——各字段逐一判空）。出参引用形态：带
/// 析构的类型不做 by-value 返回（规避移动后临时对象析构误释放）。
struct IgdAssembly {
    UPNPUrls urls{};
    IGDdatas data{};
    ~IgdAssembly() { FreeUPNPUrls(&urls); }
};

void assemble_igd(const std::string& desc_url, IgdAssembly& igd) {
    std::memset(&igd.data, 0, sizeof(igd.data));
    parserootdesc(kDescXml, static_cast<int>(sizeof(kDescXml) - 1), &igd.data);
    std::memset(&igd.urls, 0, sizeof(igd.urls));
    GetUPNPUrls(&igd.urls, &igd.data, desc_url.c_str(), 0);
}

TEST(UpnpDetailSoap, AddPortMappingRoundTripCarriesRequestFields) {
    MockIgdServer server;
    ASSERT_TRUE(server.start()) << "mock IGD 服务器启动失败";

    IgdAssembly igd;
    assemble_igd(server.desc_url(), igd);
    ASSERT_NE(igd.urls.controlURL, nullptr);
    ASSERT_STRNE(igd.urls.controlURL, "");

    PortMappingRequest req = makeReq(51415);
    req.external_port = 40000;  // 期望外部端口 ≠ 内部端口——两字段独立钉住
    req.description = "falcon-test";
    uint16_t out_port = 0;
    std::string out_ip;
    std::string error;
    ASSERT_TRUE(detail::upnp_add_mapping(
        igd.urls, igd.data, /*lanaddr=*/"192.0.2.9", /*wanaddr=*/"203.0.113.7",
        req, /*want_port=*/40000, out_port, out_ip, error))
        << "add failed: " << error;
    EXPECT_EQ(out_port, 40000);        // 外部端口 == want_port
    EXPECT_EQ(out_ip, "203.0.113.7");  // 外部地址 == wanaddr

    const std::string request = server.snapshot_requests();
    // SOAP 调用经 desc 推导的 controlURL（base + /ctl/IPConn）
    EXPECT_NE(request.find("POST /ctl/IPConn"), std::string::npos);
    // servicetype 原文进入请求（SOAPAction 头 / body SOAPAction 元素）
    EXPECT_NE(request.find("AddPortMapping"), std::string::npos);
    EXPECT_NE(request.find("WANIPConnection:1"), std::string::npos);
    // UPnP 标准 tag：两端口独立、协议、内部地址、描述
    EXPECT_NE(request.find("<NewExternalPort>40000</NewExternalPort>"),
              std::string::npos);
    EXPECT_NE(request.find("<NewInternalPort>51415</NewInternalPort>"),
              std::string::npos);
    EXPECT_NE(request.find("<NewInternalClient>192.0.2.9</NewInternalClient>"),
              std::string::npos);
    EXPECT_NE(request.find("<NewProtocol>TCP</NewProtocol>"),
              std::string::npos);
    EXPECT_NE(
        request.find(
            "<NewPortMappingDescription>falcon-test</NewPortMappingDescription>"),
        std::string::npos);
}

TEST(UpnpDetailSoap, AddPortMappingFaultSurfacesConflictError) {
    MockIgdServer server;
    ASSERT_TRUE(server.start());
    server.set_add_fault(true);  // HTTP 500 + SOAP Fault 718

    IgdAssembly igd;
    assemble_igd(server.desc_url(), igd);
    ASSERT_NE(igd.urls.controlURL, nullptr);

    PortMappingRequest req = makeReq(51416);
    uint16_t out_port = 0;
    std::string out_ip;
    std::string error;
    EXPECT_FALSE(detail::upnp_add_mapping(igd.urls, igd.data, "192.0.2.9",
                                          /*wanaddr=*/"", req, 51416, out_port,
                                          out_ip, error));
    // miniupnpc 以 body 内 errorCode 判定失败（非 HTTP 状态码），
    // 错误文本经 strupnperror 可读
    EXPECT_NE(error.find("ConflictInMappingEntry"), std::string::npos);
    EXPECT_EQ(out_port, 0);
}

TEST(UpnpDetailSoap, RemovePortMappingTreatsNoSuchEntryAsSuccess) {
    MockIgdServer server;
    ASSERT_TRUE(server.start());  // 默认回 Fault 714 NoSuchEntryInArray

    IgdAssembly igd;
    assemble_igd(server.desc_url(), igd);
    ASSERT_NE(igd.urls.controlURL, nullptr);

    PortMappingRequest req = makeReq(51417);
    std::string error;
    // 网关已回收映射 = 幂等成功（RAII 析构路径不误报）
    EXPECT_TRUE(
        detail::upnp_remove_mapping(igd.urls, igd.data, req, 40001, error));
    EXPECT_TRUE(error.empty());

    const std::string request = server.snapshot_requests();
    EXPECT_NE(request.find("POST /ctl/IPConn"), std::string::npos);
    EXPECT_NE(request.find("DeletePortMapping"), std::string::npos);
    EXPECT_NE(request.find("<NewExternalPort>40001</NewExternalPort>"),
              std::string::npos);
}

TEST(UpnpDetailSoap, RemovePortMappingSuccess) {
    MockIgdServer server;
    ASSERT_TRUE(server.start());
    server.set_remove_success(true);  // 200 + DeletePortMappingResponse

    IgdAssembly igd;
    assemble_igd(server.desc_url(), igd);
    ASSERT_NE(igd.urls.controlURL, nullptr);

    PortMappingRequest req = makeReq(51418);
    std::string error;
    EXPECT_TRUE(
        detail::upnp_remove_mapping(igd.urls, igd.data, req, 40002, error));
    EXPECT_TRUE(error.empty());
}

TEST(UpnpDetailSoap, RemovePortMappingFaultSurfacesError) {
    MockIgdServer server;
    ASSERT_TRUE(server.start());
    // 非 714 的 Fault（501 ActionFailed）：仅 714 走幂等成功，其余上抛
    server.set_remove_fault_code(501);

    IgdAssembly igd;
    assemble_igd(server.desc_url(), igd);
    ASSERT_NE(igd.urls.controlURL, nullptr);

    PortMappingRequest req = makeReq(51419);
    std::string error;
    EXPECT_FALSE(
        detail::upnp_remove_mapping(igd.urls, igd.data, req, 40003, error));
    // 生产错误前缀确定性拼接；具体错误文本经 strupnperror 可读
    EXPECT_NE(error.find("UPNP_DeletePortMapping failed"),
              std::string::npos);
}

#else

TEST(UpnpLoopback, DisabledWithoutUpnp) {
    GTEST_SKIP() << "UPnP 后端仅在 FALCON_ENABLE_NAT_UPNP 构建下存在";
}

#endif  // FALCON_ENABLE_NAT_UPNP

}  // namespace
