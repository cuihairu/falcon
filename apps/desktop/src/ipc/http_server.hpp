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

namespace falcon::desktop {

struct IncomingDownloadRequest {
    QString url;
    QString filename;
    QString referrer;
    QString user_agent;
};

class HttpIpcServer : public QObject
{
    Q_OBJECT

public:
    explicit HttpIpcServer(QObject* parent = nullptr);
    ~HttpIpcServer() override = default;

    bool start(quint16 port);
    void stop();
    quint16 port() const { return port_; }

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
};

} // namespace falcon::desktop
