/**
 * @file port_mapper.cpp
 * @brief NAT 端口映射门面实现（miniupnpc + libnatpmp 双后端）
 * @author Falcon Team
 * @date 2026-09-23
 */

#include <falcon/protocols/net/port_mapper.hpp>

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#endif

#ifdef FALCON_ENABLE_NAT_UPNP
// C 库头必须在全局作用域包含（namespace 内包含会把 struct UPNPUrls/
// IGDdatas 定义成 falcon::net 私有类型，与其它 TU 的全局定义构成
// ODR 冲突；前向声明侧同理，见 port_mapper.hpp 注释）
#include <miniupnpc/igd_desc_parse.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnpdev.h>
#include <miniupnpc/upnperrors.h>
#endif

namespace falcon::net {

#ifdef FALCON_ENABLE_NAT_NATPMP
namespace {

using SteadyClock = std::chrono::steady_clock;

int64_t remaining_ms(SteadyClock::time_point deadline) {
    const auto now = SteadyClock::now();
    if (now >= deadline) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                 now)
        .count();
}

}  // namespace
#endif  // FALCON_ENABLE_NAT_NATPMP

// ============================================================================
// PortMapping（RAII 句柄）
// ============================================================================

struct PortMapping::Impl {
    std::shared_ptr<IPortMappingBackend> backend;
    PortMappingRequest req;
    uint16_t external_port = 0;
    std::string external_ip;
    std::string backend_name;
    std::string error;
    /// add 成功才置位——失败句柄只携带 error，析构不得触发 remove
    bool valid_mapping = false;

    ~Impl() {
        if (!backend) return;
        // 尽力删除：网关不可达/映射已被网关回收（NAT-PMP 硬生命周期）
        // 都是常态，静默吞错——进程退出路径上绝不阻塞、绝不抛
        std::string remove_error;
        (void)backend->remove(req, external_port, remove_error);
    }
};

PortMapping::PortMapping() = default;

