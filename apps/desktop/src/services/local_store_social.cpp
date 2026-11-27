/**
 * @file local_store_social.cpp
 * @brief LocalStore 域数据：配对设备 / NAS 节点 / 好友 / 请求 / 分享组 / 记录
 * @author Falcon Team
 * @date 2026-11-27
 */

#include "local_store.hpp"

#include <QDateTime>
#include <QJsonValue>

namespace falcon::desktop {

namespace {

QStringList json_to_string_list(const QJsonValue& value)
{
    QStringList out;
    if (!value.isArray()) {
        return out;
    }
    for (const QJsonValue& item : value.toArray()) {
        out.append(item.toString());
    }
    return out;
}

QJsonArray string_list_to_json(const QStringList& list)
{
    QJsonArray array;
    for (const QString& item : list) {
        array.append(item);
    }
    return array;
}

QString json_string(const QJsonObject& obj, const char* key)
{
    return obj.value(QStringLiteral(key)).toString();
}

qint64 json_epoch(const QJsonObject& obj, const char* key)
{
    return static_cast<qint64>(obj.value(QStringLiteral(key)).toDouble());
}

} // namespace

// ============================================================
// 配对设备
// ============================================================
QVector<PairedDevice> LocalStore::paired_devices() const
{
    QMutexLocker locker(&mutex_);
    QVector<PairedDevice> out;
    for (const QJsonValue& value : read_array(QStringLiteral("devices"))) {
        const QJsonObject obj = value.toObject();
        PairedDevice d;
        d.id = json_string(obj, "id");
        d.name = json_string(obj, "name");
        d.platform = json_string(obj, "platform");
        d.code = json_string(obj, "code");
        d.paired_at = json_epoch(obj, "paired_at");
        d.online = obj.value(QStringLiteral("online")).toBool(true);
        out.append(d);
    }
    return out;
}

bool LocalStore::add_paired_device(const PairedDevice& device)
{
    QMutexLocker locker(&mutex_);
    QJsonArray devices = read_array(QStringLiteral("devices"));
    for (const QJsonValue& value : devices) {
        if (value.toObject().value(QStringLiteral("id")).toString() == device.id) {
            return false;
        }
    }
    QJsonObject obj;
    obj[QStringLiteral("id")] = device.id.isEmpty() ? make_id(QStringLiteral("dev_")) : device.id;
    obj[QStringLiteral("name")] = device.name;
    obj[QStringLiteral("platform")] = device.platform;
    obj[QStringLiteral("code")] = device.code;
    obj[QStringLiteral("paired_at")] = static_cast<double>(
        device.paired_at > 0 ? device.paired_at : QDateTime::currentSecsSinceEpoch());
    obj[QStringLiteral("online")] = device.online;
    devices.append(obj);
    write_array(QStringLiteral("devices"), devices);
    return true;
}

bool LocalStore::remove_paired_device(const QString& id)
{
    QMutexLocker locker(&mutex_);
    QJsonArray devices = read_array(QStringLiteral("devices"));
    QJsonArray fresh;
    bool removed = false;
    for (const QJsonValue& value : devices) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id) {
            removed = true;
        } else {
            fresh.append(value);
        }
    }
    if (removed) {
        write_array(QStringLiteral("devices"), fresh);
    }
    return removed;
}

// ============================================================
// NAS 节点
// ============================================================
QVector<NasNode> LocalStore::nas_nodes() const
{
    QMutexLocker locker(&mutex_);
    QVector<NasNode> out;
    for (const QJsonValue& value : read_array(QStringLiteral("nodes"))) {
        const QJsonObject obj = value.toObject();
        NasNode n;
        n.id = json_string(obj, "id");
        n.name = json_string(obj, "name");
        n.protocol = json_string(obj, "protocol");
        n.endpoint = json_string(obj, "endpoint");
        n.access_key = json_string(obj, "access_key");
        n.secret_key = json_string(obj, "secret_key");
        n.region = json_string(obj, "region");
        n.capabilities = json_to_string_list(obj.value(QStringLiteral("capabilities")));
        n.online = obj.value(QStringLiteral("online")).toBool(false);
        n.added_at = json_epoch(obj, "added_at");
        out.append(n);
    }
    return out;
}

