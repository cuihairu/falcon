/**
 * @file local_store.hpp
 * @brief 本地数据服务：账号 / 好友 / 分享组 / 配对设备 / NAS 节点 / 分享记录
 *
 * 这些能力属于产品「后续阶段」，daemon 当前无对应 RPC。本服务把域数据落到
 * 本机（QStandardPaths::AppConfigLocation/local_store.json，QSaveFile 原子写），
 * 提供完整的 CRUD，使界面不再是硬编码假数据，而是真实可读可写、重启仍在。
 *
 * 说明：配对码 / 传送码的生成与校验也在本机闭环，用于单机演示完整流程；
 * 真正的跨设备中继需后端，UI 会明确标注这一边界，不冒充联网能力。
 *
 * @author Falcon Team
 * @date 2026-11-27
 */

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace falcon::desktop {

/** 本地账号资料 */
struct LocalProfile {
    QString id;
    QString nickname;
    QString phone;
    bool logged_in = false;
    qint64 updated_at = 0;
};

/** 五项跨设备同步开关 */
struct SyncSettings {
    bool clipboard = true;
    bool dictionary = true;
    bool phrases = true;
    bool download_history = false;
    bool airdrop = true;
};

/** 已配对设备 */
struct PairedDevice {
    QString id;
    QString name;
    QString platform;
    QString code;
    qint64 paired_at = 0;
    bool online = true;
};

/** NAS / 路由节点 */
struct NasNode {
    QString id;
    QString name;
    QString protocol;
    QString endpoint;
    QString access_key;
    QString secret_key;
    QString region;
    QStringList capabilities;
    bool online = false;
    qint64 added_at = 0;
};

/** 好友 */
struct Friend {
    QString id;
    QString name;
    int device_count = 0;
    QString note;
    qint64 added_at = 0;
};

/** 好友请求 */
struct FriendRequest {
    QString id;
    QString name;
    QString message;
    qint64 created_at = 0;
};

/** 分享组 */
struct ShareGroup {
    QString id;
    QString name;
    QStringList members;
    int device_count = 0;
    bool active = true;
    qint64 created_at = 0;
};

/** 分享 / 传送记录 */
struct ShareRecord {
    QString id;
    QString content;
    QString target;
    QString method;
    QString status;
    QString size;
    qint64 created_at = 0;
};

/** 一次性码（本地闭环） */
struct PairingCode {
    QString code;
    QString purpose;
    qint64 expires_at = 0;
};

/** epoch 秒 → 本地化显示串；0 返回「—」 */
QString format_timestamp(qint64 epoch_seconds);

/** 生成带前缀的短 id（时间 + 随机数，碰撞概率可忽略） */
QString make_id(const QString& prefix);

/**
 * @brief 单例本地存储。所有 public 方法线程安全（内部互斥锁）。
 */
class LocalStore
{
public:
    static LocalStore& instance();

    LocalStore(const LocalStore&) = delete;
    LocalStore& operator=(const LocalStore&) = delete;

    // ---- 账号 ----
    LocalProfile profile() const;
    void save_profile(const LocalProfile& profile);
    bool login(const QString& phone, const QString& code);
    void logout();

    // ---- 同步开关 ----
    SyncSettings sync_settings() const;
    void set_sync_enabled(const QString& key, bool enabled);

    // ---- 配对设备 ----
    QVector<PairedDevice> paired_devices() const;
    bool add_paired_device(const PairedDevice& device);
    bool remove_paired_device(const QString& id);

    // ---- NAS 节点 ----
    QVector<NasNode> nas_nodes() const;
    bool add_nas_node(const NasNode& node);
    bool remove_nas_node(const QString& id);

    // ---- 好友 ----
    QVector<Friend> friends() const;
    bool add_friend(const Friend& friend_item);
    bool remove_friend(const QString& id);

    // ---- 好友请求 ----
    QVector<FriendRequest> friend_requests() const;
    bool add_friend_request(const FriendRequest& request);
    bool resolve_friend_request(const QString& id, bool accept);

    // ---- 分享组 ----
    QVector<ShareGroup> share_groups() const;
    bool add_share_group(const ShareGroup& group);
    bool remove_share_group(const QString& id);

    // ---- 分享记录 ----
    QVector<ShareRecord> share_records() const;
    bool add_share_record(const ShareRecord& record);
    bool clear_share_records();

    // ---- 一次性码 ----
    /** 生成 6 位码（xxx xxx 形式），ttl 秒后失效 */
    QString issue_code(const QString& purpose, int ttl_seconds = 120);
    /** 校验码是否有效且未过期；purpose 非空时同时匹配用途 */
    bool verify_code(const QString& code, const QString& purpose = QString()) const;
    /** 校验并作废（配对成功后调用） */
    bool consume_code(const QString& code, const QString& purpose = QString());

    QString file_path() const { return file_path_; }

private:
    LocalStore();

    void load();
    void save() const;
    QJsonArray read_array(const QString& key) const;
    void write_array(const QString& key, const QJsonArray& array);

    QString file_path_;
    QJsonObject root_;
    mutable QMutex mutex_;
};

} // namespace falcon::desktop
