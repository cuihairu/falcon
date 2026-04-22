/**
 * @file search_service.hpp
 * @brief 资源搜索服务 - 提供磁力链接、HTTP资源、网盘资源搜索
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QVariantMap>
#include <QSharedPointer>
#include <QSet>
#include <QFuture>
#include <QDateTime>
#include <functional>
#include <QtConcurrent>

namespace falcon::desktop {

/**
 * @brief 搜索结果项
 */
struct SearchResultItem {
    QString title;
    QString url;
    QString size;
    QString source;
    QString type;
    QString date;
    int seeders = 0;
    int leechers = 0;
    QString magnet_link;
};

/**
 * @brief 搜索选项
 */
struct SearchOptions {
    QString search_type;   // magnet, http, cloud, ftp
    QString category;      // all, video, audio, document, software, image
    QString sort_by;       // relevance, size, date, seeders
    qint64 min_size = 0;
    qint64 max_size = 0;
    QString size_unit;     // mb, gb
    int max_results = 50;
};

/**
 * @brief 搜索服务
 */
class SearchService : public QObject {
    Q_OBJECT

public:
    explicit SearchService(QObject* parent = nullptr);
    ~SearchService() override;

    using SearchCallback = std::function<void(const QList<SearchResultItem>&)>;

    // 搜索方法
    void search(const QString& keyword, const SearchOptions& options, SearchCallback callback);
    void search_magnet(const QString& keyword, const SearchOptions& options, SearchCallback callback);
    void search_http(const QString& keyword, const SearchOptions& options, SearchCallback callback);
    void search_cloud(const QString& keyword, const SearchOptions& options, SearchCallback callback);
    void search_ftp(const QString& keyword, const SearchOptions& options, SearchCallback callback);

    // 取消搜索
    void cancel_search();

signals:
    void search_started(const QString& keyword);
    void search_finished(const QString& keyword, int result_count);
    void search_error(const QString& keyword, const QString& error);
    void search_progress(const QString& keyword, int current, int total);

private:
    struct Impl;
    QSharedPointer<Impl> p_impl_;
};

} // namespace falcon::desktop
