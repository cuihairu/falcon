/**
 * @file search_service.hpp
 * @brief 资源搜索服务：engines.json 配置驱动（drives 包 ResourceSearchManager 封装）
 * @author Falcon Team
 * @date 2026-09-28
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace falcon::desktop {

/// 一条搜索结果（发现页表格展示形态）
struct SearchResultItem {
    QString title;
    QString url;
    QString size;       // 已格式化字符串；未知为「未知」
    QString source;     // 引擎名（engines.json 的 name）
    QString type;
    QString date;
    int seeders = 0;
    int leechers = 0;
    QString magnet_link; // 仅 url 以 magnet: 开头时非空
};

/// 搜索选项（大小过滤 / 排序 / 数量上限）
struct SearchOptions {
    QString sort_by = "relevance"; // relevance / size / date / seeders
    double min_size = 0;           // 0 = 不限（单位 size_unit）
    double max_size = 0;           // 0 = 不限
    QString size_unit = "mb";      // mb / gb
    int max_results = 50;
};

/**
 * @brief 资源搜索服务
 *
 * Falcon 不内置第三方站点适配器：全部引擎来自
 * ~/.config/falcon/engines.json（首次使用由 SearchEngineCatalog 生成
 * 全 disabled 示例模板）。search() 每次重载该文件——设置页启停即时生效。
 *
 * 线程模型：search() 立即返回，搜索在 QtConcurrent worker 中执行；
 * 信号跨线程排队投递，SearchCallback 经 invokeMethod 编组回本对象
 * 所在线程后调用（消费方可在回调里直接操作 GUI 控件）。
 * 析构等待全部在途 worker（cancelled 置位短路结果投递）。
 */
class SearchService : public QObject {
    Q_OBJECT
public:
    using SearchCallback = std::function<void(const QList<SearchResultItem>&)>;

    explicit SearchService(QObject* parent = nullptr);
    ~SearchService() override;

    /// 发起搜索（重复调用作废前一次的结果投递）
    void search(const QString& keyword, const SearchOptions& options = {},
                SearchCallback callback = {});

    /// 请求取消当前搜索（结果丢弃；不影响下一次 search）
    void cancel_search();

    /// 字节量格式化（GB/MB/KB/B 两级小数）
    static QString format_size(quint64 bytes);

signals:
    void search_started(const QString& keyword);
    void search_finished(const QString& keyword, int result_count);
    void search_error(const QString& keyword, const QString& error);

private:
    struct Impl;
    std::unique_ptr<Impl> p_impl_;

    /// worker 主体：载入目录 → manager 搜索 → 映射结果 → 投递
    void run_search(quint64 seq, const QString& keyword,
                    const SearchOptions& options, SearchCallback callback);
    /// 结果作废（被新搜索顶替或显式取消）时丢弃错误投递
    void finish_error(quint64 seq, const QString& keyword, const QString& message);
};

} // namespace falcon::desktop
