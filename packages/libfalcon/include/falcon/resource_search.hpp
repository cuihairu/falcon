/**
 * @file resource_search.hpp
 * @brief 资源搜索插件接口定义
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include "plugin.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>

namespace falcon {
namespace search {

/**
 * @brief 搜索结果项
 */
struct SearchResult {
    std::string title;           // 资源标题
    std::string url;             // 下载链接（magnet/http/torrent）
    std::string source;          // 来源网站
    size_t size;                 // 文件大小（字节）
    std::string type;            // 资源类型（video/audio/software等）
    int seeds;                   // 种子数（BT资源）
    int peers;                   // 连接数（BT资源）
    double confidence;           // 置信度（0.0-1.0）
    std::string hash;            // 资源哈希值
    std::string publish_date;    // 发布日期
    std::map<std::string, std::string> metadata;  // 额外元数据
};

/**
 * @brief 搜索查询参数
 */
struct SearchQuery {
    std::string keyword;         // 搜索关键词
    std::string category;        // 分类筛选（可选）
    size_t min_size = 0;         // 最小文件大小（可选）
    size_t max_size = 0;         // 最大文件大小（可选）
    int min_seeds = 0;           // 最小种子数（可选）
    size_t limit = 50;           // 结果数量限制
    int page = 1;                // 页码
    std::string sort_by;         // 排序字段（size/seeds/date）
    bool sort_desc = true;       // 是否降序
};

/**
 * @brief 搜索引擎配置
 */
struct SearchEngineConfig {
    std::string name;            // 搜索引擎名称
    std::string base_url;        // 基础URL
    std::string search_path;     // 搜索路径
    std::map<std::string, std::string> headers;     // HTTP头
    std::map<std::string, std::string> params;      // 默认参数
    std::string encoding = "utf-8";  // 响应编码
    int delay_ms = 1000;         // 请求延迟（毫秒）
    bool enabled = true;         // 是否启用
    double weight = 1.0;         // 权重（用于结果排序）
};

/**
 * @brief 资源搜索器接口
 */
class ISearchProvider {
public:
    virtual ~ISearchProvider() = default;

    /**
     * @brief 获取搜索引擎名称
     */
    virtual std::string name() const = 0;

    /**
     * @brief 搜索资源
     * @param query 搜索查询参数
     * @return 搜索结果列表
     */
    virtual std::vector<SearchResult> search(const SearchQuery& query) = 0;

    /**
     * @brief 验证搜索结果链接是否有效
     * @param url 资源链接
     * @return 是否有效
     */
    virtual bool validate_url(const std::string& url) = 0;

    /**
     * @brief 获取资源详细信息
     * @param url 资源链接
     * @return 详细信息
     */
    virtual SearchResult get_details(const std::string& url) = 0;

    /**
     * @brief 检查搜索引擎是否可用
     */
    virtual bool is_available() = 0;

    /**
     * @brief 获取搜索延迟（毫秒）
     */
    virtual int get_delay() const { return 1000; }
};

/**
 * @brief 资源搜索管理器
 */
class ResourceSearchManager {
public:
    ResourceSearchManager();
    ~ResourceSearchManager();

    /**
     * @brief 注册搜索引擎
     */
    void register_provider(std::unique_ptr<ISearchProvider> provider);

    /**
     * @brief 从配置文件加载搜索引擎
     */
    bool load_config(const std::string& config_file);

    /**
     * @brief 搜索所有启用的搜索引擎
     */
    std::vector<SearchResult> search_all(const SearchQuery& query);

    /**
     * @brief 搜索指定的搜索引擎
     */
    std::vector<SearchResult> search_providers(
        const SearchQuery& query,
        const std::vector<std::string>& provider_names
    );

    /**
     * @brief 获取已注册的搜索引擎列表
     */
    std::vector<std::string> get_providers() const;

    /**
     * @brief 获取搜索建议（自动补全）
     */
    std::vector<std::string> get_suggestions(const std::string& keyword);

    /**
     * @brief 设置全局搜索延迟
     */
    void set_global_delay(int delay_ms);

    /**
     * @brief 启用/禁用搜索引擎
     */
    void enable_provider(const std::string& name, bool enabled);

private:
    class Impl;
    std::unique_ptr<Impl> p_impl;
};

/**
 * @brief 通用网页爬虫基类
 */
class WebCrawlerBase {
public:
    WebCrawlerBase(const SearchEngineConfig& config);
    virtual ~WebCrawlerBase() = default;

protected:
    /**
     * @brief 发送HTTP GET请求
     */
    std::string http_get(const std::string& url, const std::map<std::string, std::string>& params = {});

    /**
     * @brief 解析HTML内容（简单实现）
     */
    std::vector<std::map<std::string, std::string>> parse_html(
        const std::string& html,
        const std::string& item_selector,
        const std::map<std::string, std::string>& field_selectors
    );

    /**
     * @brief URL编码
     */
    std::string url_encode(const std::string& str);

    /**
     * @brief 解析文件大小
     */
    size_t parse_size(const std::string& size_str);

    /**
     * @brief 解析日期
     */
    std::string parse_date(const std::string& date_str);

    SearchEngineConfig config_;
    int last_request_time_ = 0;
};

} // namespace search
} // namespace falcon