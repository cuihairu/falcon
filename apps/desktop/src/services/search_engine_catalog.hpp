/**
 * @file search_engine_catalog.hpp
 * @brief 资源搜索引擎目录：engines.json 的读取 / 默认模板生成 / 启停切换（纯 C++，不依赖 Qt）
 * @author Falcon Team
 * @date 2026-09-28
 */

#pragma once

#include <map>
#include <string>
#include <vector>

namespace falcon::desktop {

/// 目录中的一个搜索引擎条目（engines.json search_engines 数组的字段投影；
/// params/selectors/headers 仅收字符串值——非字符串值 drives load_config
/// 会整份拒绝，展示层保持宽松）
struct CatalogEngine {
    std::string name;             ///< 引擎名（唯一键，资源搜索设置按此启停）
    std::string base_url;         ///< 站点根地址
    std::string search_path;      ///< 搜索路径（与 base_url 拼接；关键词经 params 传入）
    std::string response_format;  ///< html / json（信息性；解析由 drives 选择器完成）
    bool enabled = false;         ///< 是否启用（缺省 false——模板显式写 false）
    int delay_ms = 2000;          ///< 请求间隔（防封禁）
    std::map<std::string, std::string> params;     ///< 查询参数（留空 q/search/keyword 自动填关键词）
    std::map<std::string, std::string> selectors;  ///< HTML 结果解析正则（item/title/url/size/seeds…）
    std::map<std::string, std::string> headers;    ///< 可选：额外请求头
};

/**
 * @brief 搜索引擎目录（~/.config/falcon/engines.json）
 *
 * Falcon 不内置任何第三方站点适配器：发现页搜索完全由 engines.json
 * 配置驱动（drives 包 ResourceSearchManager 按同一文件 load_config），
 * 目录负责四件事：
 *   1. load()           读取目录供设置页展示
 *   2. ensure_default() 首次使用生成全 disabled 的示例模板
 *   3. set_enabled()    启停切换（磁盘读-改-写，其余字段原样保留）
 *   4. upsert/remove    设置页「添加/编辑/删除引擎」的写路径
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

    /// 新增或更新引擎并原子落盘（设置页添加/编辑表单的写路径）。
    /// 同名引擎：仅覆盖表单可见的已知键，引擎对象上的未知键
    /// （headers、手写扩展字段等）原样保留；新引擎：追加到数组尾。
    /// params/selectors/headers 为空时移除对应键（表单清空 = 删除）。
    /// 文件缺失 / 损坏返回 false。
    bool upsert_engine(const CatalogEngine& engine);

    /// 按名删除引擎并原子落盘；文件缺失 / 损坏 / 名字不存在返回 false。
    bool remove_engine(const std::string& name);

private:
    std::string path_;
    std::vector<CatalogEngine> engines_;
};

} // namespace falcon::desktop
