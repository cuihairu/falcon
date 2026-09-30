/**
 * @file storage_service_test.cpp
 * @brief StorageService 单测（云存储桥接层，零测模块补缺）
 *
 * 网络红线纪律：成功路径全部经回环 MockHttpServer（packages/
 * libfalcon-storage 测试共享基建）；失败路径用 http://127.0.0.1:1
 * （回环连接拒绝，秒级失败，不出网）——与 storage 包 browser mock
 * 测试同口径。
 *
 * 线程语义：list_directory 回调在 QtConcurrent worker 线程直调
 * （QMutex 编组）；upload_file 回调恒经 QueuedConnection 回主线程；
 * worker 线程 emit 的信号经 context-object connect 排队送达，
 * spin_until（processEvents 驱动）收割——waitFor* 家族同线程必死锁
 * （ipc_server_test 文件头纪律沿用）。
 *
 * QSettings 隔离：IniFormat UserScope 重定向到临时目录 + 每用例
 * clear()，配置持久化用例（跨实例 round-trip）不污染真实用户配置。
 * @author Falcon Team
 * @date 2026-09-30
 */

#include "services/storage_service.hpp"
#include "mock_http_server.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <memory>

namespace {

using falcon::desktop::CloudStorageConfig;
using falcon::desktop::RemoteResourceInfo;
using falcon::desktop::StorageService;

// 事件循环自旋等待（沿 ipc_server_test 口径：等待必须 processEvents 驱动）
bool spin_until(const std::function<bool()>& pred, int timeout_ms = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred()) {
        if (timer.elapsed() >= timeout_ms) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

CloudStorageConfig make_config(const QString& name, const QString& protocol,
                               const QString& bucket = QStringLiteral("testbucket"),
                               const QString& endpoint = QString())
{
    CloudStorageConfig config;
    config.name = name;
    config.protocol = protocol;
    config.endpoint = endpoint;
    config.access_key = QStringLiteral("AKIA_TEST");
    config.secret_key = QStringLiteral("secret");
    config.region = QStringLiteral("us-west-2");
    config.bucket = bucket;
    return config;
}

// worker 线程直调回调的编组结果（list_directory 族）
struct ListOutcome {
    QMutex mutex;
    bool invoked = false;
    QList<RemoteResourceInfo> resources;
};

class StorageServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QSettings settings; // 隔离目录内的 Ini（见 main）
        settings.clear();

        service = std::make_unique<StorageService>();
        // fixture 非 QObject：connect 必须 QObject:: 限定（静态成员查找）
        QObject::connect(service.get(), &StorageService::connected, service.get(),
                [this](const QString& name) {
                    last_connected_name = name;
                    ++connected_count;
                });
        QObject::connect(service.get(), &StorageService::disconnected, service.get(),
                [this](const QString& name) {
                    last_disconnected_name = name;
                    ++disconnected_count;
                });
        QObject::connect(service.get(), &StorageService::error, service.get(),
                [this](const QString& name, const QString& message) {
                    last_error_name = name;
                    last_error_message = message;
                    ++error_count;
                });
        QObject::connect(service.get(), &StorageService::directory_loaded, service.get(),
                [this](const QString&, const QString& path,
                       const QList<RemoteResourceInfo>& resources) {
                    last_loaded_path = path;
                    last_loaded_resources = resources;
                    ++directory_loaded_count;
                });
        QObject::connect(service.get(), &StorageService::download_requested, service.get(),
                [this](const QString& url, const QString& local_path) {
                    last_download_url = url;
                    last_download_local = local_path;
                    ++download_requested_count;
                });
    }

    std::unique_ptr<StorageService> service;

    // 信号观测（全部经排队/直连送达主线程，spin 收割）
    int connected_count = 0;
    int disconnected_count = 0;
    int error_count = 0;
    int directory_loaded_count = 0;
    int download_requested_count = 0;
    QString last_connected_name;
    QString last_disconnected_name;
    QString last_error_name;
    QString last_error_message;
    QString last_loaded_path;
    QList<RemoteResourceInfo> last_loaded_resources;
    QString last_download_url;
    QString last_download_local;
};

} // namespace

