/**
 * @file port_mapper.hpp
 * @brief NAT 端口映射门面（UPnP IGD + NAT-PMP，成熟开源库后端）
 * @author Falcon Team
 * @date 2026-09-23
 *
 * 设计参考: aria2 的 NAT 穿透姿态（UPnP IGD 优先、NAT-PMP 回退）。
 *
 * 此前仓库自研 NAT 穿透为零实现（BT 私有模式注释里明确
 * "NAT-PMP/UPnP 未实现"）；本模块以 miniupnpc（UPnP IGD）+
 * libnatpmp（NAT-PMP）两个成熟开源库为数据面，自身只做：
 *   - 后端接缝抽象（IPortMappingBackend，测试可注入 fake）
 *   - 按序回退（UPnP → NAT-PMP，第一个成功者胜）
 *   - RAII 映射句柄（析构尽力删除映射，绝不阻塞）
 *
 * 生命周期语义（两后端不对称，文档化）：
 *   - UPnP IGD 映射无自动过期，RAII 析构即删除
 *   - NAT-PMP 映射有硬生命周期（默认 7200s，到期网关自动回收），
 *     续租由上层对同一端口重新 add（幂等更新 lifetime）
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace falcon::net {

/// 映射的传输协议（NAT-PMP 线协议与 UPnP 均区分 TCP/UDP）
enum class PortMappingProtocol {
    kTcp,
    kUdp,
};

/// 一次端口映射请求
struct PortMappingRequest {
    /// 期望的外部端口；0 = 退化为 internal_port（IGD 普遍拒绝分配
    /// 任意端口的请求，aria2 同姿态：以监听端口作期望外部端口）
    uint16_t external_port = 0;
    /// 本机监听端口
    uint16_t internal_port = 0;
    PortMappingProtocol protocol = PortMappingProtocol::kTcp;
    /// 映射描述（IGD 管理界面展示用，可为空）
    std::string description;
    /// NAT-PMP 映射生命周期秒数；0 = 默认 7200（UPnP 忽略此字段）
    uint32_t lifetime_seconds = 0;
};

/**
 * @brief 端口映射后端接缝。
 *
 * 实现方必须满足：
 *   - add/remove 都是同步阻塞调用，且实现方自行保证有界耗时
 *     （发现/应答等待带超时，绝不无限等待）
 *   - 失败时返回 false 并填充 error（人类可读，供日志与上层展示）
 *   - 可安全析构（析构不得抛异常）
 */
class IPortMappingBackend {
public:
    virtual ~IPortMappingBackend() = default;

    /// 后端名（"upnp" / "natpmp"），用于结果标注与日志
    virtual std::string name() const = 0;

    /// 尝试建立映射。成功时填充实际生效的外部端口与外部地址
    /// （外部地址尽力而为，拿不到可为空串）。
    virtual bool add(const PortMappingRequest& req,
                     uint16_t& out_external_port,
                     std::string& out_external_ip,
                     std::string& error) = 0;

    /// 删除此前 add 建立的映射。尽力而为：网关已回收/不可达时返回
    /// false 不构成错误（NAT-PMP 映射本来就有硬生命周期）。
    virtual bool remove(const PortMappingRequest& req,
                        uint16_t external_port,
                        std::string& error) = 0;
};

/**
 * @brief 单个端口映射的 RAII 句柄。
 *
 * valid() 为 true 时析构会尽力删除映射（静默吞错——进程退出路径
 * 上网关不可达是常态，不得阻塞也不得上抛）。move 转移所有权。
 */
class PortMapping {
public:
    /// out-of-line：inline 默认构造会在消费 TU 实例化 incomplete
    /// Impl 的析构（仓库既有 pimpl 陷阱，构造/析构一律离线定义）
    PortMapping();
    PortMapping(PortMapping&& other) noexcept;
    PortMapping& operator=(PortMapping&& other) noexcept;
    PortMapping(const PortMapping&) = delete;
    PortMapping& operator=(const PortMapping&) = delete;
    ~PortMapping();

    /// 映射是否建立成功（out-of-line：失败句柄的 Impl 只携带 error，
    /// 以 Impl 内标志区分而非判 impl_ 非空）
    bool valid() const;

    /// 实际生效的外部端口（valid() 前提下有定义）
    uint16_t external_port() const;

    /// 外部地址（尽力而为，可能为空——网关未报告）
    const std::string& external_ip() const;

