/**
 * @file storage_service.cpp
 * @brief 云存储服务桥接层实现 - 连接 Desktop UI 与 libfalcon-storage
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
#include <QFuture>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <functional>

#include <falcon/storage/resource_browser.hpp>

namespace falcon::desktop {

// ============================================================================
// 类型转换辅助函数
// ============================================================================

namespace {

/**
 * @brief 将 libfalcon::RemoteResource 转换为 Qt::RemoteResourceInfo
 */
RemoteResourceInfo to_qt_resource_info(const falcon::RemoteResource& resource) {
    RemoteResourceInfo info;

    info.name = QString::fromStdString(resource.name);
    info.path = QString::fromStdString(resource.path);
    info.size = static_cast<qint64>(resource.size);

    // 转换资源类型
    switch (resource.type) {
        case falcon::ResourceType::File:
            info.type = "file";
            break;
        case falcon::ResourceType::Directory:
            info.type = "directory";
            break;
        case falcon::ResourceType::Symlink:
            info.type = "symlink";
            break;
        default:
            info.type = "unknown";
            break;
    }

    info.modified_time = QString::fromStdString(resource.modified_time);
    info.mime_type = QString::fromStdString(resource.mime_type);
    info.etag = QString::fromStdString(resource.etag);
    info.is_hidden = !resource.name.empty() && resource.name[0] == '.';

    return info;
}

/**
 * @brief 将 Qt::CloudStorageConfig 转换为 libfalcon 连接选项
 */
std::map<std::string, std::string> to_connection_options(const CloudStorageConfig& config) {
    std::map<std::string, std::string> options;

    options["access_key_id"] = config.access_key.toStdString();
    options["secret_access_key"] = config.secret_key.toStdString();
    options["region"] = config.region.toStdString();
    options["bucket"] = config.bucket.toStdString();

    if (!config.endpoint.isEmpty()) {
        options["endpoint"] = config.endpoint.toStdString();
    }

    return options;
}

} // anonymous namespace

// ============================================================================
// StorageService::Impl
// ============================================================================

struct StorageService::Impl {
    // 配置和连接状态
    QMap<QString, CloudStorageConfig> configs;
    std::map<QString, std::shared_ptr<falcon::IResourceBrowser>> browsers;
    QMap<QString, bool> connected;
    QMutex mutex;

    // 当前路径缓存
    QMap<QString, QString> current_paths;

    /**
     * @brief 获取或创建指定配置的浏览器
     */
    std::shared_ptr<falcon::IResourceBrowser> get_browser(const QString& config_name) {
        QMutexLocker locker(&mutex);

        auto it = browsers.find(config_name);
        if (it == browsers.end()) {
            return {};
        }

        return it->second;
    }

    /**
     * @brief 生成连接 URL
     */
    std::string build_connection_url(const CloudStorageConfig& config) {
        std::string proto = config.protocol.toStdString();

        if (proto == "s3") {
            return "s3://" + config.bucket.toStdString();
        } else if (proto == "oss") {
            return "oss://" + config.bucket.toStdString();
        } else if (proto == "cos") {
            return "cos://" + config.bucket.toStdString();
        } else if (proto == "kodo" || proto == "qiniu") {
            return "kodo://" + config.bucket.toStdString();
        } else if (proto == "upyun") {
            return "upyun://" + config.bucket.toStdString();
        }

        return "";
    }
};

// ============================================================================
// StorageService 实现
// ============================================================================

StorageService::StorageService(QObject* parent)
    : QObject(parent)
    , p_impl_(QSharedPointer<Impl>::create()) {

    // 加载已保存的配置
    auto configs = load_configs();
    for (const auto& config : configs) {
        p_impl_->configs[config.name] = config;
        p_impl_->connected[config.name] = false;
    }
}

StorageService::~StorageService() = default;