// ---------- 配置持久化 ----------

TEST_F(StorageServiceTest, ConfigRoundTripAcrossInstances)
{
    const CloudStorageConfig a = make_config("primary", "s3", "media", "http://minio.local:9000");
    const CloudStorageConfig b = make_config("backup", "upyun", "files");
    service->save_config(a);
    service->save_config(b);

    // 新实例从持久化装载（ctor load_configs）
    StorageService reloaded;
    const QList<CloudStorageConfig> configs = reloaded.load_configs();
    ASSERT_EQ(configs.size(), 2);

    const CloudStorageConfig* primary = nullptr;
    const CloudStorageConfig* backup = nullptr;
    for (const auto& c : configs) {
        if (c.name == "primary") {
            primary = &c;
        } else if (c.name == "backup") {
            backup = &c;
        }
    }
    ASSERT_NE(primary, nullptr);
    ASSERT_NE(backup, nullptr);
    // is_connected 持久化位不因 save 而虚标（fresh save 恒 false）
    EXPECT_EQ(primary->protocol.toStdString(), "s3");
    EXPECT_EQ(primary->bucket.toStdString(), "media");
    EXPECT_EQ(primary->endpoint.toStdString(), "http://minio.local:9000");
    EXPECT_EQ(primary->access_key.toStdString(), "AKIA_TEST");
    EXPECT_EQ(primary->secret_key.toStdString(), "secret");
    EXPECT_EQ(primary->region.toStdString(), "us-west-2");
    EXPECT_FALSE(primary->is_connected);
    EXPECT_EQ(backup->protocol.toStdString(), "upyun");
    EXPECT_FALSE(backup->is_connected);
}

TEST_F(StorageServiceTest, SaveConfigUpdatesInPlaceByName)
{
    service->save_config(make_config("same", "s3", "old-bucket"));
    service->save_config(make_config("other", "oss", "kept"));
    // 同名再存 = 原位更新，不是追加
    service->save_config(make_config("same", "cos", "new-bucket"));

    const QList<CloudStorageConfig> configs = service->load_configs();
    ASSERT_EQ(configs.size(), 2);
    for (const auto& c : configs) {
        if (c.name == "same") {
            EXPECT_EQ(c.protocol.toStdString(), "cos");
            EXPECT_EQ(c.bucket.toStdString(), "new-bucket");
        } else {
            EXPECT_EQ(c.name.toStdString(), "other");
            EXPECT_EQ(c.bucket.toStdString(), "kept");
        }
    }
}

TEST_F(StorageServiceTest, RemoveConfigDeletesOnlyNamedEntry)
{
    service->save_config(make_config("a", "s3"));
    service->save_config(make_config("b", "oss"));

    service->remove_config("a");
    const QList<CloudStorageConfig> after = service->load_configs();
    ASSERT_EQ(after.size(), 1);
    EXPECT_EQ(after.first().name.toStdString(), "b");
}

// ---------- 连接管理 ----------

TEST_F(StorageServiceTest, FreshServiceHasNoConnections)
{
    EXPECT_TRUE(service->connected_storages().isEmpty());
    EXPECT_FALSE(service->is_connected("anything"));
}

TEST_F(StorageServiceTest, ConnectUnsupportedProtocolFailsWithError)
{
    // webdav 不在 BrowserFactory 注册表内：create_browser 空指针收口
    const bool ok = service->connect_storage(make_config("dav", "webdav"));
    EXPECT_FALSE(ok);
    EXPECT_FALSE(service->is_connected("dav"));
    EXPECT_TRUE(service->connected_storages().isEmpty());
    EXPECT_EQ(error_count, 1);
    EXPECT_EQ(last_error_name.toStdString(), "dav");
    EXPECT_TRUE(last_error_message.contains("Unsupported protocol",
                                            Qt::CaseInsensitive))
        << last_error_message.toStdString();
}