    /// 建立该映射的后端名（"upnp" / "natpmp"）
    const std::string& backend() const;

    /// 建立失败时的原因（valid() 为 false 时有定义）
    const std::string& error() const;

    /// 显式提前释放（等价析构，幂等；返回后句柄失效）
    void release();

private:
    friend class PortMapper;
    struct Impl;
    explicit PortMapping(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief 端口映射门面：按序尝试后端，第一个成功者胜。
 *
 * 生产构造默认 UPnP → NAT-PMP 回退序（可用 Config 关掉任一侧）；
 * 测试构造直接注入后端序列。
 */
class PortMapper {
public:
    struct Config {
        bool enable_upnp = true;
        bool enable_natpmp = true;
        /// UPnP 发现（SSDP 组播等待）超时毫秒
        int discover_timeout_ms = 2000;
        /// NAT-PMP 应答等待超时毫秒
        int natpmp_response_timeout_ms = 3000;
    };

    /// 生产构造：全默认配置装配（UPnP → NAT-PMP 回退序）
    PortMapper();

    /// 生产构造：按 config 装配默认后端序列
    explicit PortMapper(Config config);

    /// 测试构造：显式后端序列（依序回退）
    explicit PortMapper(std::vector<std::unique_ptr<IPortMappingBackend>> backends);

    ~PortMapper();
    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    /// 建立映射（同步；失败返回 invalid 句柄，error() 带原因）
    PortMapping add(const PortMappingRequest& req);

private:
    /// 内部统一持 shared_ptr：活跃 PortMapping 句柄与 PortMapper
    /// 共享后端所有权（句柄可能比门面长寿，析构时的 remove 需要
    /// 后端仍存活；对象只构造一次、只析构一次）
    std::vector<std::shared_ptr<IPortMappingBackend>> backends_;
};

// ============================================================================
// 默认后端工厂（#ifdef 门控；未启用对应库时返回 nullptr）
// ============================================================================

/// UPnP IGD 后端（miniupnpc；FALCON_ENABLE_NAT_UPNP 未定义时 nullptr）
std::unique_ptr<IPortMappingBackend> make_upnp_backend(int discover_timeout_ms);

/// NAT-PMP 后端（libnatpmp；FALCON_ENABLE_NAT_NATPMP 未定义时 nullptr）。
/// gateway_override 非零时强制以该地址为网关（回环测试口，生产传 0
/// 走系统默认网关探测）。
std::unique_ptr<IPortMappingBackend> make_natpmp_backend(
    uint32_t gateway_override = 0, int response_timeout_ms = 3000);

}  // namespace falcon::net

#ifdef FALCON_ENABLE_NAT_UPNP
// miniupnpc 类型前向声明——必须放全局作用域：miniupnpc 是 C 库，其
// 头文件在全局作用域定义 struct UPNPUrls / struct IGDdatas；若把前向
// 声明写进 namespace，会制造出独立的 falcon::net::IGDdatas 类型（与
// 全局 ::IGDdatas 不同类型、不同 ABI），消费 TU include 顺序不同即
// 触发二义性/链接错。引用形参只需前向声明，公共头零第三方 include
// 污染（真实定义在 miniupnpc/miniupnpc.h 与 igd_desc_parse.h）。
struct UPNPUrls;
struct IGDdatas;

namespace falcon::net::detail {

/// 可测接缝：add/remove 的 SOAP 调用段（SSDP 组播发现与 IGD 校验留在
/// UpnpBackend 内）。测试经 mock IGD HTTP 服务器 + parserootdesc /
/// GetUPNPUrls 装配真实结构后直调。

/// UPnP IGD SOAP AddPortMapping。成功时填充外部端口（== want_port）与
/// 外部地址（wanaddr 为空串则外部地址保持不动）。
bool upnp_add_mapping(const UPNPUrls& urls, const IGDdatas& data,
                      const char* lanaddr, const char* wanaddr,
                      const PortMappingRequest& req, uint16_t want_port,
                      uint16_t& out_external_port,
                      std::string& out_external_ip, std::string& error);

/// UPnP IGD SOAP DeletePortMapping。rc=714（NoSuchEntryInArray，网关
/// 已回收）视为已删除幂等成功。
bool upnp_remove_mapping(const UPNPUrls& urls, const IGDdatas& data,
                         const PortMappingRequest& req,
                         uint16_t external_port, std::string& error);

}  // namespace falcon::net::detail
#endif  // FALCON_ENABLE_NAT_UPNP
