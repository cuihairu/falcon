/**
 * @file plugin_loader.hpp
 * @brief Drives 插件动态加载器（进程级单例）
 *
 * 唯一插件边界是 falcon/abi/drive_api.h 的稳定 C ABI。
 *
 * 插件发现顺序（可叠加多个）：
 *  1. 环境变量 FALCON_PLUGIN_DIR（分号/冒号分隔，显式覆盖）
 *  2. 可执行文件同级的 falcon/plugins 目录（绿色便携分发）
 *  3. 编译期 FALCON_PLUGIN_DEFAULT_DIR（系统安装分发）
 *
 * 加载失败不抛异常，只记录到 errors()，门面层据此给出明确诊断。
 */
#pragma once

#include <falcon/abi/drive_api.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace falcon {
namespace drives {

class PluginLoader {
public:
    /// 进程级单例；首次访问任一服务表时完成扫描。
    static PluginLoader& instance();

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    /// 稳定 C ABI 服务表；插件缺失或未加载时返回 nullptr。
    const falcon_cloud_service* cloud_storage() const;
    const falcon_search_service* resource_search() const;
    const falcon_config_service* config_storage() const;

    /// 已加载插件的全部服务描述符。
    std::vector<const falcon_plugin_descriptor*> descriptors() const;

    /// 已成功加载且 ABI 校验通过的插件描述（name@version）。
    std::vector<std::string> loaded_plugins() const;

    /// 加载/校验失败的诊断信息（含路径与原因）。
    std::vector<std::string> errors() const;

    /// 扫描目录列表（未加载任何 so），用于诊断当前发现策略。
    std::vector<std::string> search_paths() const;

    /// 设置宿主服务表；必须在首次访问任一服务表之前调用。
    /// 传 nullptr 表示不注入（插件侧网络能力将不可用）。
    static void set_host_api(const falcon_host_api* host);

    /// 当前宿主服务表（可能为 nullptr）。
    static const falcon_host_api* host_api();

    /// 清空并重新扫描。仅在测试或插件热更新场景使用；调用前必须确保
    /// 所有由此加载器创建的服务实例已销毁。
    void reload();

private:
    PluginLoader();
    ~PluginLoader();

    class Impl;
    std::unique_ptr<Impl> impl_;
    mutable std::mutex mutex_;
};

} // namespace drives
} // namespace falcon
