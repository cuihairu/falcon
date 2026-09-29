/**
 * @file search_engine_catalog.hpp
 * @brief 资源搜索引擎目录：engines.json 的读取 / 默认模板生成 / 启停切换（纯 C++，不依赖 Qt）
 * @author Falcon Team
 * @date 2026-09-28
 */

#pragma once

#include <string>
#include <vector>

namespace falcon::desktop {

/// 目录中的一个搜索引擎条目（engines.json search_engines 数组的字段投影；
/// headers/params/selectors 等高级字段不在此展示——set_enabled 全字段保留）
struct CatalogEngine {
    std::string name;             ///< 引擎名（唯一键，资源搜索设置按此启停）
    std::string base_url;         ///< 站点根地址
    std::string search_path;      ///< 搜索路径（与 base_url 拼接）
    std::string response_format;  ///< html / json（信息性；解析由 drives 选择器完成）
    bool enabled = false;         ///< 是否启用（缺省 false——模板显式写 false）
    int delay_ms = 2000;          ///< 请求间隔（防封禁）
};

/**
 * @brief 搜索引擎目录（~/.config/falcon/engines.json）
 *
 * Falcon 不内置任何第三方站点适配器：发现页搜索完全由 engines.json
 * 配置驱动（drives 包 ResourceSearchManager 按同一文件 load_config），
 * 目录负责三件事：
 *   1. load()          读取目录供设置页展示
 *   2. ensure_default() 首次使用生成全 disabled 的示例模板
 *   3. set_enabled()    启停切换（磁盘读-改-写，其余字段原样保留）
 *
 * load() 对文件缺失 / 损坏 / 缺 search_engines 数组 / 引擎缺 name 或
 * base_url 一律按空目录处理且绝不写盘（与 drives load_config 的拒绝
 * 语义一致——目录显示的引擎集合必须是 manager 真能加载的）。
 */
class SearchEngineCatalog {
public:
    /// path 为空时取 default_path()
    explicit SearchEngineCatalog(std::string path = default_path());

    /// 配置路径：XDG_CONFIG_HOME → $HOME/.config → USERPROFILE → cwd
    static std::string default_path();

    /// 文件已存在直接返回 true；否则生成全 disabled 示例模板（原子写）。
    /// 返回 false 表示模板写入失败。
    static bool ensure_default(const std::string& path);

    /// 读取目录；返回 false 表示文件缺失 / 损坏（engines() 为空，不写盘）
    bool load();

    const std::vector<CatalogEngine>& engines() const { return engines_; }
    bool has_enabled() const;
    const std::string& path() const { return path_; }

    /// 翻转指定引擎的 enabled 并原子落盘（重读完整 JSON，仅改该键——
    /// headers/params/selectors/_usage 等字段全保留）。
    /// 文件缺失 / 损坏 / 引擎名不存在返回 false。
    bool set_enabled(const std::string& name, bool enabled);

private:
    std::string path_;
    std::vector<CatalogEngine> engines_;
};

} // namespace falcon::desktop
