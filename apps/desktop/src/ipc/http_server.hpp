/**
 * @file http_server.hpp
 * @brief Localhost HTTP server for browser-extension IPC
 * @author Falcon Team
 * @date 2025-12-29
 */

#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>

namespace falcon::desktop {

struct IncomingDownloadRequest {
    QString url;
    QString filename;
    QString referrer;
    QString user_agent;
    QString cookies;
};

class HttpIpcServer : public QObject
{
    Q_OBJECT

public:
    /// 只读查询端点的数据源：返回序列化好的 JSON 响应体。
    /// 由 MainWindow 注入（快照在 GUI 线程刷新，本服务器也活在 GUI 线程，
    /// 回调内直接读缓存即可，无跨线程问题）。
    using JsonProvider = std::function<QByteArray()>;

    explicit HttpIpcServer(QObject* parent = nullptr);
    ~HttpIpcServer() override = default;

    bool start(quint16 port);
    void stop();
    quint16 port() const { return port_; }

    void set_tasks_provider(JsonProvider provider) { tasks_provider_ = std::move(provider); }
    void set_stats_provider(JsonProvider provider) { stats_provider_ = std::move(provider); }

signals:
    void download_requested(const IncomingDownloadRequest& request);

private slots:
    void on_new_connection();

private:
    struct HttpRequest {
        QByteArray method;
        QByteArray path;
        QByteArray version;
        QHash<QByteArray, QByteArray> headers;
        QByteArray body;
    };

    enum class ParseResult {
        Ok,
        Incomplete,
        Error
    };

    void handle_socket(QTcpSocket* socket);
    static ParseResult parse_http_request(const QByteArray& data, HttpRequest& out_request, QByteArray& out_error);
    static QByteArray header_value(const QHash<QByteArray, QByteArray>& headers, const QByteArray& key_lower);

    static void write_json(QTcpSocket* socket, int status, const QByteArray& json, const QByteArray& extra_headers = {});
    static void write_text(QTcpSocket* socket, int status, const QByteArray& text, const QByteArray& extra_headers = {});

    QTcpServer server_;
    quint16 port_ = 0;
    JsonProvider tasks_provider_;
    JsonProvider stats_provider_;
};

} // namespace falcon::desktop
