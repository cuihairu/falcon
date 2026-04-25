/**
 * @file storage_service.hpp
 * @brief 云存储服务桥接层 - 连接 Desktop UI 与 libfalcon-storage
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <QObject>
#include <QString>
#include <QMap>
#include <QVariantList>
#include <QVariantMap>
#include <QSharedPointer>
#include <QList>
#include <functional>
#include <QEventLoop>
#include <QTimer>

namespace falcon::desktop {

/**
 * @brief 云存储配置
 */
struct CloudStorageConfig {
    QString name;
    QString protocol;       // s3, oss, cos, kodo, upyun
    QString endpoint;
    QString access_key;
    QString secret_key;
    QString region;
    QString bucket;
    bool is_connected = false;

    bool operator==(const CloudStorageConfig& other) const {
        return name == other.name &&
               protocol == other.protocol &&
               endpoint == other.endpoint &&
               access_key == other.access_key &&
               secret_key == other.secret_key &&
               region == other.region &&
               bucket == other.bucket &&
               is_connected == other.is_connected;
    }
};

/**
 * @brief 远程资源信息
 */
struct RemoteResourceInfo {
    QString name;
    QString path;
    qint64 size = 0;
    QString type;           // file, directory, symlink
    QString modified_time;
    QString mime_type;
    QString etag;
    bool is_hidden = false;
};

/**
 * @brief 存储服务 - 云存储操作接口
 */
class StorageService : public QObject {
    Q_OBJECT

public:
    explicit StorageService(QObject* parent = nullptr);
    ~StorageService() override;

    // 连接管理
    bool connect_storage(const CloudStorageConfig& config);
    void disconnect_storage(const QString& config_name);
    bool is_connected(const QString& config_name) const;
    QStringList connected_storages() const;

    // 配置管理
    void save_config(const CloudStorageConfig& config);
    QList<CloudStorageConfig> load_configs();
    void remove_config(const QString& name);

    // 文件浏览
    using ResourceListCallback = std::function<void(const QList<RemoteResourceInfo>&)>;
    void list_directory(const QString& config_name, const QString& path,
                       ResourceListCallback callback);

    RemoteResourceInfo get_resource_info(const QString& config_name, const QString& path);

    // 文件操作
    bool create_directory(const QString& config_name, const QString& path);
    bool remove_resource(const QString& config_name, const QString& path, bool recursive = false);
    bool rename_resource(const QString& config_name, const QString& old_path, const QString& new_path);
    bool copy_resource(const QString& config_name, const QString& source_path, const QString& dest_path);

    // 上传文件
    using UploadCallback = std::function<void(bool success, const QString& message)>;
    void upload_file(const QString& config_name, const QString& local_path,
                     const QString& remote_path, UploadCallback callback);

    // 配额信息
    QMap<QString, quint64> get_quota_info(const QString& config_name);

    // 下载请求
    void request_download(const QString& config_name, const QString& remote_path,
                         const QString& local_path);

signals:
    void connected(const QString& config_name);
    void disconnected(const QString& config_name);
    void error(const QString& config_name, const QString& message);
    void directory_loaded(const QString& config_name, const QString& path, const QList<RemoteResourceInfo>& resources);
    void download_requested(const QString& url, const QString& local_path);

private:
    struct Impl;
    QSharedPointer<Impl> p_impl_;
};

} // namespace falcon::desktop
