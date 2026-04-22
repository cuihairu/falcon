/**
 * @file search_service.cpp
 * @brief 资源搜索服务实现 - 支持真实搜索 API
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
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace falcon::desktop {

// ============================================================================
// 搜索源配置
// ============================================================================

namespace {
    // 公共磁力搜索源（这些网站提供 HTML 或 API 接口）
    const char* MAGNET_SOURCES[] = {
        "https://www.bttiantang.com",      // BT 天堂
        "https://www.btdidi.com",           // BT 弟弟
        "https://www.torrentkitty.com",     // Torrent Kitty
        nullptr
    };

    // 用户代理
    const char* USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                             "AppleWebKit/537.36 (KHTML, like Gecko) "
                             "Chrome/120.0.0.0 Safari/537.36";

    /**
     * @brief 从 HTML 中提取磁力链接
     */
    QString extract_magnet_link(const QString& html) {
        QRegularExpression re("magnet:\\?xt=urn:btih:[a-fA-F0-9]{40}[^\"'<>\\s]*");
        QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            return match.captured(0);
        }
        return QString();
    }

    /**
     * @brief 从 HTML 中提取多个磁力链接
     */
    QStringList extract_magnet_links(const QString& html) {
        QStringList links;
        QRegularExpression re("magnet:\\?xt=urn:btih:[a-fA-F0-9]{40}[^\"'<>\\s]*");
        QRegularExpressionMatchIterator it = re.globalMatch(html);
        while (it.hasNext()) {
            QRegularExpressionMatch match = it.next();
            links.append(match.captured(0));
        }
        return links;
    }

    /**
     * @brief 从 HTML 中提取标题
     */
    QString extract_title(const QString& html) {
        // 尝试提取 <title> 标签
        QRegularExpression re("<title[^>]*>([^<]+)</title>", QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            return match.captured(1).trimmed();
        }
        return QString();
    }

    /**
     * @brief 解析文件大小字符串为字节数
     */
    qint64 parse_size(const QString& size_str) {
        QString str = size_str.toLower().trimmed();
        qint64 multiplier = 1;

        if (str.endsWith("tb") || str.endsWith("t")) {
            multiplier = 1024LL * 1024LL * 1024LL * 1024LL;
            str.chop(2);
        } else if (str.endsWith("gb") || str.endsWith("g")) {
            multiplier = 1024LL * 1024LL * 1024LL;
            str.chop(2);
        } else if (str.endsWith("mb") || str.endsWith("m")) {
            multiplier = 1024LL * 1024LL;
            str.chop(2);
        } else if (str.endsWith("kb") || str.endsWith("k")) {
            multiplier = 1024LL;
            str.chop(2);
        } else if (str.endsWith("b")) {
            str.chop(1);
        }

        bool ok;
        double value = str.toDouble(&ok);
        if (ok) {
            return static_cast<qint64>(value * multiplier);
        }
        return 0;
    }

    /**
     * @brief 格式化字节数为可读字符串
     */
    QString format_size(qint64 bytes) {
        const qint64 GB = 1024LL * 1024LL * 1024LL;
        const qint64 MB = 1024LL * 1024LL;
        const qint64 KB = 1024LL;

        if (bytes >= GB) {
            return QString::number(bytes / (double)GB, 'f', 2) + " GB";
        } else if (bytes >= MB) {
            return QString::number(bytes / (double)MB, 'f', 2) + " MB";
        } else if (bytes >= KB) {
            return QString::number(bytes / (double)KB, 'f', 2) + " KB";
        } else {
            return QString::number(bytes) + " B";
        }
    }
}

// ============================================================================
// SearchService::Impl
// ============================================================================

struct SearchService::Impl {
    QNetworkAccessManager* network_manager = nullptr;
    QMutex mutex;
    bool cancelled = false;
    QString current_search_id;

    /**
     * @brief 执行 HTTP GET 请求
     */
    QByteArray http_get(const QString& url, int timeout_ms = 10000) {
        if (!network_manager) {
            return QByteArray();
        }

        QNetworkRequest request(QUrl(url));
        request.setHeader(QNetworkRequest::UserAgentHeader, USER_AGENT);

        QNetworkReply* reply = network_manager->get(request);

        // 等待响应完成
        QTimer timer;
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeout_ms);
        loop.exec();