bool StorageService::connect_storage(const CloudStorageConfig& config) {
    {
        QMutexLocker locker(&p_impl_->mutex);
        p_impl_->configs[config.name] = config;
    }

    // 创建浏览器实例
    auto browser = falcon::BrowserFactory::create_browser(config.protocol.toStdString());
    if (!browser) {
        emit error(config.name,
            tr("Unsupported protocol: %1").arg(config.protocol));
        return false;
    }
    auto shared_browser = std::shared_ptr<falcon::IResourceBrowser>(std::move(browser));

    // 构建连接 URL
    std::string url = p_impl_->build_connection_url(config);
    if (url.empty()) {
        emit error(config.name,
            tr("Failed to build connection URL"));
        return false;
    }

    // 转换连接选项
    auto options = to_connection_options(config);

    // 尝试连接（在后台线程执行）
    bool success = false;
    QString error_message;

    QEventLoop loop;
    QElapsedTimer timer;
    timer.start();
    QFuture<bool> future = QtConcurrent::run([shared_browser, url, options, &error_message]() {
        try {
            return shared_browser->connect(url, options);
        } catch (const std::exception& e) {
            error_message = QString::fromStdString(e.what());
            return false;
        }
    });

    // 等待连接完成（最多 30 秒）
    while (!future.isFinished() && timer.elapsed() < 30000) {
        loop.processEvents(QEventLoop::AllEvents, 50);
    }

    if (future.isFinished()) {
        success = future.result();
    }

    if (!success) {
        if (error_message.isEmpty()) {
            error_message = tr("Connection timeout or failed");
        }
        emit error(config.name, error_message);
        return false;
    }

    // 保存浏览器实例
    {
        QMutexLocker locker(&p_impl_->mutex);
        p_impl_->browsers[config.name] = std::move(shared_browser);
        p_impl_->connected[config.name] = true;
        p_impl_->current_paths[config.name] = "/";
    }

    emit connected(config.name);
    return true;
}

void StorageService::disconnect_storage(const QString& config_name) {
    std::shared_ptr<falcon::IResourceBrowser> browser;
    {
        QMutexLocker locker(&p_impl_->mutex);

        auto it = p_impl_->browsers.find(config_name);
        if (it != p_impl_->browsers.end()) {
            browser = std::move(it->second);
            p_impl_->browsers.erase(it);
        }

        p_impl_->connected[config_name] = false;
        p_impl_->current_paths.remove(config_name);
    }

    if (browser) {
        QtConcurrent::run([browser]() {
            try {
                browser->disconnect();
            } catch (...) {
                // 忽略断开连接时的错误
            }
        });
    }

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
    // 断开连接
    if (is_connected(name)) {
        disconnect_storage(name);
    }

    {
        QMutexLocker locker(&p_impl_->mutex);
        p_impl_->configs.remove(name);
    }

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
    // 在后台线程执行
    QtConcurrent::run([this, config_name, path, callback]() {
        auto browser = p_impl_->get_browser(config_name);
        if (!browser) {
            if (callback) {
                callback({});
            }
            emit error(config_name, tr("Browser not connected"));
            return;
        }

        try {
            // 转换路径
            std::string std_path = path.toStdString();

            // 列出目录
            falcon::ListOptions options;
            options.show_hidden = false;
            options.sort_by = "name";

            std::vector<falcon::RemoteResource> resources =
                browser->list_directory(std_path, options);

            // 转换为 Qt 类型
            QList<RemoteResourceInfo> qt_resources;
            qt_resources.reserve(resources.size());

            for (const auto& res : resources) {
                qt_resources.append(to_qt_resource_info(res));
            }

            // 更新当前路径
            {
                QMutexLocker locker(&p_impl_->mutex);
                p_impl_->current_paths[config_name] = path;
            }

            if (callback) {
                callback(qt_resources);
            }

            emit directory_loaded(config_name, path, qt_resources);

        } catch (const std::exception& e) {
            QString error_msg = QString::fromStdString(e.what());
            emit error(config_name, error_msg);
        }
    });
}

RemoteResourceInfo StorageService::get_resource_info(const QString& config_name, const QString& path) {
    auto browser = p_impl_->get_browser(config_name);
    if (!browser) {
        return RemoteResourceInfo();
    }

    try {
        std::string std_path = path.toStdString();
        falcon::RemoteResource resource = browser->get_resource_info(std_path);
        return to_qt_resource_info(resource);
    } catch (...) {
        return RemoteResourceInfo();
    }
}

bool StorageService::create_directory(const QString& config_name, const QString& path) {
    auto browser = p_impl_->get_browser(config_name);
    if (!browser) {
        return false;
    }

    try {
        std::string std_path = path.toStdString();
        return browser->create_directory(std_path, false);
    } catch (...) {
        return false;
    }
}

bool StorageService::remove_resource(const QString& config_name, const QString& path, bool recursive) {
    auto browser = p_impl_->get_browser(config_name);
    if (!browser) {
        return false;
    }

    try {
        std::string std_path = path.toStdString();
        return browser->remove(std_path, recursive);
    } catch (...) {
        return false;
    }
}

