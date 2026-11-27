/**
 * @file local_store.cpp
 * @brief LocalStore 核心：初始化 / 原子落盘 / 账号 / 同步开关 / 一次性码
 * @author Falcon Team
 * @date 2026-11-27
 */

#include "local_store.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

namespace falcon::desktop {

namespace {

constexpr int kStoreVersion = 1;
constexpr const char* kFileName = "local_store.json";

QString normalized_code(const QString& code)
{
    QString out;
    for (const QChar& ch : code) {
        if (ch.isDigit()) {
            out += ch;
        }
    }
    return out;
}

} // namespace

QString format_timestamp(qint64 epoch_seconds)
{
    if (epoch_seconds <= 0) {
        return QStringLiteral("—");
    }
    return QDateTime::fromSecsSinceEpoch(epoch_seconds).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString make_id(const QString& prefix)
{
    const qint64 ms = QDateTime::currentMSecsSinceEpoch();
    const int rnd = QRandomGenerator::global()->bounded(100000, 999999);
    return prefix + QString::number(ms) + QStringLiteral("_") + QString::number(rnd);
}

LocalStore::LocalStore()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    file_path_ = QDir(base).filePath(QStringLiteral(kFileName));
    load();
}

LocalStore& LocalStore::instance()
{
    static LocalStore store;
    return store;
}

void LocalStore::load()
{
    QFile file(file_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        root_ = QJsonObject{{QStringLiteral("version"), kStoreVersion}};
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    root_ = doc.isObject() ? doc.object() : QJsonObject{{QStringLiteral("version"), kStoreVersion}};
    if (!root_.contains(QStringLiteral("version"))) {
        root_[QStringLiteral("version")] = kStoreVersion;
    }
}

void LocalStore::save() const
{
    const QFileInfo info(file_path_);
    QDir().mkpath(info.absolutePath());

    QSaveFile file(file_path_);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(root_).toJson(QJsonDocument::Indented));
    file.commit();
}

QJsonArray LocalStore::read_array(const QString& key) const
{
    const QJsonValue value = root_.value(key);
    return value.isArray() ? value.toArray() : QJsonArray();
}

void LocalStore::write_array(const QString& key, const QJsonArray& array)
{
    root_[key] = array;
    save();
}

// ============================================================
// 账号
// ============================================================
LocalProfile LocalStore::profile() const
{
    QMutexLocker locker(&mutex_);
    const QJsonObject obj = root_.value(QStringLiteral("profile")).toObject();
    LocalProfile p;
    p.id = obj.value(QStringLiteral("id")).toString();
    p.nickname = obj.value(QStringLiteral("nickname")).toString();
    p.phone = obj.value(QStringLiteral("phone")).toString();
    p.logged_in = obj.value(QStringLiteral("logged_in")).toBool(false);
    p.updated_at = static_cast<qint64>(obj.value(QStringLiteral("updated_at")).toDouble());
    if (p.id.isEmpty()) {
        p.id = QStringLiteral("8f2c9a1d");
    }
    if (p.nickname.isEmpty()) {
        p.nickname = QStringLiteral("Falcon 用户");
    }
    return p;
}

void LocalStore::save_profile(const LocalProfile& profile)
{
    QMutexLocker locker(&mutex_);
    QJsonObject obj;
    obj[QStringLiteral("id")] = profile.id.isEmpty() ? QStringLiteral("8f2c9a1d") : profile.id;
    obj[QStringLiteral("nickname")] = profile.nickname;
    obj[QStringLiteral("phone")] = profile.phone;
    obj[QStringLiteral("logged_in")] = profile.logged_in;
    obj[QStringLiteral("updated_at")] = static_cast<double>(
        profile.updated_at > 0 ? profile.updated_at : QDateTime::currentSecsSinceEpoch());
    root_[QStringLiteral("profile")] = obj;
    save();
}

bool LocalStore::login(const QString& phone, const QString& code)
{
    if (phone.trimmed().isEmpty() || !verify_code(code, QStringLiteral("login"))) {
        return false;
    }
    consume_code(code, QStringLiteral("login"));
    LocalProfile p = profile();
    p.phone = phone.trimmed();
    p.logged_in = true;
    p.updated_at = QDateTime::currentSecsSinceEpoch();
    save_profile(p);
    return true;
}

void LocalStore::logout()
{
    LocalProfile p = profile();
    p.logged_in = false;
    p.updated_at = QDateTime::currentSecsSinceEpoch();
    save_profile(p);
}

// ============================================================
// 同步开关
// ============================================================
SyncSettings LocalStore::sync_settings() const
{
    QMutexLocker locker(&mutex_);
    const QJsonObject obj = root_.value(QStringLiteral("sync")).toObject();
    SyncSettings s;
    s.clipboard = obj.value(QStringLiteral("clipboard")).toBool(true);
    s.dictionary = obj.value(QStringLiteral("dictionary")).toBool(true);
    s.phrases = obj.value(QStringLiteral("phrases")).toBool(true);
    s.download_history = obj.value(QStringLiteral("download_history")).toBool(false);
    s.airdrop = obj.value(QStringLiteral("airdrop")).toBool(true);
    return s;
}

void LocalStore::set_sync_enabled(const QString& key, bool enabled)
{
    QMutexLocker locker(&mutex_);
    QJsonObject obj = root_.value(QStringLiteral("sync")).toObject();
    obj[key] = enabled;
    root_[QStringLiteral("sync")] = obj;
    save();
}

// ============================================================
// 一次性码（本地闭环：生成 / 校验 / 作废）
// ============================================================
QString LocalStore::issue_code(const QString& purpose, int ttl_seconds)
{
    QString raw;
    for (int i = 0; i < 6; ++i) {
        raw += QString::number(QRandomGenerator::global()->bounded(10));
    }
    const QString code = raw.mid(0, 3) + QStringLiteral(" ") + raw.mid(3, 3);

    QMutexLocker locker(&mutex_);
    QJsonArray codes = read_array(QStringLiteral("codes"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QJsonArray fresh;
    for (const QJsonValue& value : codes) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("expires_at")).toDouble() > now) {
            fresh.append(item);
        }
    }
    QJsonObject item;
    item[QStringLiteral("code")] = code;
    item[QStringLiteral("purpose")] = purpose;
    item[QStringLiteral("expires_at")] = static_cast<double>(now + ttl_seconds);
    fresh.append(item);
    write_array(QStringLiteral("codes"), fresh);
    return code;
}

