/**
 * @file http_server.cpp
 * @brief Localhost HTTP server for browser-extension IPC
 * @author Falcon Team
 * @date 2025-12-29
 */

#include "http_server.hpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>

namespace falcon::desktop {

namespace {
constexpr int kMaxRequestBytes = 256 * 1024;

QByteArray status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}

QByteArray to_lower_ascii(QByteArray s)
{
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}
} // namespace

HttpIpcServer::HttpIpcServer(QObject* parent)
    : QObject(parent)
{
    connect(&server_, &QTcpServer::newConnection, this, &HttpIpcServer::on_new_connection);
}

bool HttpIpcServer::start(quint16 port)
{
    stop();

    if (!server_.listen(QHostAddress::LocalHost, port)) {
        port_ = 0;
        return false;
    }

    port_ = server_.serverPort();
    return true;
}

void HttpIpcServer::stop()
{
    if (server_.isListening()) {
        server_.close();
    }
    port_ = 0;
}

void HttpIpcServer::on_new_connection()
{
    while (server_.hasPendingConnections()) {
        QTcpSocket* socket = server_.nextPendingConnection();
        if (!socket) {
            continue;
        }

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handle_socket(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void HttpIpcServer::handle_socket(QTcpSocket* socket)
{
    QByteArray buffer = socket->property("falcon_http_buffer").toByteArray();
    buffer += socket->readAll();
    socket->setProperty("falcon_http_buffer", buffer);

    if (buffer.size() > kMaxRequestBytes) {
        write_text(socket, 413, "Request too large");
        socket->disconnectFromHost();
        return;
    }

    HttpRequest req;
    QByteArray error;
    const ParseResult parse_result = parse_http_request(buffer, req, error);
    if (parse_result == ParseResult::Incomplete) {
        return;
    }
    if (parse_result == ParseResult::Error) {
        write_text(socket, 400, error.isEmpty() ? "Bad request" : error);
        socket->disconnectFromHost();
        return;
    }

    const QByteArray method = to_lower_ascii(req.method);
    const QByteArray path = req.path;

    if (method == "options") {
        write_text(
            socket,
            200,
            "OK",
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: content-type\r\n"
        );
        socket->disconnectFromHost();
        return;
    }

    if (method != "post") {
        write_text(socket, 405, "Method not allowed");
        socket->disconnectFromHost();
        return;
    }

    if (path != "/v1/add") {
        write_text(socket, 404, "Not found");
        socket->disconnectFromHost();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(req.body);
    if (!doc.isObject()) {
        write_text(socket, 400, "Invalid JSON");
        socket->disconnectFromHost();
        return;
    }

    const QJsonObject obj = doc.object();
    const QString url = obj.value("url").toString().trimmed();
    if (url.isEmpty()) {
        write_text(socket, 400, "Missing url");
        socket->disconnectFromHost();
        return;
    }

    IncomingDownloadRequest request;
    request.url = url;
    request.filename = obj.value("filename").toString().trimmed();
    request.referrer = obj.value("referrer").toString();
    request.user_agent = obj.value("user_agent").toString();
    emit download_requested(request);

    write_json(
        socket,
        202,
        R"({"ok":true})",
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
    );
    socket->disconnectFromHost();
}

HttpIpcServer::ParseResult HttpIpcServer::parse_http_request(
    const QByteArray& data,
    HttpRequest& out_request,
    QByteArray& out_error
)
{
    const qsizetype header_end = data.indexOf("\r\n\r\n");
    if (header_end < 0) {
        return ParseResult::Incomplete;
    }

    const QByteArray header = data.left(header_end);
    const QList<QByteArray> lines = header.split('\n');
    if (lines.isEmpty()) {
        out_error = "Empty request";
        return ParseResult::Error;
    }

    const QByteArray request_line = lines[0].trimmed();
    const QList<QByteArray> parts = request_line.split(' ');
    if (parts.size() < 3) {
        out_error = "Invalid request line";
        return ParseResult::Error;
    }

    out_request.method = parts[0].trimmed();
    out_request.path = parts[1].trimmed();
    out_request.version = parts[2].trimmed();

    out_request.headers.clear();
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const QByteArray key = to_lower_ascii(line.left(colon).trimmed());
        const QByteArray val = line.mid(colon + 1).trimmed();
        out_request.headers.insert(key, val);
    }

    const QByteArray cl = header_value(out_request.headers, "content-length");
    int content_len = 0;
    if (!cl.isEmpty()) {
        bool ok = false;
        content_len = cl.toInt(&ok);
        if (!ok || content_len < 0) {
            out_error = "Invalid Content-Length";
            return ParseResult::Error;
        }
    }

    const QByteArray body = data.mid(header_end + 4);
    if (content_len > 0) {
        if (body.size() < content_len) {
            return ParseResult::Incomplete;
        }
        out_request.body = body.left(content_len);
        return ParseResult::Ok;
    }

    out_request.body = body;
    return ParseResult::Ok;
}

QByteArray HttpIpcServer::header_value(const QHash<QByteArray, QByteArray>& headers, const QByteArray& key_lower)
{
    auto it = headers.find(key_lower);
    if (it == headers.end()) {
        return {};
    }
    return it.value();
}

void HttpIpcServer::write_json(QTcpSocket* socket, int status, const QByteArray& json, const QByteArray& extra_headers)
{
    write_text(socket, status, json, extra_headers);
}

void HttpIpcServer::write_text(QTcpSocket* socket, int status, const QByteArray& text, const QByteArray& extra_headers)
{
    QByteArray response;
    response += "HTTP/1.1 ";
    response += QByteArray::number(status);
    response += ' ';
    response += status_text(status);
    response += "\r\n";
    response += "Connection: close\r\n";
    response += "Content-Length: ";
    response += QByteArray::number(text.size());
    response += "\r\n";
    if (!extra_headers.isEmpty()) {
        response += extra_headers;
        if (!extra_headers.endsWith("\r\n")) {
            response += "\r\n";
        }
    }
    response += "\r\n";
    response += text;
    socket->write(response);
    socket->flush();
}

} // namespace falcon::desktop
