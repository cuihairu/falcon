/**
 * @file storage_service.cpp
 * @brief 云存储服务桥接层实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "storage_service.hpp"
#include <QSettings>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QMutexLocker>
#include <QMutex>

// 由于这是 Qt 应用，我们通过 JSON-RPC 或直接调用访问 libfalcon-storage
// 这里提供一个简化的模拟实现

namespace falcon::desktop {

struct StorageService::Impl {
    QMap<QString, CloudStorageConfig> configs;
    QMap<QString, bool> connected;
    QMutex mutex;
    QMap<QString, QList<RemoteResourceInfo>> cache;

    // 模拟数据生成器
    QList<RemoteResourceInfo> generate_mock_resources(const QString& path) {
        QList<RemoteResourceInfo> resources;

        if (path == "/" || path.isEmpty()) {
            // 根目录
            RemoteResourceInfo dir1;
            dir1.name = "documents";
            dir1.path = "/documents";
            dir1.type = "directory";
            dir1.size = 0;
            dir1.modified_time = "2025-12-21 10:00:00";
            resources.append(dir1);

            RemoteResourceInfo dir2;
            dir2.name = "images";
            dir2.path = "/images";
            dir2.type = "directory";
            dir2.size = 0;
            dir2.modified_time = "2025-12-20 15:30:00";
            resources.append(dir2);

            RemoteResourceInfo file1;
            file1.name = "readme.txt";
            file1.path = "/readme.txt";
            file1.type = "file";
            file1.size = 1024;
            file1.modified_time = "2025-12-19 09:15:00";
            file1.mime_type = "text/plain";
            resources.append(file1);
        } else if (path == "/documents") {
            RemoteResourceInfo file1;
            file1.name = "report.pdf";
            file1.path = "/documents/report.pdf";
            file1.type = "file";
            file1.size = 2048576;
            file1.modified_time = "2025-12-18 14:20:00";
            file1.mime_type = "application/pdf";
            resources.append(file1);

            RemoteResourceInfo file2;
            file2.name = "notes.txt";
            file2.path = "/documents/notes.txt";
            file2.type = "file";
            file2.size = 512;
            file2.modified_time = "2025-12-17 11:45:00";
            file2.mime_type = "text/plain";
            resources.append(file2);
        }

        return resources;
    }
};

StorageService::StorageService(QObject* parent)
    : QObject(parent)
    , p_impl_(std::make_shared<Impl>()) {

    // 加载已保存的配置
    auto configs = load_configs();
    for (const auto& config : configs) {
        p_impl_->configs[config.name] = config;
        p_impl_->connected[config.name] = false;
    }
}

StorageService::~StorageService() = default;

bool StorageService::connect_storage(const CloudStorageConfig& config) {
    QMutexLocker locker(&p_impl_->mutex);

    // 保存配置
    p_impl_->configs[config.name] = config;

    // 在实际实现中，这里会调用 libfalcon-storage 的 S3Browser::connect()
    // 由于跨语言边界，我们通过以下方式之一：
    // 1. JSON-RPC 调用 falcon-daemon
    // 2. 直接链接 libfalcon-storage 库
    // 3. 使用 C++/CLI 或类似的桥接技术

    // 模拟连接成功
    p_impl_->connected[config.name] = true;

    emit connected(config.name);
    return true;
}

void StorageService::disconnect_storage(const QString& config_name) {
    QMutexLocker locker(&p_impl_->mutex);

    p_impl_->connected[config_name] = false;
    p_impl_->cache.remove(config_name);

    emit disconnected(config_name);
}

bool StorageService::is_connected(const QString& config_name) const {
    return p_impl_->connected.value(config_name, false);
}

QStringList StorageService::connected_storages() const {
    QStringList result;
    for (auto it = p_impl_->connected.begin(); it != p_impl_->connected.end(); ++it) {
        if (it.value()) {
            result.append(it.key());
        }
    }
    return result;
}

void StorageService::save_config(const CloudStorageConfig& config) {
    QSettings settings;
    settings.beginGroup("CloudStorage");

    // 获取现有配置列表
    QList<QVariantMap> config_list;
    QByteArray array_data = settings.value("configs").toByteArray();
    if (!array_data.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(array_data);
        if (doc.isArray()) {
            QJsonArray array = doc.array();
            for (const auto& val : array) {
                config_list.append(val.toObject().toVariantMap());
            }
        }
    }

    // 添加或更新配置
    QVariantMap config_map;
    config_map["name"] = config.name;
    config_map["protocol"] = config.protocol;
    config_map["endpoint"] = config.endpoint;
    config_map["access_key"] = config.access_key;
    config_map["secret_key"] = config.secret_key;
    config_map["region"] = config.region;
    config_map["bucket"] = config.bucket;

    // 检查是否已存在
    bool found = false;
    for (auto& item : config_list) {
        if (item["name"].toString() == config.name) {
            item = config_map;
            found = true;
            break;
        }
    }
    if (!found) {
        config_list.append(config_map);
    }

    // 保存
    QJsonArray array;
    for (const auto& item : config_list) {
        array.append(QJsonObject::fromVariantMap(item));
    }
    settings.setValue("configs", QJsonDocument(array).toJson());
    settings.endGroup();

    // 更新内存中的配置
    p_impl_->configs[config.name] = config;
}

QList<CloudStorageConfig> StorageService::load_configs() {
    QList<CloudStorageConfig> result;

    QSettings settings;
    settings.beginGroup("CloudStorage");
    QByteArray array_data = settings.value("configs").toByteArray();
    settings.endGroup();

    if (array_data.isEmpty()) {
        return result;
    }

    QJsonDocument doc = QJsonDocument::fromJson(array_data);
    if (!doc.isArray()) {
        return result;
    }

    QJsonArray array = doc.array();
    for (const auto& val : array) {
        QJsonObject obj = val.toObject();
        CloudStorageConfig config;
        config.name = obj["name"].toString();
        config.protocol = obj["protocol"].toString();
        config.endpoint = obj["endpoint"].toString();
        config.access_key = obj["access_key"].toString();
        config.secret_key = obj["secret_key"].toString();
        config.region = obj["region"].toString();
        config.bucket = obj["bucket"].toString();
        result.append(config);
    }

    return result;
}

void StorageService::remove_config(const QString& name) {
    QMutexLocker locker(&p_impl_->mutex);

    // 断开连接
    if (p_impl_->connected[name]) {
        disconnect_storage(name);
    }

    // 从内存中移除
    p_impl_->configs.remove(name);

    // 从设置中移除
    QSettings settings;
    settings.beginGroup("CloudStorage");
    QByteArray array_data = settings.value("configs").toByteArray();
    if (!array_data.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(array_data);
        if (doc.isArray()) {
            QJsonArray array = doc.array();
            QJsonArray new_array;
            for (const auto& val : array) {
                QJsonObject obj = val.toObject();
                if (obj["name"].toString() != name) {
                    new_array.append(obj);
                }
            }
            settings.setValue("configs", QJsonDocument(new_array).toJson());
        }
    }
    settings.endGroup();
}

void StorageService::list_directory(const QString& config_name, const QString& path,
                                   ResourceListCallback callback) {
    // 在实际实现中，这会在后台线程中调用 libfalcon-storage
    // 这里使用模拟数据

    QList<RemoteResourceInfo> resources = p_impl_->generate_mock_resources(path);

    // 更新缓存
    QString cache_key = config_name + ":" + path;
    p_impl_->cache[cache_key] = resources;

    if (callback) {
        callback(resources);
    }

    emit directory_loaded(config_name, path, resources);
}

RemoteResourceInfo StorageService::get_resource_info(const QString& config_name, const QString& path) {
    // 在实际实现中，调用 libfalcon-storage 的 get_resource_info()
    RemoteResourceInfo info;
    info.name = path.mid(path.lastIndexOf('/') + 1);
    info.path = path;
    info.type = "file";
    return info;
}

bool StorageService::create_directory(const QString& config_name, const QString& path) {
    // 在实际实现中，调用 libfalcon-storage 的 create_directory()
    emit directory_loaded(config_name, path.left(path.lastIndexOf('/')), {});
    return true;
}

bool StorageService::remove_resource(const QString& config_name, const QString& path, bool recursive) {
    // 在实际实现中，调用 libfalcon-storage 的 remove()
    return true;
}

bool StorageService::rename_resource(const QString& config_name, const QString& old_path, const QString& new_path) {
    // 在实际实现中，调用 libfalcon-storage 的 rename()
    return true;
}

bool StorageService::copy_resource(const QString& config_name, const QString& source_path, const QString& dest_path) {
    // 在实际实现中，调用 libfalcon-storage 的 copy()
    return true;
}

QMap<QString, quint64> StorageService::get_quota_info(const QString& config_name) {
    QMap<QString, quint64> quota;
    quota["used"] = 1073741824;  // 1GB
    quota["total"] = 0;           // 无限制
    return quota;
}

void StorageService::request_download(const QString& config_name, const QString& remote_path,
                                      const QString& local_path) {
    // 构建下载 URL
    CloudStorageConfig config = p_impl_->configs[config_name];

    QString url;
    if (config.protocol == "s3") {
        url = QString("s3://%1/%2").arg(config.bucket, remote_path);
    } else {
        url = QString("%3://%1/%2").arg(config.bucket, remote_path, config.protocol);
    }

    emit download_requested(url, local_path);
}

} // namespace falcon::desktop