TEST_F(StorageServiceTest, ConnectUnreachableEndpointFailsCleanly)
{
    // 回环端口 1：连接拒绝秒级失败（不出网），失败路径全链
    const bool ok = service->connect_storage(
        make_config("dead", "s3", "bucket-x", "http://127.0.0.1:1"));
    EXPECT_FALSE(ok);
    EXPECT_FALSE(service->is_connected("dead"));
    EXPECT_TRUE(service->connected_storages().isEmpty());
    EXPECT_EQ(error_count, 1);
    EXPECT_EQ(last_error_name.toStdString(), "dead");
    EXPECT_FALSE(last_error_message.isEmpty());
}

TEST_F(StorageServiceTest, ConnectSucceedsAgainstLoopbackMock)
{
    MockHttpServer server([](const std::string&, const std::string&) {
        return MockHttpServer::Response{200, "{}"}; // test_connection: 非空响应即成功
    });
    ASSERT_TRUE(server.start());

    const bool ok = service->connect_storage(
        make_config("mock-s3", "s3", "testbucket", QString::fromStdString(server.base_url())));
    EXPECT_TRUE(ok);
    EXPECT_TRUE(service->is_connected("mock-s3"));
    EXPECT_EQ(service->connected_storages(), QStringList{"mock-s3"});
    EXPECT_EQ(connected_count, 1);
    EXPECT_EQ(last_connected_name.toStdString(), "mock-s3");
    EXPECT_EQ(error_count, 0);

    // 连接探测真实到达回环服务器（GET ?max-keys=1）
    const auto reqs = server.requests();
    ASSERT_FALSE(reqs.empty());
    EXPECT_EQ(reqs.front().first, "GET");
    EXPECT_TRUE(reqs.front().second.find("max-keys=1") != std::string::npos)
        << reqs.front().second;
}

TEST_F(StorageServiceTest, DisconnectClearsStateAndEmits)
{
    MockHttpServer server([](const std::string&, const std::string&) {
        return MockHttpServer::Response{200, "{}"};
    });
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(service->connect_storage(
        make_config("tmp", "s3", "testbucket", QString::fromStdString(server.base_url()))));

    service->disconnect_storage("tmp");
    EXPECT_EQ(disconnected_count, 1);
    EXPECT_EQ(last_disconnected_name.toStdString(), "tmp");
    EXPECT_FALSE(service->is_connected("tmp"));
    EXPECT_TRUE(service->connected_storages().isEmpty());
}

TEST_F(StorageServiceTest, DisconnectUnknownNameStillEmits)
{
    // 未连接/不存在的名字断开：无条件发 disconnected 信号（幂等语义）
    service->disconnect_storage("ghost");
    EXPECT_EQ(disconnected_count, 1);
    EXPECT_EQ(last_disconnected_name.toStdString(), "ghost");
}

TEST_F(StorageServiceTest, RemoveConfigOnConnectedAlsoDisconnects)
{
    MockHttpServer server([](const std::string&, const std::string&) {
        return MockHttpServer::Response{200, "{}"};
    });
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(service->connect_storage(
        make_config("doomed", "s3", "testbucket", QString::fromStdString(server.base_url()))));

    service->remove_config("doomed");
    EXPECT_EQ(disconnected_count, 1) << "remove 已连接配置必须先断开";
    EXPECT_FALSE(service->is_connected("doomed"));
    EXPECT_TRUE(service->connected_storages().isEmpty());
    EXPECT_TRUE(service->load_configs().isEmpty());
}

// ---------- 无 browser 守卫路径 ----------

