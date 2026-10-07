/**
 * @file search_engine_catalog.cpp
 * @brief SearchEngineCatalog 实现（nlohmann JSON + 原子落盘）
 * @author Falcon Team
 * @date 2026-09-28
 */

#include "search_engine_catalog.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace falcon::desktop {

namespace {

namespace fs = std::filesystem;

constexpr int kCatalogVersion = 1;

/// 原子写：tmp + rename（失败清 tmp），沿 trash_store save_locked 模板
bool atomic_write(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        ec.clear();
    }
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            return false;
        }
        out << content;
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/// 全 disabled 示例模板（engines.json 的首次生成物）
std::string default_template_json()
{
    nlohmann::json engine;
    engine["name"] = "example";
    engine["base_url"] = "https://example.com";
    engine["search_path"] = "/search";
    engine["params"] = nlohmann::json{{"q", ""}};
    engine["response_format"] = "html";
    engine["enabled"] = false; // 模板必须显式 false——manager 侧 enabled 缺省为 true
    engine["delay_ms"] = 2000;
    engine["selectors"] = nlohmann::json{
        // 自定义定界符 rx——正则内容含 `)"`（href 捕获组），默认 )"( 会被提前终止
        {"item", R"rx(<a class="result-title" href="([^"]+)")rx"},
        {"title", R"(>\s*([^<]{1,200}?)\s*<)"}};

    nlohmann::json root;
    root["version"] = kCatalogVersion;
    // 文档键：drives load_config 静默忽略未知键，借位写给手工编辑者
    root["_usage"] = nlohmann::json{
        {"how_to", "推荐在 Falcon 设置页「资源搜索」组点「添加引擎」按表单填写（校验 + 即时生效）；手工编辑可复制 example 条目按目标站点修改"},
        {"fields", nlohmann::json{
            {"name", "引擎唯一名（设置页显示）"},
            {"base_url", "站点根地址"},
            {"search_path", "搜索路径，与 base_url 拼接；关键词经 params 中留空的 q/search/keyword 键传入"},
            {"params", "查询参数；值留空的 q/search/keyword 键自动填入关键词"},
            {"selectors", "结果解析正则：item 匹配每条结果的起始（捕获组1=链接），title 捕获标题"},
            {"enabled", "是否启用（设置页可切换）"},
            {"delay_ms", "与上一引擎请求的最小间隔毫秒数"},
            {"headers", "可选：额外请求头（键值对）"}}}};

    root["global_settings"] = nlohmann::json{
        {"default_delay_ms", 2000},
        {"proxy", nlohmann::json{{"enable", false}, {"host", ""}, {"port", 0}}}};

    root["search_engines"] = nlohmann::json::array({engine});
    return root.dump(2);
}

/// 从 JSON 引擎条目投影 CatalogEngine（name/base_url 非字符串返回 false——
/// 与 drives load_config 直接 .get 抛异常的拒绝语义对齐）
bool engine_from_json(const nlohmann::json& j, CatalogEngine& out)
{
    if (!j.is_object()) {
        return false;
    }
    const auto name = j.find("name");
    const auto base_url = j.find("base_url");
    if (name == j.end() || !name->is_string()
        || base_url == j.end() || !base_url->is_string()) {
        return false;
    }
    out.name = name->get<std::string>();
    out.base_url = base_url->get<std::string>();
    out.search_path = j.value("search_path", std::string{});
    out.response_format = j.value("response_format", std::string{});
    out.enabled = j.value("enabled", true); // 与 manager 侧同缺省
    out.delay_ms = j.value("delay_ms", 2000);

    const auto read_string_map = [&j](const char* key,
                                      std::map<std::string, std::string>& out_map) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_object()) {
            return;
        }
        for (auto const& [k, v] : it->items()) {
            if (v.is_string()) {
                out_map[k] = v.get<std::string>();
            }
        }
    };
    read_string_map("params", out.params);
    read_string_map("selectors", out.selectors);
    read_string_map("headers", out.headers);
    return true;
}

/// 读入完整 JSON 根对象（文件缺失 / 非 object / 解析失败返回 false——
/// 与 load() 的损坏不写盘语义一致，写路径绝不在损坏文件上读改写）
bool load_root(const std::string& path, nlohmann::json& out)
{
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    try {
        out = nlohmann::json::parse(in);
    } catch (const std::exception&) {
        return false;
    }
    return out.is_object();
}

nlohmann::json* find_search_engines(nlohmann::json& root)
{
    const auto arr = root.find("search_engines");
    if (arr == root.end() || !arr->is_array()) {
        return nullptr;
    }
    return &*arr;
}

nlohmann::json* find_engine_by_name(nlohmann::json& arr, const std::string& name)
{
    for (auto& item : arr) {
        if (!item.is_object()) {
            continue;
        }
        const auto it = item.find("name");
        if (it != item.end() && it->is_string()
            && it->get<std::string>() == name) {
            return &item;
        }
    }
    return nullptr;
}