        if (timer.isActive()) {
            timer.stop();
            if (reply->error() == QNetworkReply::NoError) {
                QByteArray data = reply->readAll();
                reply->deleteLater();
                return data;
            }
        }

        reply->deleteLater();
        return QByteArray();
    }

    /**
     * @brief 搜索 BT 天堂
     */
    QList<SearchResultItem> search_bt_tiantang(const QString& keyword) {
        QList<SearchResultItem> results;

        // BT 天堂搜索 URL（示例）
        QString search_url = QString("https://www.bttiantang.com/search?q=%1").arg(keyword);

        QByteArray data = http_get(search_url);
        if (data.isEmpty()) {
            return results;
        }

        QString html = QString::fromUtf8(data);

        // 解析 HTML 提取搜索结果
        // 注意：实际网站结构可能变化，需要根据实际情况调整解析逻辑

        // 简化示例：从 HTML 中提取磁力链接
        QStringList magnets = extract_magnet_links(html);
        for (const QString& magnet : magnets) {
            SearchResultItem item;
            item.title = QString("资源 (%1)").arg(magnet.left(30));
            item.url = magnet;
            item.magnet_link = magnet;
            item.size = "未知";
            item.source = "BT 天堂";
            item.type = "video";
            item.date = QDateTime::currentDateTime().toString("yyyy-MM-dd");
            item.seeders = 0;
            item.leechers = 0;
            results.append(item);

            if (results.size() >= 20) break;  // 限制结果数量
        }

        return results;
    }

    /**
     * @brief 搜索 Torrent Kitty
     */
    QList<SearchResultItem> search_torrent_kitty(const QString& keyword) {
        QList<SearchResultItem> results;

        // Torrent Kitty 搜索 URL
        QString search_url = QString("https://www.torrentkitty.com/search?q=%1").arg(keyword);

        QByteArray data = http_get(search_url);
        if (data.isEmpty()) {
            return results;
        }

        QString html = QString::fromUtf8(data);

        // Torrent Kitty 使用简单的搜索结果格式
        // 提取磁力链接和标题

        QRegularExpression title_re("<a[^>]*class=\"name\"[^>]*>([^<]+)</a>",
                                     QRegularExpression::CaseInsensitiveOption);
        QRegularExpression magnet_re("magnet:\\?xt=urn:btih:[a-fA-F0-9]{40}[^\"'<>\\s]*");
        QRegularExpression size_re("([\\d.]+\\s*[KMGT]?B)",
                                   QRegularExpression::CaseInsensitiveOption);
        QRegularExpression seed_re(\"作?(\\d+)\\s*种\", QRegularExpression::CaseInsensitiveOption);

        // 简化解析 - 实际需要根据网站结构调整
        QStringList magnets = extract_magnet_links(html);
        QRegularExpressionMatchIterator it = title_re.globalMatch(html);

        int index = 0;
        while (it.hasNext() && index < magnets.size()) {
            QRegularExpressionMatch match = it.next();
            SearchResultItem item;
            item.title = match.captured(1).trimmed();
            item.url = magnets[index];
            item.magnet_link = magnets[index];
            item.size = "未知";
            item.source = "Torrent Kitty";
            item.type = "video";
            item.date = QDateTime::currentDateTime().toString("yyyy-MM-dd");
            item.seeders = 0;
            item.leechers = 0;
            results.append(item);
            index++;

            if (results.size() >= 20) break;
        }

        return results;
    }

    /**
     * @brief DHT 网络搜索（使用公共 DHT 爬虫）
     */
    QList<SearchResultItem> search_dht_network(const QString& keyword) {
        QList<SearchResultItem> results;

        // 使用公共 DHT 搜索 API
        // 例如：BTDigg, 种子搜索等

        // 这里提供一个示例 URL 格式
        QString search_url = QString("https://btdig.com/search?q=%1").arg(keyword);

        QByteArray data = http_get(search_url);
        if (data.isEmpty()) {
            return results;
        }

        QString html = QString::fromUtf8(data);

        // BTDigg 返回的 HTML 包含搜索结果
        // 解析结果...

        return results;
    }

    /**
     * @brief 搜索 HTTP 资源（使用 Google Custom Search 或类似 API）
     */
    QList<SearchResultItem> search_http_resources(const QString& keyword) {
        QList<SearchResultItem> results;

        // 使用文件类型限定搜索
        // 例如：filetype:pdf keyword

        // 这里可以集成各种搜索引擎 API
        // Google Custom Search API, Bing Search API 等

        return results;
    }

    /**
     * @brief 搜索网盘资源
     */
    QList<SearchResultItem> search_cloud_resources(const QString& keyword) {
        QList<SearchResultItem> results;

        // 搜索各大网盘的公开分享链接
        // 可以使用网盘资源索引站

        // 示例：使用网盘搜索网站
        // QString search_url = QString("https://www.sopan.cn/search/%1").arg(keyword);

        return results;
    }
};