TEST_F(StorageServiceTest, ListDirectoryWithoutBrowserInvokesEmptyCallbackAndError)
{
    auto outcome = std::make_shared<ListOutcome>();
    service->list_directory("nobody", "/", [outcome](const QList<RemoteResourceInfo>& resources) {
        QMutexLocker lock(&outcome->mutex);
        outcome->invoked = true;
        outcome->resources = resources;
    });

    EXPECT_TRUE(spin_until([&] {
        QMutexLocker lock(&outcome->mutex);
        return outcome->invoked;
    })) << "worker 线程回调必须被调用";
    {
        QMutexLocker lock(&outcome->mutex);
        EXPECT_TRUE(outcome->resources.isEmpty());
    }
    EXPECT_TRUE(spin_until([&] { return error_count > 0; }));
    EXPECT_EQ(last_error_name.toStdString(), "nobody");
    EXPECT_EQ(last_error_message.toStdString(), "Browser not connected");
}

TEST_F(StorageServiceTest, UploadWithoutBrowserInvokesQueuedFailureCallback)
{
    bool invoked = false;
    bool success = true;
    QString message;
    service->upload_file("nobody", "/tmp/falcon-up-src.bin", "/remote/x.bin",
                         [&](bool ok, const QString& msg) {
                             invoked = true;
                             success = ok;
                             message = msg;
                         });

    EXPECT_TRUE(spin_until([&] { return invoked; })) << "回调经 QueuedConnection 必须送达";
    EXPECT_FALSE(success);
    EXPECT_EQ(message.toStdString(), "Browser not connected");
    EXPECT_TRUE(spin_until([&] { return error_count > 0; }));
    EXPECT_EQ(last_error_name.toStdString(), "nobody");
}

TEST_F(StorageServiceTest, ResourceOperationsWithoutBrowserReturnDefaults)
{
    const RemoteResourceInfo info = service->get_resource_info("nobody", "/a.txt");
    EXPECT_TRUE(info.name.isEmpty());
    EXPECT_EQ(info.size, qint64(0));
    EXPECT_TRUE(info.type.isEmpty());

    EXPECT_FALSE(service->create_directory("nobody", "/dir"));
    EXPECT_FALSE(service->remove_resource("nobody", "/dir"));
    EXPECT_FALSE(service->rename_resource("nobody", "/a", "/b"));
    EXPECT_FALSE(service->copy_resource("nobody", "/a", "/b"));
    EXPECT_TRUE(service->get_quota_info("nobody").isEmpty());
}

// ---------- 下载 URL 构造（纯信号，离线） ----------

TEST_F(StorageServiceTest, RequestDownloadBuildsProtocolUrls)
{
    struct Row {
        const char* protocol;
        const char* expected_url;
    };
    const Row rows[] = {
        {"s3", "s3://media/data/a.bin"},
        {"oss", "oss://media/data/a.bin"},
        {"cos", "cos://media/data/a.bin"},
        {"kodo", "kodo://media/data/a.bin"},
        {"qiniu", "kodo://media/data/a.bin"}, // qiniu 归一到 kodo scheme
        {"upyun", "upyun://media/data/a.bin"},
        {"webdav", "webdav://media/data/a.bin"}, // 未知协议按 scheme 直拼
    };
    for (const Row& row : rows) {
        service->save_config(make_config(QString("cfg-") + row.protocol, row.protocol, "media"));
        service->request_download(QString("cfg-") + row.protocol, "/data/a.bin", "/tmp/out.bin");
        ASSERT_EQ(download_requested_count, 1) << "protocol: " << row.protocol;
        EXPECT_EQ(last_download_url.toStdString(), row.expected_url) << "protocol: " << row.protocol;
        EXPECT_EQ(last_download_local.toStdString(), "/tmp/out.bin");
        download_requested_count = 0;
    }
}

// ---------- 回环 mock 上的目录列举 ----------