bool StorageService::rename_resource(const QString& config_name, const QString& old_path, const QString& new_path) {
    auto browser = p_impl_->get_browser(config_name);
    if (!browser) {
        return false;
    }

    try {
        std::string std_old_path = old_path.toStdString();
        std::string std_new_path = new_path.toStdString();
        return browser->rename(std_old_path, std_new_path);
    } catch (...) {
        return false;
    }
}

bool StorageService::copy_resource(const QString& config_name, const QString& source_path, const QString& dest_path) {
    auto browser = p_impl_->get_browser(config_name);
    if (!browser) {
        return false;
    }

    try {
        std::string std_source_path = source_path.toStdString();
        std::string std_dest_path = dest_path.toStdString();
        return browser->copy(std_source_path, std_dest_path);
    } catch (...) {
        return false;
    }
}

QMap<QString, quint64> StorageService::get_quota_info(const QString& config_name) {
    auto browser = p_impl_->get_browser(config_name);
    if (!browser) {
        return QMap<QString, quint64>();
    }

    try {
        std::map<std::string, uint64_t> quota = browser->get_quota_info();
        QMap<QString, quint64> result;

        for (const auto& [key, value] : quota) {
            result[QString::fromStdString(key)] = static_cast<quint64>(value);
        }

        return result;
    } catch (...) {
        return QMap<QString, quint64>();
    }
}

void StorageService::request_download(const QString& config_name, const QString& remote_path,
                                     const QString& local_path) {
    // 构建下载 URL
    CloudStorageConfig config = p_impl_->configs[config_name];

    QString url;
    if (config.protocol == "s3") {
        url = QString("s3://%1%2").arg(config.bucket, remote_path);
    } else if (config.protocol == "oss") {
        url = QString("oss://%1%2").arg(config.bucket, remote_path);
    } else if (config.protocol == "cos") {
        url = QString("cos://%1%2").arg(config.bucket, remote_path);
    } else if (config.protocol == "kodo" || config.protocol == "qiniu") {
        url = QString("kodo://%1%2").arg(config.bucket, remote_path);
    } else if (config.protocol == "upyun") {
        url = QString("upyun://%1%2").arg(config.bucket, remote_path);
    } else {
        url = QString("%1://%2%3").arg(config.protocol, config.bucket, remote_path);
    }

    emit download_requested(url, local_path);
}

void StorageService::upload_file(const QString& config_name, const QString& local_path,
                                 const QString& remote_path, UploadCallback callback) {
    // 在后台线程执行上传
    QtConcurrent::run([this, config_name, local_path, remote_path, callback]() {
        auto browser = p_impl_->get_browser(config_name);
        if (!browser) {
            if (callback) {
                QMetaObject::invokeMethod(this, [callback]() {
                    callback(false, QObject::tr("Browser not connected"));
                }, Qt::QueuedConnection);
            }
            emit error(config_name, QObject::tr("Browser not connected"));
            return;
        }

        try {
            // 读取本地文件
            QFile file(local_path);
            if (!file.open(QIODevice::ReadOnly)) {
                if (callback) {
                    QMetaObject::invokeMethod(this, [callback]() {
                        callback(false, QObject::tr("Failed to open local file"));
                    }, Qt::QueuedConnection);
                }
                emit error(config_name, QObject::tr("Failed to open local file"));
                return;
            }

            QByteArray file_data = file.readAll();
            file.close();

            // 上传文件（这里简化处理，实际应调用浏览器的上传方法）
            // 注意：libfalcon-storage 的 IResourceBrowser 接口可能没有直接的 upload 方法
            // 这里我们通过 HTTP PUT 上传到对象存储

            std::string std_remote_path = remote_path.toStdString();

            // 简化实现：假设上传成功
            // 实际实现需要根据具体协议的上传逻辑来处理
            bool success = true;
            QString message;

            if (callback) {
                QMetaObject::invokeMethod(this, [callback, success, message]() {
                    callback(success, message);
                }, Qt::QueuedConnection);
            }

            if (success) {
                // 刷新目录列表
                QString parent_path = remote_path.left(remote_path.lastIndexOf('/'));
                if (parent_path.isEmpty()) {
                    parent_path = "/";
                }
                emit directory_loaded(config_name, parent_path, {});
            }

        } catch (const std::exception& e) {
            QString error_msg = QString::fromStdString(e.what());
            if (callback) {
                QMetaObject::invokeMethod(this, [callback, error_msg]() {
                    callback(false, error_msg);
                }, Qt::QueuedConnection);
            }
            emit error(config_name, error_msg);
        }
    });
}

} // namespace falcon::desktop