// ============================================================================
// SearchService 实现
// ============================================================================

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

        // 限制结果数量
        if (results.size() > options.max_results) {
            results = results.mid(0, options.max_results);
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
        if (callback && !p_impl_->cancelled) {
            callback(results);
        }
    });
}

void SearchService::search_http(const QString& keyword, const SearchOptions& options, SearchCallback callback) {
    Qt::Concurrent::run([this, keyword, options, callback]() {
        auto results = search_http_sync(keyword, options);
        if (callback && !p_impl_->cancelled) {
            callback(results);
        }
    });
}

void SearchService::search_cloud(const QString& keyword, const SearchOptions& options, SearchCallback callback) {
    Qt::Concurrent::run([this, keyword, options, callback]() {
        auto results = search_cloud_sync(keyword, options);
        if (callback && !p_impl_->cancelled) {
            callback(results);
        }
    });
}

void SearchService::search_ftp(const QString& keyword, const SearchOptions& options, SearchCallback callback) {
    Qt::Concurrent::run([this, keyword, options, callback]() {
        auto results = search_ftp_sync(keyword, options);
        if (callback && !p_impl_->cancelled) {
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

    QList<SearchResultItem> all_results;

    // 并行搜索多个源
    QList<QFuture<QList<SearchResultItem>>> futures;

    // 搜索 BT 天堂
    futures.append(QtConcurrent::run([this, keyword]() {
        return p_impl_->search_bt_tiantang(keyword);
    }));

    // 搜索 Torrent Kitty
    futures.append(QtConcurrent::run([this, keyword]() {
        return p_impl_->search_torrent_kitty(keyword);
    }));

    // 搜索 DHT 网络
    futures.append(QtConcurrent::run([this, keyword]() {
        return p_impl_->search_dht_network(keyword);
    }));

    // 等待所有搜索完成
    for (auto& future : futures) {
        future.waitForFinished();
        all_results.append(future.result());
    }

    // 去重（基于磁力链接）
    QSet<QString> seen_urls;
    QList<SearchResultItem> unique_results;
    for (const auto& item : all_results) {
        if (!seen_urls.contains(item.url)) {
            seen_urls.insert(item.url);
            unique_results.append(item);
        }
    }

    return unique_results;
}

QList<SearchResultItem> SearchService::search_http_sync(const QString& keyword, const SearchOptions& options) {
    Q_UNUSED(options);

    // 搜索 HTTP 资源
    return p_impl_->search_http_resources(keyword);
}

QList<SearchResultItem> SearchService::search_cloud_sync(const QString& keyword, const SearchOptions& options) {
    Q_UNUSED(options);

    // 搜索网盘资源
    return p_impl_->search_cloud_resources(keyword);
}

QList<SearchResultItem> SearchService::search_ftp_sync(const QString& keyword, const SearchOptions& options) {
    Q_UNUSED(keyword);
    Q_UNUSED(options);

    // FTP 搜索通常需要访问特定的 FTP 服务器
    // 这里返回空列表
    return {};
}

} // namespace falcon::desktop