/// 表单可见键写回引擎对象；params/selectors/headers 空表 = 移除键
void apply_engine_fields(nlohmann::json& item, const CatalogEngine& engine)
{
    item["base_url"] = engine.base_url;
    item["search_path"] = engine.search_path;
    item["response_format"] = engine.response_format;
    item["enabled"] = engine.enabled;
    item["delay_ms"] = engine.delay_ms;

    const auto write_string_map = [&item](
        const char* key, const std::map<std::string, std::string>& map) {
        item.erase(key);
        if (!map.empty()) {
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& [k, v] : map) {
                obj[k] = v;
            }
            item[key] = std::move(obj);
        }
    };
    write_string_map("params", engine.params);
    write_string_map("selectors", engine.selectors);
    write_string_map("headers", engine.headers);
}

} // namespace

SearchEngineCatalog::SearchEngineCatalog(std::string path)
    : path_(std::move(path))
{
    if (path_.empty()) {
        path_ = default_path();
    }
}

std::string SearchEngineCatalog::default_path()
{
    // XDG_CONFIG_HOME / HOME / USERPROFILE 依次兜底；全空回落 cwd
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return (fs::path(xdg) / "falcon" / "engines.json").string();
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return (fs::path(home) / ".config" / "falcon" / "engines.json").string();
    }
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) {
        return (fs::path(profile) / ".config" / "falcon" / "engines.json").string();
    }
    return "engines.json";
}

bool SearchEngineCatalog::ensure_default(const std::string& path)
{
    std::error_code ec;
    if (fs::exists(path, ec) && !ec) {
        return true; // 已有配置（含用户手改），绝不覆盖
    }
    return atomic_write(path, default_template_json());
}

bool SearchEngineCatalog::load()
{
    engines_.clear();
    std::ifstream in(path_);
    if (!in) {
        return false; // 首次使用 / 文件缺失：空目录录，不写盘
    }
    try {
        nlohmann::json root = nlohmann::json::parse(in);
        if (!root.is_object()) {
            return false;
        }
        const auto arr = root.find("search_engines");
        if (arr == root.end() || !arr->is_array()) {
            return false;
        }
        std::vector<CatalogEngine> parsed;
        parsed.reserve(arr->size());
        for (const auto& item : *arr) {
            CatalogEngine engine;
            if (!engine_from_json(item, engine)) {
                return false; // 任一引擎缺 name/base_url：manager 会整份拒绝，
                              // 目录同步按损坏处理保持两侧一致
            }
            parsed.push_back(std::move(engine));
        }
        engines_ = std::move(parsed);
        return true;
    } catch (const std::exception&) {
        engines_.clear();
        return false; // JSON 损坏按空目录处理，不写盘
    }
}

bool SearchEngineCatalog::has_enabled() const
{
    for (const auto& engine : engines_) {
        if (engine.enabled) {
            return true;
        }
    }
    return false;
}

bool SearchEngineCatalog::set_enabled(const std::string& name, bool enabled)
{
    nlohmann::json root;
    if (!load_root(path_, root)) {
        return false;
    }
    const auto arr = root.find("search_engines");
    if (arr == root.end() || !arr->is_array()) {
        return false;
    }
    bool found = false;
    for (auto& item : *arr) {
        if (!item.is_object()) {
            continue;
        }
        const auto it = item.find("name");
        if (it != item.end() && it->is_string() && it->get<std::string>() == name) {
            item["enabled"] = enabled; // 仅改该键，headers/params/selectors 全保留
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    return atomic_write(path_, root.dump(2));
}

bool SearchEngineCatalog::upsert_engine(const CatalogEngine& engine)
{
    nlohmann::json root;
    if (!load_root(path_, root)) {
        return false;
    }
    auto* arr = find_search_engines(root);
    if (!arr) {
        return false;
    }
    if (auto* existing = find_engine_by_name(*arr, engine.name)) {
        // 仅覆盖表单可见键——该引擎对象上的未知键原样保留
        apply_engine_fields(*existing, engine);
    } else {
        nlohmann::json item = nlohmann::json::object();
        item["name"] = engine.name;
        apply_engine_fields(item, engine);
        arr->push_back(std::move(item));
    }
    return atomic_write(path_, root.dump(2));
}

bool SearchEngineCatalog::remove_engine(const std::string& name)
{
    nlohmann::json root;
    if (!load_root(path_, root)) {
        return false;
    }
    auto* arr = find_search_engines(root);
    if (!arr) {
        return false;
    }
    bool found = false;
    nlohmann::json kept = nlohmann::json::array();
    for (auto& item : *arr) {
        if (!found && item.is_object()) {
            const auto it = item.find("name");
            if (it != item.end() && it->is_string()
                && it->get<std::string>() == name) {
                found = true;
                continue; // 只删第一个同名条目（name 是唯一键）
            }
        }
        kept.push_back(std::move(item));
    }
    if (!found) {
        return false;
    }
    *arr = std::move(kept);
    return atomic_write(path_, root.dump(2));
}

} // namespace falcon::desktop