bool LocalStore::add_nas_node(const NasNode& node)
{
    QMutexLocker locker(&mutex_);
    QJsonArray nodes = read_array(QStringLiteral("nodes"));
    for (const QJsonValue& value : nodes) {
        if (value.toObject().value(QStringLiteral("id")).toString() == node.id) {
            return false;
        }
    }
    QJsonObject obj;
    obj[QStringLiteral("id")] = node.id.isEmpty() ? make_id(QStringLiteral("node_")) : node.id;
    obj[QStringLiteral("name")] = node.name;
    obj[QStringLiteral("protocol")] = node.protocol;
    obj[QStringLiteral("endpoint")] = node.endpoint;
    obj[QStringLiteral("access_key")] = node.access_key;
    obj[QStringLiteral("secret_key")] = node.secret_key;
    obj[QStringLiteral("region")] = node.region;
    obj[QStringLiteral("capabilities")] = string_list_to_json(node.capabilities);
    obj[QStringLiteral("online")] = node.online;
    obj[QStringLiteral("added_at")] = static_cast<double>(
        node.added_at > 0 ? node.added_at : QDateTime::currentSecsSinceEpoch());
    nodes.append(obj);
    write_array(QStringLiteral("nodes"), nodes);
    return true;
}

bool LocalStore::remove_nas_node(const QString& id)
{
    QMutexLocker locker(&mutex_);
    QJsonArray nodes = read_array(QStringLiteral("nodes"));
    QJsonArray fresh;
    bool removed = false;
    for (const QJsonValue& value : nodes) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id) {
            removed = true;
        } else {
            fresh.append(value);
        }
    }
    if (removed) {
        write_array(QStringLiteral("nodes"), fresh);
    }
    return removed;
}

// ============================================================
// 好友
// ============================================================
QVector<Friend> LocalStore::friends() const
{
    QMutexLocker locker(&mutex_);
    QVector<Friend> out;
    for (const QJsonValue& value : read_array(QStringLiteral("friends"))) {
        const QJsonObject obj = value.toObject();
        Friend f;
        f.id = json_string(obj, "id");
        f.name = json_string(obj, "name");
        f.device_count = static_cast<int>(obj.value(QStringLiteral("device_count")).toInt());
        f.note = json_string(obj, "note");
        f.added_at = json_epoch(obj, "added_at");
        out.append(f);
    }
    return out;
}

bool LocalStore::add_friend(const Friend& friend_item)
{
    QMutexLocker locker(&mutex_);
    QJsonArray friends = read_array(QStringLiteral("friends"));
    for (const QJsonValue& value : friends) {
        if (value.toObject().value(QStringLiteral("name")).toString() == friend_item.name) {
            return false;
        }
    }
    QJsonObject obj;
    obj[QStringLiteral("id")] = friend_item.id.isEmpty() ? make_id(QStringLiteral("fr_")) : friend_item.id;
    obj[QStringLiteral("name")] = friend_item.name;
    obj[QStringLiteral("device_count")] = friend_item.device_count;
    obj[QStringLiteral("note")] = friend_item.note;
    obj[QStringLiteral("added_at")] = static_cast<double>(
        friend_item.added_at > 0 ? friend_item.added_at : QDateTime::currentSecsSinceEpoch());
    friends.append(obj);
    write_array(QStringLiteral("friends"), friends);
    return true;
}

bool LocalStore::remove_friend(const QString& id)
{
    QMutexLocker locker(&mutex_);
    QJsonArray friends = read_array(QStringLiteral("friends"));
    QJsonArray fresh;
    bool removed = false;
    for (const QJsonValue& value : friends) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id) {
            removed = true;
        } else {
            fresh.append(value);
        }
    }
    if (removed) {
        write_array(QStringLiteral("friends"), fresh);
    }
    return removed;
}

// ============================================================
// 好友请求
// ============================================================
QVector<FriendRequest> LocalStore::friend_requests() const
{
    QMutexLocker locker(&mutex_);
    QVector<FriendRequest> out;
    for (const QJsonValue& value : read_array(QStringLiteral("requests"))) {
        const QJsonObject obj = value.toObject();
        FriendRequest r;
        r.id = json_string(obj, "id");
        r.name = json_string(obj, "name");
        r.message = json_string(obj, "message");
        r.created_at = json_epoch(obj, "created_at");
        out.append(r);
    }
    return out;
}

bool LocalStore::add_friend_request(const FriendRequest& request)
{
    QMutexLocker locker(&mutex_);
    QJsonArray requests = read_array(QStringLiteral("requests"));
    QJsonObject obj;
    obj[QStringLiteral("id")] = request.id.isEmpty() ? make_id(QStringLiteral("req_")) : request.id;
    obj[QStringLiteral("name")] = request.name;
    obj[QStringLiteral("message")] = request.message;
    obj[QStringLiteral("created_at")] = static_cast<double>(
        request.created_at > 0 ? request.created_at : QDateTime::currentSecsSinceEpoch());
    requests.append(obj);
    write_array(QStringLiteral("requests"), requests);
    return true;
}

