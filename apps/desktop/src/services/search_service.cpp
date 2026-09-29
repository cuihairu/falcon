/**
 * @file search_service.cpp
 * @brief SearchService 实现（engines.json → ResourceSearchManager）
 * @author Falcon Team
 * @date 2026-09-28
 */

#include "search_service.hpp"

#include "search_engine_catalog.hpp"

#include <falcon/drives/resource_search.hpp>

#include <QMetaObject>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <mutex>
#include <vector>

namespace falcon::desktop {

namespace {

constexpr size_t kDefaultLimit = 50;

/// DiscoveryPage 排序键 → manager sort_by（"" = 相关度，manager 回落
/// confidence 降序；"size"/"date"/"seeds" 为 manager 仅有的字段排序）
std::string sort_by_for_manager(const QString& sort)
{
    if (sort == QLatin1String("size")) {
        return "size";
    }
    if (sort == QLatin1String("date")) {
        return "date";
    }
    if (sort == QLatin1String("seeders")) {
        return "seeds";
    }
    return {};
}

quint64 size_unit_factor(const QString& unit)
{
    return unit == QLatin1String("gb") ? 1024ull * 1024 * 1024
                                       : 1024ull * 1024;
}

} // namespace

struct SearchService::Impl {
    std::mutex mutex;
    std::vector<QFuture<void>> inflight; // 析构等待在途 worker，防 this 悬垂
    std::atomic<bool> cancelled{false};
    std::atomic<quint64> search_seq{0};

    void track(QFuture<void> future)
    {
        std::lock_guard<std::mutex> lock(mutex);
        inflight.erase(std::remove_if(inflight.begin(), inflight.end(),
                                      [](const QFuture<void>& f) {
                                          return f.isFinished();
                                      }),
                       inflight.end());
        inflight.push_back(std::move(future));
    }

    void wait_all()
    {
        std::vector<QFuture<void>> local;
        {
            std::lock_guard<std::mutex> lock(mutex);
            local.swap(inflight);
        }
        for (auto& future : local) {
            future.waitForFinished();
        }
    }
};

SearchService::SearchService(QObject* parent)
    : QObject(parent)
    , p_impl_(std::make_unique<Impl>()) {}

SearchService::~SearchService()
{
    p_impl_->cancelled.store(true);
    p_impl_->wait_all();
}

void SearchService::search(const QString& keyword, const SearchOptions& options,
                           SearchCallback callback)
{
    const QString trimmed = keyword.trimmed();
    if (trimmed.isEmpty()) {
        emit search_error(keyword, tr("关键词为空。"));
        return;
    }

    // 序号自增使全部在途旧结果作废（worker 完成时按 seq 不匹配丢弃）
    const quint64 seq = p_impl_->search_seq.fetch_add(1) + 1;
    p_impl_->cancelled.store(false);

    emit search_started(trimmed);

    p_impl_->track(QtConcurrent::run(
        [this, seq, trimmed, options, callback = std::move(callback)]() {
            run_search(seq, trimmed, options, std::move(callback));
        }));
}

void SearchService::cancel_search()
{
    p_impl_->cancelled.store(true);
}

void SearchService::run_search(quint64 seq, const QString& keyword,
                               const SearchOptions& options,
                               SearchCallback callback)
{
    // 每次搜索重载目录：设置页启停即时生效（文件缺失/损坏按空目录，
    // 与 manager load_config 的整份拒绝语义一致）
    SearchEngineCatalog catalog;
    if (!catalog.load() || !catalog.has_enabled()) {
        finish_error(seq, keyword,
                     tr("未启用任何搜索引擎。请在设置页「资源搜索」组勾选启用，"
                        "或编辑 engines.json 添加引擎。"));
        return;
    }

    falcon::search::ResourceSearchManager manager;
    if (!manager.load_config(catalog.path())) {
        finish_error(seq, keyword,
                     tr("engines.json 无法加载（格式无效），请修正后重试。"));
        return;
    }

    falcon::search::SearchQuery query;
    query.keyword = keyword.toStdString();
    query.sort_by = sort_by_for_manager(options.sort_by);
    const quint64 factor = size_unit_factor(options.size_unit);
    if (options.min_size > 0) {
        query.min_size = static_cast<size_t>(options.min_size * factor);
    }
    if (options.max_size > 0) {
        query.max_size = static_cast<size_t>(options.max_size * factor);
    }
    query.limit = options.max_results > 0
        ? static_cast<size_t>(options.max_results) : kDefaultLimit;

    const auto results = manager.search_all(query);

    QList<SearchResultItem> items;
    items.reserve(static_cast<int>(results.size()));
    for (const auto& r : results) {
        SearchResultItem item;
        item.title = QString::fromStdString(r.title);
        item.url = QString::fromStdString(r.url);
        item.size = r.size > 0
            ? format_size(static_cast<quint64>(r.size))
            : tr("未知");
        item.source = QString::fromStdString(r.source);
        item.type = QString::fromStdString(r.type);
        item.date = QString::fromStdString(r.publish_date);
        item.seeders = r.seeds;
        item.leechers = r.peers;
        // 磁力标记仅对真实 magnet: 链接成立，不按来源猜
        item.magnet_link = item.url.startsWith(QLatin1String("magnet:"))
            ? item.url : QString();
        items.append(item);
    }

    if (p_impl_->cancelled.load() || seq != p_impl_->search_seq.load()) {
        return;
    }
    emit search_finished(keyword, static_cast<int>(items.size()));

    if (!callback) {
        return;
    }
    // 回调在 worker 线程不能直调（消费方 display_results 操作 GUI 控件）
    // ——编组回本对象所在线程；执行时再校验一次 seq/cancelled 防迟到旧结果
    const quint64 exec_seq = seq;
    QMetaObject::invokeMethod(
        this,
        [this, exec_seq, callback = std::move(callback), items]() {
            if (p_impl_->cancelled.load()
                || exec_seq != p_impl_->search_seq.load()) {
                return;
            }
            callback(items);
        },
        Qt::QueuedConnection);
}

void SearchService::finish_error(quint64 seq, const QString& keyword,
                                 const QString& message)
{
    if (p_impl_->cancelled.load() || seq != p_impl_->search_seq.load()) {
        return;
    }
    emit search_error(keyword, message);
}

QString SearchService::format_size(quint64 bytes)
{
    if (bytes >= 1024ull * 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
    }
    if (bytes >= 1024ull * 1024) {
        return QString::number(bytes / (1024.0 * 1024), 'f', 2) + " MB";
    }
    if (bytes >= 1024) {
        return QString::number(bytes / 1024.0, 'f', 2) + " KB";
    }
    return QString::number(bytes) + " B";
}

} // namespace falcon::desktop
