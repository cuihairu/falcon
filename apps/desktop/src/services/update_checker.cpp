/**
 * @file update_checker.cpp
 * @brief GitHub Releases 检查更新服务实现
 * @author Falcon Team
 * @date 2026-09-28
 */

#include "update_checker.hpp"

#include <falcon/version.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace falcon::desktop {

namespace {

// 仓库 releases/latest 端点（GitHub API 强制要求 User-Agent 头）
constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/cuihairu/falcon/releases/latest";
constexpr const char* kReleasesPageUrl =
    "https://github.com/cuihairu/falcon/releases";
constexpr int kTimeoutMs = 15000;

/** 剥 tag 前导 v/V */
QString strip_tag_prefix(const QString& tag)
{
    QString version = tag.trimmed();
    if (!version.isEmpty() && (version.startsWith(QLatin1Char('v'))
                               || version.startsWith(QLatin1Char('V')))) {
        version.remove(0, 1);
    }
    return version;
}

} // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
    , checking_(false)
{
}

UpdateChecker::~UpdateChecker() = default;

QString UpdateChecker::current_version()
{
    return QString::fromLatin1(FALCON_VERSION_STRING);
}

bool UpdateChecker::is_newer_tag_version(const QString& tag)
{
    // 解析失败的 tag（非常规版本号）视作无更新，绝不误报
    const auto latest = falcon::Version::parse(strip_tag_prefix(tag).toStdString());
    const auto current = falcon::Version::parse(FALCON_VERSION_STRING);
    return latest.has_value() && current.has_value() && *latest > *current;
}

void UpdateChecker::check_for_updates()
{
    if (checking_) {
        return;
    }
    checking_ = true;

    QNetworkRequest request{QUrl(QString::fromLatin1(kLatestReleaseUrl))};
    request.setRawHeader("User-Agent", "Falcon-Desktop/" FALCON_VERSION_STRING);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = nam_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        checking_ = false;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit check_failed(reply->errorString());
            return;
        }

        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            emit check_failed(tr("响应不是有效的 JSON"));
            return;
        }
        const auto obj = doc.object();
        const QString tag = obj.value("tag_name").toString();
        if (strip_tag_prefix(tag).isEmpty()) {
            emit check_failed(tr("响应缺少 tag_name 字段"));
            return;
        }
        if (!is_newer_tag_version(tag)) {
            emit up_to_date();
            return;
        }
        QString release_url = obj.value("html_url").toString();
        if (release_url.isEmpty()) {
            release_url = QString::fromLatin1(kReleasesPageUrl);
        }
        emit update_available(strip_tag_prefix(tag), release_url);
    });
}

} // namespace falcon::desktop