bool LocalStore::resolve_friend_request(const QString& id, bool accept)
{
    QMutexLocker locker(&mutex_);
    QJsonArray requests = read_array(QStringLiteral("requests"));
    QString name;
    QJsonArray fresh;
    for (const QJsonValue& value : requests) {
        const QJsonObject obj = value.toObject();
        if (obj.value(QStringLiteral("id")).toString() == id) {
            name = obj.value(QStringLiteral("name")).toString();
        } else {
            fresh.append(value);
        }
    }
    if (name.isEmpty()) {
        return false;
    }
    write_array(QStringLiteral("requests"), fresh);
    if (accept) {
        // 注意：此处已持锁，不能调用同样加锁的 add_friend（QMutex 非递归）
        QJsonArray friends = read_array(QStringLiteral("friends"));
        QJsonObject f;
        f[QStringLiteral("id")] = make_id(QStringLiteral("fr_"));
        f[QStringLiteral("name")] = name;
        f[QStringLiteral("device_count")] = 0;
        f[QStringLiteral("note")] = QStringLiteral("通过匹配码添加");
        f[QStringLiteral("added_at")] = static_cast<double>(QDateTime::currentSecsSinceEpoch());
        friends.append(f);
        write_array(QStringLiteral("friends"), friends);
    }
    return true;
}

// ============================================================
// 分享组
// ============================================================
QVector<ShareGroup> LocalStore::share_groups() const
{
    QMutexLocker locker(&mutex_);
    QVector<ShareGroup> out;
    for (const QJsonValue& value : read_array(QStringLiteral("groups"))) {
        const QJsonObject obj = value.toObject();
        ShareGroup g;
        g.id = json_string(obj, "id");
        g.name = json_string(obj, "name");
        g.members = json_to_string_list(obj.value(QStringLiteral("members")));
        g.device_count = static_cast<int>(obj.value(QStringLiteral("device_count")).toInt());
        g.active = obj.value(QStringLiteral("active")).toBool(true);
        g.created_at = json_epoch(obj, "created_at");
        out.append(g);
    }
    return out;
}

bool LocalStore::add_share_group(const ShareGroup& group)
{
    QMutexLocker locker(&mutex_);
    QJsonArray groups = read_array(QStringLiteral("groups"));
    QJsonObject obj;
    obj[QStringLiteral("id")] = group.id.isEmpty() ? make_id(QStringLiteral("grp_")) : group.id;
    obj[QStringLiteral("name")] = group.name;
    obj[QStringLiteral("members")] = string_list_to_json(group.members);
    obj[QStringLiteral("device_count")] = group.device_count;
    obj[QStringLiteral("active")] = group.active;
    obj[QStringLiteral("created_at")] = static_cast<double>(
        group.created_at > 0 ? group.created_at : QDateTime::currentSecsSinceEpoch());
    groups.append(obj);
    write_array(QStringLiteral("groups"), groups);
    return true;
}

bool LocalStore::remove_share_group(const QString& id)
{
    QMutexLocker locker(&mutex_);
    QJsonArray groups = read_array(QStringLiteral("groups"));
    QJsonArray fresh;
    bool removed = false;
    for (const QJsonValue& value : groups) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id) {
            removed = true;
        } else {
            fresh.append(value);
        }
    }
    if (removed) {
        write_array(QStringLiteral("groups"), fresh);
    }
    return removed;
}

// ============================================================
// 分享记录
// ============================================================
QVector<ShareRecord> LocalStore::share_records() const
{
    QMutexLocker locker(&mutex_);
    QVector<ShareRecord> out;
    for (const QJsonValue& value : read_array(QStringLiteral("records"))) {
        const QJsonObject obj = value.toObject();
        ShareRecord r;
        r.id = json_string(obj, "id");
        r.content = json_string(obj, "content");
        r.target = json_string(obj, "target");
        r.method = json_string(obj, "method");
        r.status = json_string(obj, "status");
        r.size = json_string(obj, "size");
        r.created_at = json_epoch(obj, "created_at");
        out.append(r);
    }
    return out;
}

bool LocalStore::add_share_record(const ShareRecord& record)
{
    QMutexLocker locker(&mutex_);
    QJsonArray records = read_array(QStringLiteral("records"));
    QJsonObject obj;
    obj[QStringLiteral("id")] = record.id.isEmpty() ? make_id(QStringLiteral("rec_")) : record.id;
    obj[QStringLiteral("content")] = record.content;
    obj[QStringLiteral("target")] = record.target;
    obj[QStringLiteral("method")] = record.method;
    obj[QStringLiteral("status")] = record.status;
    obj[QStringLiteral("size")] = record.size;
    obj[QStringLiteral("created_at")] = static_cast<double>(
        record.created_at > 0 ? record.created_at : QDateTime::currentSecsSinceEpoch());
    records.append(obj);
    write_array(QStringLiteral("records"), records);
    return true;
}

bool LocalStore::clear_share_records()
{
    QMutexLocker locker(&mutex_);
    write_array(QStringLiteral("records"), QJsonArray());
    return true;
}

} // namespace falcon::desktop