PortMapping::PortMapping(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PortMapping::PortMapping(PortMapping&& other) noexcept
    : impl_(std::move(other.impl_)) {}

PortMapping& PortMapping::operator=(PortMapping&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

PortMapping::~PortMapping() = default;

bool PortMapping::valid() const {
    return impl_ && impl_->valid_mapping;
}

uint16_t PortMapping::external_port() const {
    return impl_ ? impl_->external_port : 0;
}

const std::string& PortMapping::external_ip() const {
    static const std::string kEmpty;
    return impl_ ? impl_->external_ip : kEmpty;
}

const std::string& PortMapping::backend() const {
    static const std::string kEmpty;
    return impl_ ? impl_->backend_name : kEmpty;
}

const std::string& PortMapping::error() const {
    static const std::string kEmpty;
    return impl_ ? impl_->error : kEmpty;
}

void PortMapping::release() {
    impl_.reset();
}

// ============================================================================
// PortMapper 门面
// ============================================================================

PortMapper::PortMapper() : PortMapper(Config()) {}

PortMapper::PortMapper(Config config) {
    if (config.enable_upnp) {
        if (auto backend = make_upnp_backend(config.discover_timeout_ms)) {
            backends_.push_back(std::move(backend));
        }
    }
    if (config.enable_natpmp) {
        if (auto backend = make_natpmp_backend(0, config.natpmp_response_timeout_ms)) {
            backends_.push_back(std::move(backend));
        }
    }
}

PortMapper::PortMapper(
    std::vector<std::unique_ptr<IPortMappingBackend>> backends) {
    // 测试构造入口的 unique_ptr 序列转内部 shared_ptr 持有
    backends_.reserve(backends.size());
    for (auto& backend : backends) {
        backends_.push_back(std::move(backend));
    }
}

PortMapper::~PortMapper() = default;

PortMapping PortMapper::add(const PortMappingRequest& req) {
    std::string last_error = "no port mapping backend available";
    for (auto& backend : backends_) {
        uint16_t external_port = 0;
        std::string external_ip;
        std::string error;
        if (backend->add(req, external_port, external_ip, error)) {
            auto impl = std::make_unique<PortMapping::Impl>();
            impl->backend = backend;
            impl->req = req;
            impl->external_port = external_port;
            impl->external_ip = std::move(external_ip);
            impl->backend_name = backend->name();
            impl->valid_mapping = true;
            return PortMapping(std::move(impl));
        }
        last_error = backend->name() + ": " + error;
    }
    auto impl = std::make_unique<PortMapping::Impl>();
    impl->error = std::move(last_error);
    return PortMapping(std::move(impl));
}

// ============================================================================
// UPnP IGD 后端（miniupnpc）
// ============================================================================

#ifdef FALCON_ENABLE_NAT_UPNP

// 可测接缝：SSDP 组播发现与 IGD 校验无法在回环构造（组播不经回环
// 接口），而单次 SOAP 调用可经 mock IGD HTTP 服务器全链验证——把
// add/remove 的 SOAP 段拆为 detail 自由函数，测试用 parserootdesc +
// GetUPNPUrls 从 mock desc.xml 装配真实 UPNPUrls/IGDdatas 直调
// （声明见 port_mapper.hpp）。
namespace detail {

bool upnp_add_mapping(const UPNPUrls& urls, const IGDdatas& data,
                      const char* lanaddr, const char* wanaddr,
                      const PortMappingRequest& req, uint16_t want_port,
                      uint16_t& out_external_port,
                      std::string& out_external_ip, std::string& error) {
    const char* proto = req.protocol == PortMappingProtocol::kTcp
                            ? "TCP"
                            : "UDP";
    const int rc = UPNP_AddPortMapping(
        urls.controlURL, data.first.servicetype,
        std::to_string(want_port).c_str(),
        std::to_string(req.internal_port).c_str(), lanaddr,
        req.description.empty() ? "falcon" : req.description.c_str(),
        proto, /*remoteHost=*/nullptr, /*leaseDuration=*/"0");
    if (rc != UPNPCOMMAND_SUCCESS) {
        error = std::string("UPNP_AddPortMapping failed: ") +
                strupnperror(rc);
        return false;
    }

    out_external_port = want_port;
    if (wanaddr[0] != '\0') out_external_ip = wanaddr;
    return true;
}

bool upnp_remove_mapping(const UPNPUrls& urls, const IGDdatas& data,
                         const PortMappingRequest& req,
                         uint16_t external_port, std::string& error) {
    const char* proto = req.protocol == PortMappingProtocol::kTcp
                            ? "TCP"
                            : "UDP";
    const int rc = UPNP_DeletePortMapping(
        urls.controlURL, data.first.servicetype,
        std::to_string(external_port).c_str(), proto, "");
    if (rc != UPNPCOMMAND_SUCCESS) {
        // 714 NoSuchEntryInArray：网关已回收，视为已删除
        if (rc == 714) return true;
        error = std::string("UPNP_DeletePortMapping failed: ") +
                strupnperror(rc);
        return false;
    }
    return true;
}

}  // namespace detail

namespace {

class UpnpBackend : public IPortMappingBackend {
public:
    explicit UpnpBackend(int discover_timeout_ms)
        : discover_timeout_ms_(discover_timeout_ms) {}

    std::string name() const override { return "upnp"; }

    bool add(const PortMappingRequest& req,
             uint16_t& out_external_port,
             std::string& out_external_ip,
             std::string& error) override {
        const uint16_t want_port =
            req.external_port != 0 ? req.external_port : req.internal_port;

        int discover_error = 0;
        // 局域网 SSDP 组播发现；超时有界（构造参数），绝不无限等待
        UPNPDev* devlist =
            upnpDiscover(discover_timeout_ms_, /*multicastif=*/nullptr,
                         /*minissdpdpath=*/nullptr, /*localport=*/0,
                         /*ipv6=*/0, /*ttl=*/2, &discover_error);
        if (devlist == nullptr) {
            error = "SSDP discovery failed (error " +
                    std::to_string(discover_error) +
                    ")；无 UPnP IGD 设备响应";
            return false;
        }
        // RAII：设备列表
        struct DevListGuard {
            UPNPDev* dev;
            ~DevListGuard() { freeUPNPDevlist(dev); }
        } dev_guard{devlist};

        UPNPUrls urls{};
        IGDdatas data{};
        char lanaddr[64] = {0};
        char wanaddr[64] = {0};
        // 1 = IGD 已连接、2 = IGD 未连接（仍可尝试）、3 = 非 IGD 设备
        const int igd =
            UPNP_GetValidIGD(devlist, &urls, &data, lanaddr,
                             static_cast<int>(sizeof(lanaddr)), wanaddr,
                             static_cast<int>(sizeof(wanaddr)));
        if (igd != 1 && igd != 2) {
            error = "no Internet Gateway Device found";
            return false;
        }
        // RAII：controlURL 等堆资源
        struct UrlsGuard {
            UPNPUrls* urls;
            ~UrlsGuard() { FreeUPNPUrls(urls); }
        } urls_guard{&urls};

        return detail::upnp_add_mapping(urls, data, lanaddr, wanaddr, req,
                                        want_port, out_external_port,
                                        out_external_ip, error);
    }

    bool remove(const PortMappingRequest& req,
                uint16_t external_port,
                std::string& error) override {
        // 重新发现 IGD（add 的 IGD 会话不保留——映射生命周期内网关
        // 可能变化，删除时按当前网络状态找）
        int discover_error = 0;
        UPNPDev* devlist =
            upnpDiscover(discover_timeout_ms_, nullptr, nullptr, 0, 0, 2,
                         &discover_error);
        if (devlist == nullptr) {
            error = "SSDP discovery failed during remove";
            return false;
        }
        struct DevListGuard {
            UPNPDev* dev;
            ~DevListGuard() { freeUPNPDevlist(dev); }
        } dev_guard{devlist};

        UPNPUrls urls{};
        IGDdatas data{};
        char lanaddr[64] = {0};
        char wanaddr[64] = {0};
        const int igd =
            UPNP_GetValidIGD(devlist, &urls, &data, lanaddr,
                             static_cast<int>(sizeof(lanaddr)), wanaddr,
                             static_cast<int>(sizeof(wanaddr)));
        if (igd != 1 && igd != 2) {
            error = "no Internet Gateway Device found";
            return false;
        }
        struct UrlsGuard {
            UPNPUrls* urls;
            ~UrlsGuard() { FreeUPNPUrls(urls); }
        } urls_guard{&urls};

        return detail::upnp_remove_mapping(urls, data, req, external_port,
                                           error);
    }

private:
    int discover_timeout_ms_;
};

}  // namespace

std::unique_ptr<IPortMappingBackend> make_upnp_backend(
    int discover_timeout_ms) {
    return std::make_unique<UpnpBackend>(discover_timeout_ms);
}

#else

std::unique_ptr<IPortMappingBackend> make_upnp_backend(int) {
    return nullptr;
}

#endif  // FALCON_ENABLE_NAT_UPNP

// ============================================================================
// NAT-PMP 后端（libnatpmp）
// ============================================================================

#ifdef FALCON_ENABLE_NAT_NATPMP

// natpmp.h 内含 winsock2.h（Windows），必须最先包含
#include <natpmp.h>

namespace {

/// NAT-PMP 是重试型协议：readnatpmpresponseorretry 返回 TRYAGAIN 时
/// 按库的重试节奏（getnatpmprequesttimeout）select 等待 socket 可读。
/// 本辅助把「等待应答」收敛为有界阻塞，超时按失败收口。
bool wait_response(natpmp_t& np, natpmpresp_t& resp, int timeout_ms,
                   std::string& error) {
    const auto deadline = SteadyClock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (true) {
        const int r = readnatpmpresponseorretry(&np, &resp);
        if (r == 0) return true;
        if (r != NATPMP_TRYAGAIN) {
            error = "readnatpmpresponseorretry failed: " + std::to_string(r);
            return false;
        }
        const int64_t left = remaining_ms(deadline);
        if (left <= 0) {
            error = "NAT-PMP response timeout";
            return false;
        }
        struct timeval tv{};
        if (getnatpmprequesttimeout(&np, &tv) == 0) {
            // 库建议的重试时刻与总预算取小
            const int64_t retry_ms =
                static_cast<int64_t>(tv.tv_sec) * 1000 +
                tv.tv_usec / 1000;
            const auto wait = std::chrono::milliseconds(
                retry_ms > 0 && retry_ms < left ? retry_ms : left);
            // tv_usec 的成员类型三平台不一（macOS __darwin_suseconds_t=int，
            // Linux/Windows=long）——按成员类型 cast，避免 brace-init 窄化错
            const int64_t wait_ms = wait.count();
            struct timeval sel_tv{
                static_cast<decltype(sel_tv.tv_sec)>(wait_ms / 1000),
                static_cast<decltype(sel_tv.tv_usec)>(wait_ms % 1000 * 1000)};
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(np.s, &fds);
            (void)::select(np.s + 1, &fds, nullptr, nullptr, &sel_tv);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

class NatPmpBackend : public IPortMappingBackend {
public:
    NatPmpBackend(uint32_t gateway_override, int response_timeout_ms)
        : gateway_override_(gateway_override),
          response_timeout_ms_(response_timeout_ms) {}

    std::string name() const override { return "natpmp"; }

    bool add(const PortMappingRequest& req,
             uint16_t& out_external_port,
             std::string& out_external_ip,
             std::string& error) override {
        natpmp_t np;
        if (initnatpmp(&np, gateway_override_ != 0 ? 1 : 0,
                       static_cast<in_addr_t>(gateway_override_)) != 0) {
            error = "initnatpmp failed（无法确定默认网关或 socket 创建失败）";
            return false;
        }
        struct SessionGuard {
            natpmp_t* np;
            ~SessionGuard() { closenatpmp(np); }
        } session_guard{&np};

        const int proto = req.protocol == PortMappingProtocol::kTcp
                              ? NATPMP_PROTOCOL_TCP
                              : NATPMP_PROTOCOL_UDP;
        const uint16_t want_port =
            req.external_port != 0 ? req.external_port : req.internal_port;
        const uint32_t lifetime =
            req.lifetime_seconds != 0 ? req.lifetime_seconds : 7200;

        if (sendnewportmappingrequest(&np, proto, req.internal_port,
                                      want_port, lifetime) < 0) {
            error = "sendnewportmappingrequest failed";
            return false;
        }
        natpmpresp_t resp{};
        if (!wait_response(np, resp, response_timeout_ms_, error)) {
            return false;
        }
        const uint16_t expect_type =
            req.protocol == PortMappingProtocol::kTcp
                ? NATPMP_RESPTYPE_TCPPORTMAPPING
                : NATPMP_RESPTYPE_UDPPORTMAPPING;
        if (resp.type != expect_type ||
            resp.pnu.newportmapping.privateport != req.internal_port) {
            error = "unexpected NAT-PMP response type/port";
            return false;
        }

        out_external_port = resp.pnu.newportmapping.mappedpublicport;
        // NAT-PMP 协议无独立的外部地址回报（public address 是另一次
        // 请求）；留空（接口契约：尽力而为）
        (void)out_external_ip;
        return true;
    }

    bool remove(const PortMappingRequest& req,
                uint16_t external_port,
                std::string& error) override {
        natpmp_t np;
        if (initnatpmp(&np, gateway_override_ != 0 ? 1 : 0,
                       static_cast<in_addr_t>(gateway_override_)) != 0) {
            error = "initnatpmp failed";
            return false;
        }
        struct SessionGuard {
            natpmp_t* np;
            ~SessionGuard() { closenatpmp(np); }
        } session_guard{&np};

        const int proto = req.protocol == PortMappingProtocol::kTcp
                              ? NATPMP_PROTOCOL_TCP
                              : NATPMP_PROTOCOL_UDP;
        // lifetime=0 即删除映射（NAT-PMP 协议语义）
        if (sendnewportmappingrequest(&np, proto, req.internal_port,
                                      external_port, 0) < 0) {
            error = "sendnewportmappingrequest(lifetime=0) failed";
            return false;
        }
        natpmpresp_t resp{};
        // 删除应答超时按「网关已回收」处理，尽力而为语义
        return wait_response(np, resp, response_timeout_ms_, error);
    }

private:
    uint32_t gateway_override_;
    int response_timeout_ms_;
};

}  // namespace

std::unique_ptr<IPortMappingBackend> make_natpmp_backend(
    uint32_t gateway_override, int response_timeout_ms) {
    return std::make_unique<NatPmpBackend>(gateway_override,
                                           response_timeout_ms);
}

#else

std::unique_ptr<IPortMappingBackend> make_natpmp_backend(uint32_t, int) {
    return nullptr;
}

#endif  // FALCON_ENABLE_NAT_NATPMP

}  // namespace falcon::net