TEST_F(StorageServiceTest, ListDirectoryMapsContentsAndFiltersHidden)
{
    // Contents 三项：两可见一隐藏；name=basename、path=key、size 直传
    static const std::string kListBody = R"({"Contents":[
        {"Key":"docs/readme.md","Size":100,"LastModified":"2026-01-01T00:00:00Z","ETag":"\"e1\""},
        {"Key":"data.csv","Size":2048,"LastModified":"2026-01-02T00:00:00Z","ETag":"\"e2\""},
        {"Key":".hidden","Size":7,"LastModified":"2026-01-03T00:00:00Z","ETag":"\"e3\""}
    ]})";
    MockHttpServer server([&](const std::string&, const std::string& path) {
        if (path.find("list-type=2") != std::string::npos) {
            return MockHttpServer::Response{200, kListBody};
        }
        return MockHttpServer::Response{200, "{}"};
    });
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(service->connect_storage(
        make_config("listed", "s3", "testbucket", QString::fromStdString(server.base_url()))));

    auto outcome = std::make_shared<ListOutcome>();
    service->list_directory("listed", "/", [outcome](const QList<RemoteResourceInfo>& resources) {
        QMutexLocker lock(&outcome->mutex);
        outcome->invoked = true;
        outcome->resources = resources;
    });

    ASSERT_TRUE(spin_until([&] {
        QMutexLocker lock(&outcome->mutex);
        return outcome->invoked;
    })) << "列举回调必须被调用";

    QList<RemoteResourceInfo> resources;
    {
        QMutexLocker lock(&outcome->mutex);
        resources = outcome->resources;
    }
    // show_hidden=false（桥接层固定）+ name 升序
    ASSERT_EQ(resources.size(), 2);
    EXPECT_EQ(resources[0].name.toStdString(), "data.csv");
    EXPECT_EQ(resources[0].path.toStdString(), "data.csv");
    EXPECT_EQ(resources[0].size, qint64(2048));
    EXPECT_EQ(resources[0].type.toStdString(), "file");
    EXPECT_FALSE(resources[0].is_hidden);
    EXPECT_EQ(resources[1].name.toStdString(), "readme.md");
    EXPECT_EQ(resources[1].path.toStdString(), "docs/readme.md");
    EXPECT_EQ(resources[1].size, qint64(100));
    EXPECT_EQ(resources[1].type.toStdString(), "file");

    // directory_loaded 信号与回调同源（worker emit → 排队送达）
    EXPECT_TRUE(spin_until([&] { return directory_loaded_count > 0; }));
    EXPECT_EQ(last_loaded_path.toStdString(), "/");
    EXPECT_EQ(last_loaded_resources.size(), 2);

    // 请求真实到达回环服务器且带 ListObjectsV2 标记
    bool saw_list = false;
    for (const auto& [method, path] : server.requests()) {
        if (path.find("list-type=2") != std::string::npos) {
            saw_list = true;
            EXPECT_EQ(method, "GET");
        }
    }
    EXPECT_TRUE(saw_list) << "ListObjectsV2 请求必须真实到达回环服务器";
}

TEST_F(StorageServiceTest, UploadLocalFileOpenFailureReports)
{
    MockHttpServer server([](const std::string&, const std::string&) {
        return MockHttpServer::Response{200, "{}"};
    });
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(service->connect_storage(
        make_config("up", "s3", "testbucket", QString::fromStdString(server.base_url()))));

    bool invoked = false;
    bool success = true;
    QString message;
    service->upload_file("up", "/tmp/falcon-no-such-src-file.bin", "/remote/x.bin",
                         [&](bool ok, const QString& msg) {
                             invoked = true;
                             success = ok;
                             message = msg;
                         });

    EXPECT_TRUE(spin_until([&] { return invoked; }));
    EXPECT_FALSE(success);
    EXPECT_EQ(message.toStdString(), "Failed to open local file");
    EXPECT_TRUE(spin_until([&] { return error_count > 0; }));
    EXPECT_EQ(last_error_name.toStdString(), "up");
    EXPECT_EQ(last_error_message.toStdString(), "Failed to open local file");
}

int main(int argc, char** argv)
{
    // QSettings 隔离：IniFormat UserScope 重定向临时目录（main 存活期），
    // 组织/应用名在首个 QSettings 构造之前设定——StorageService 构造
    // 函数里 load_configs 就会读配置，晚了重定向不生效
    static QTemporaryDir settings_dir;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("falcon-desktop-tests");
    QCoreApplication::setApplicationName("storage-service-test");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
