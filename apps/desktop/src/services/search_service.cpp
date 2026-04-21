/**
 * @file search_service.cpp
 * @brief 资源搜索服务实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "search_service.hpp"
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QtConcurrent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QTimer>

namespace falcon::desktop {

struct SearchService::Impl {
    QNetworkAccessManager* network_manager = nullptr;
    QMutex mutex;
    bool cancelled = false;
    QString current_search_id;

    // 模拟搜索数据生成器
    QList<SearchResultItem> generate_mock_magnet_results(const QString& keyword) {
        QList<SearchResultItem> results;

        // 示例数据 - 实际实现应调用真实的搜索 API
        SearchResultItem item1;
        item1.title = QString("%1 [2025] BluRay 1080p").arg(keyword);
        item1.url = QString("magnet:?xt=urn:btih:%1").arg(generate_mock_hash());
        item1.magnet_link = item1.url;
        item1.size = "4.2 GB";
        item1.source = "示例种子站 1";
        item1.type = "video";
        item1.date = "2025-12-21";
        item1.seeders = 1523;
        item1.leechers = 456;
        results.append(item1);

        SearchResultItem item2;
        item2.title = QString("%1 软件包 v2.0").arg(keyword);
        item2.url = QString("magnet:?xt=urn:btih:%1").arg(generate_mock_hash());
        item2.magnet_link = item2.url;
        item2.size = "850 MB";
        item2.source = "示例种子站 2";
        item2.type = "software";
        item2.date = "2025-12-20";
        item2.seeders = 892;
        item2.leechers = 234;
        results.append(item2);

        return results;
    }

    QList<SearchResultItem> generate_mock_http_results(const QString& keyword) {
        QList<SearchResultItem> results;

        SearchResultItem item1;
        item1.title = QString("%1_文档.pdf").arg(keyword);
        item1.url = QString("https://example.com/files/%1.pdf").arg(keyword);
        item1.size = "2.5 MB";
        item1.source = "示例文件站";
        item1.type = "document";
        item1.date = "2025-12-19";
        results.append(item1);

        return results;
    }

    QList<SearchResultItem> generate_mock_cloud_results(const QString& keyword) {
        QList<SearchResultItem> results;

        SearchResultItem item1;
        item1.title = QString("%1_合集.zip").arg(keyword);
        item1.url = QString("https://pan.example.com/s/%1").arg(generate_mock_hash().left(8));
        item1.size = "1.2 GB";
        item1.source = "示例网盘";
        item1.type = "archive";
        item1.date = "2025-12-18";
        results.append(item1);

        return results;
    }

    static QString generate_mock_hash() {
        const char hex_chars[] = "0123456789abcdef";
        QString hash;
        for (int i = 0; i < 40; ++i) {
            hash += hex_chars[qrand() % 16];
        }
        return hash;
    }
};

SearchService::SearchService(QObject* parent)
    : QObject(parent)
    , p_impl_(std::make_shared<Impl>()) {
    p_impl_->network_manager = new QNetworkAccessManager(this);
}

SearchService::~SearchService() = default;

void SearchService::search(const QString& keyword, const SearchOptions& options, SearchCallback callback) {
    emit search_started(keyword);

    p_impl_->cancelled = false;
    p_impl_->current_search_id = keyword + ":" + QString::number(QDateTime::currentMSecsSinceEpoch());

    Qt::Concurrent::run([this, keyword, options, callback]() {
        QList<SearchResultItem> results;

        // 根据搜索类型调用不同的搜索方法
        if (options.search_type == "magnet") {
            results = search_magnet_sync(keyword, options);
        } else if (options.search_type == "http") {
            results = search_http_sync(keyword, options);
        } else if (options.search_type == "cloud") {
            results = search_cloud_sync(keyword, options);
        } else if (options.search_type == "ftp") {
            results = search_ftp_sync(keyword, options);
        } else {
            // 默认搜索全部
            results = search_magnet_sync(keyword, options);
            results += search_http_sync(keyword, options);
        }

        if (!p_impl_->cancelled) {
            emit search_finished(keyword, results.size());
            if (callback) {
                callback(results);
            }
        }
    });
}

void SearchService::search_magnet(const QString& keyword, const SearchOptions& options, SearchCallback callback) {
    Qt::Concurrent::run([this, keyword, options, callback]() {
        auto results = search_magnet_sync(keyword, options);
        if (callback) {
            callback(results);
        }
    });
}

void SearchService::search_http(const QString& keyword, const SearchOptions& options, SearchCallback callback) {
    Qt::Concurrent::run([this, keyword, options, callback]() {
        auto results = search_http_sync(keyword, options);
        if (callback) {
            callback(results);
        }
    });
}

void SearchService::search_cloud(const QString& keyword, const SearchOptions& options, SearchCallback callback) {
    Qt::Concurrent::run([this, keyword, options, callback]() {
        auto results = search_cloud_sync(keyword, options);
        if (callback) {
            callback(results);
        }
    });
}

void SearchService::search_ftp(const QString& keyword, const SearchOptions& options, SearchCallback callback) {
    Qt::Concurrent::run([this, keyword, options, callback]() {
        auto results = search_ftp_sync(keyword, options);
        if (callback) {
            callback(results);
        }
    });
}

void SearchService::cancel_search() {
    p_impl_->cancelled = true;
}

// ============================================================================
// 同步搜索实现（在后台线程运行）
// ============================================================================

QList<SearchResultItem> SearchService::search_magnet_sync(const QString& keyword, const SearchOptions& options) {
    Q_UNUSED(options);

    // 在实际实现中，这里会调用真实的磁力链接搜索 API
    // 可能的 API 源：
    // - 各种公共 BT tracker
    // - 磁力搜索网站（需要爬取或使用 API）
    // - DHT 网络搜索

    // 模拟网络延迟
    QThread::msleep(500);

    return p_impl_->generate_mock_magnet_results(keyword);
}

QList<SearchResultItem> SearchService::search_http_sync(const QString& keyword, const SearchOptions& options) {
    Q_UNUSED(options);

    // 在实际实现中，这里会：
    // 1. 调用文件搜索引擎
    // 2. 搜索公开的文件索引
    // 3. 查询各种下载站点的 API

    QThread::msleep(300);

    return p_impl_->generate_mock_http_results(keyword);
}

QList<SearchResultItem> SearchService::search_cloud_sync(const QString& keyword, const SearchOptions& options) {
    Q_UNUSED(options);

    // 在实际实现中，这里会：
    // 1. 搜索各大网盘的公开分享链接
    // 2. 查询网盘资源索引站

    QThread::msleep(400);

    return p_impl_->generate_mock_cloud_results(keyword);
}

QList<SearchResultItem> SearchService::search_ftp_sync(const QString& keyword, const SearchOptions& options) {
    Q_UNUSED(keyword);
    Q_UNUSED(options);

    // FTP 搜索通常需要访问特定的 FTP 服务器
    // 这里返回空列表
    return {};
}

} // namespace falcon::desktop