bool LocalStore::verify_code(const QString& code, const QString& purpose) const
{
    QMutexLocker locker(&mutex_);
    const QString needle = normalized_code(code);
    if (needle.size() != 6) {
        return false;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QJsonValue& value : read_array(QStringLiteral("codes"))) {
        const QJsonObject item = value.toObject();
        if (normalized_code(item.value(QStringLiteral("code")).toString()) != needle) {
            continue;
        }
        if (item.value(QStringLiteral("expires_at")).toDouble() <= now) {
            continue;
        }
        if (!purpose.isEmpty() && item.value(QStringLiteral("purpose")).toString() != purpose) {
            continue;
        }
        return true;
    }
    return false;
}

bool LocalStore::consume_code(const QString& code, const QString& purpose)
{
    if (!verify_code(code, purpose)) {
        return false;
    }
    QMutexLocker locker(&mutex_);
    const QString needle = normalized_code(code);
    QJsonArray fresh;
    for (const QJsonValue& value : read_array(QStringLiteral("codes"))) {
        const QJsonObject item = value.toObject();
        if (normalized_code(item.value(QStringLiteral("code")).toString()) != needle) {
            fresh.append(item);
        }
    }
    write_array(QStringLiteral("codes"), fresh);
    return true;
}

} // namespace falcon::desktop
