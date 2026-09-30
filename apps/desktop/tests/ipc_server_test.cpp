/**
 * @file ipc_server_test.cpp
 * @brief HttpIpcServer 回环测试（真实 QTcpServer/QTcpSocket 全链）
 *
 * 自动化 09-19 批次的离屏 + curl 手工冒烟（health/tasks/stats/202/
 * OPTIONS/404/405），并把「/v1/add 应答先于 download_requested 派发」
 * 修复钉成回归用例（模态对话框嵌套事件循环模拟）。
 *
 * 事件循环纪律：QTcpSocket::waitForReadyRead 不处理事件，同线程的
 * 服务器端（QTcpServer/readyRead 处理器）永不触发会死锁——所有等待
 * 必须 processEvents 驱动（嵌套 processEvents 合法，QDialog::exec
 * 同机制）。服务器每应答即 disconnectFromHost，helper 每请求开新连接。
 * @author Falcon Team
 * @date 2026-09-30
 */

#include "ipc/http_server.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <functional>

namespace {

using falcon::desktop::HttpIpcServer;
using falcon::desktop::IncomingDownloadRequest;

// 事件循环自旋等待（替代 waitForReadyRead，理由见文件头）
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

// 按响应头 Content-Length 判断响应体是否已完整到达
bool response_complete(const QByteArray& raw)
{
    const qsizetype header_end = raw.indexOf("\r\n\r\n");
    if (header_end < 0) {
        return false;
    }
    // toLower 不改变长度,索引对原串同样有效
    const qsizetype cl_pos = raw.left(header_end).toLower().indexOf("content-length:");
    if (cl_pos < 0) {
        return true; // 实现恒带 Content-Length,此分支纯防御
    }
    const qsizetype value_off = cl_pos + static_cast<qsizetype>(15); // len("content-length:")
    const qsizetype line_end = raw.indexOf("\r\n", cl_pos);
    const QByteArray value = raw.mid(value_off,
                                     line_end < 0 ? -1 : line_end - value_off).trimmed();
    bool ok = false;
    const int length = value.toInt(&ok);
    if (!ok) {
        return true;
    }
    return raw.size() - (header_end + 4) >= length;
}

QByteArray add_request(const QJsonObject& payload)
{
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    return "POST /v1/add HTTP/1.1\r\nHost: localhost\r\n"
           "Content-Type: application/json\r\nContent-Length: "
           + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

} // namespace

class HttpIpcServerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start(0));
        ASSERT_NE(server.port(), quint16(0));
    }

    void TearDown() override
    {
        server.stop();
    }

    // 每请求一条新连接（实现 Connection: close,每应答即断开）
    QByteArray exchange(const QByteArray& request, bool* ok = nullptr)
    {
        QTcpSocket client;
        QByteArray received;
        QObject::connect(&client, &QTcpSocket::readyRead,
                         [&client, &received] { received += client.readAll(); });
        client.connectToHost(QHostAddress::LocalHost, server.port());
        if (!spin_until([&] {
                return client.state() == QAbstractSocket::ConnectedState;
            })) {
            if (ok) {
                *ok = false;
            }
            return {};
        }
        client.write(request);
        client.flush();
        const bool complete = spin_until([&] { return response_complete(received); });
        if (ok) {
            *ok = complete;
        }
        // 等服务器侧主动断开（应答后 disconnectFromHost 的语义锚）
        spin_until([&] {
            return client.state() == QAbstractSocket::UnconnectedState;
        }, 1000);
        return received;
    }

    HttpIpcServer server;
};

// ---------- 生命周期 ----------

TEST_F(HttpIpcServerTest, StartStopLifecycle)
{
    const quint16 first = server.port();
    EXPECT_NE(first, quint16(0));

    server.stop();
    EXPECT_EQ(server.port(), quint16(0));

    // 重启回到可用状态
    EXPECT_TRUE(server.start(0));
    EXPECT_NE(server.port(), quint16(0));
    bool ok = false;
    const QByteArray raw = exchange("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n", &ok);
    EXPECT_TRUE(ok) << "restart must serve requests again";
    EXPECT_TRUE(raw.contains("200"));
}

TEST_F(HttpIpcServerTest, StartOnOccupiedPortFails)
{
    HttpIpcServer second;
    EXPECT_FALSE(second.start(server.port()));
    EXPECT_EQ(second.port(), quint16(0));
    // 原服务器不受影响
    bool ok = false;
    exchange("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n", &ok);
    EXPECT_TRUE(ok);
}

// ---------- GET 只读端点 ----------

TEST_F(HttpIpcServerTest, HealthEndpointReturnsStaticJson)
{
    const QByteArray raw = exchange("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 200 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Access-Control-Allow-Origin: *"));
    EXPECT_TRUE(raw.contains("\"ok\":true"));
    EXPECT_TRUE(raw.contains("\"name\":\"falcon-desktop\""));
    EXPECT_TRUE(raw.contains("\"api\":\"v1\""));
}

TEST_F(HttpIpcServerTest, TasksWithoutProviderReturns503)
{
    const QByteArray raw = exchange("GET /v1/tasks HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 503 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("\"ok\":false"));
    EXPECT_TRUE(raw.contains("tasks provider not ready"));
}

TEST_F(HttpIpcServerTest, TasksProviderPassthroughReturns200)
{
    server.set_tasks_provider([] {
        return QByteArray(R"([{"gid":"1","status":"active"}])");
    });
    const QByteArray raw = exchange("GET /v1/tasks HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 200 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Access-Control-Allow-Origin: *"));
    EXPECT_TRUE(raw.contains("\"status\":\"active\""));
}

TEST_F(HttpIpcServerTest, StatsWithoutProviderReturns503)
{
    const QByteArray raw = exchange("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 503 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("stats provider not ready"));
}

TEST_F(HttpIpcServerTest, StatsProviderPassthroughReturns200)
{
    server.set_stats_provider([] { return QByteArray(R"({"ok":true})"); });
    const QByteArray raw = exchange("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 200 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains(R"({"ok":true})"));
}

TEST_F(HttpIpcServerTest, UnknownGetPathReturns404)
{
    const QByteArray raw = exchange("GET /nope HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 404 ")) << raw.toStdString();
}

TEST_F(HttpIpcServerTest, UnsupportedMethodReturns405)
{
    const QByteArray raw = exchange("PUT /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 405 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Method not allowed"));
}

TEST_F(HttpIpcServerTest, OptionsPreflightReturnsCorsTrio)
{
    const QByteArray raw = exchange("OPTIONS /v1/add HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 200 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Access-Control-Allow-Origin: *"));
    EXPECT_TRUE(raw.contains("Access-Control-Allow-Methods: GET, POST, OPTIONS"));
    EXPECT_TRUE(raw.contains("Access-Control-Allow-Headers: content-type"));
}

TEST_F(HttpIpcServerTest, LowercaseGetMethodAccepted)
{
    // 方法名统一小写化后分发：小写 "get" 与 "GET" 同路径
    const QByteArray raw = exchange("get /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 200 ")) << raw.toStdString();
}

// ---------- POST /v1/add ----------

TEST_F(HttpIpcServerTest, PostAddReturns202AndEmitsTrimmedRequest)
{
    IncomingDownloadRequest captured;
    int emissions = 0;
    QObject::connect(&server, &HttpIpcServer::download_requested,
                     [&captured, &emissions](const IncomingDownloadRequest& req) {
                         captured = req;
                         ++emissions;
                     });

    QJsonObject payload;
    payload.insert("url", "  https://example.com/file.zip  ");
    payload.insert("filename", "  renamed.zip  ");
    payload.insert("referrer", "https://example.com/page");
    payload.insert("user_agent", "FalconExt/1.0");
    payload.insert("cookies", "session=abc");
    const QByteArray raw = exchange(add_request(payload));

    EXPECT_TRUE(raw.startsWith("HTTP/1.1 202 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains(R"({"ok":true})"));
    EXPECT_EQ(emissions, 1);
    // url/filename 两字段 trim；referrer/user_agent/cookies 原样透传
    EXPECT_EQ(captured.url.toStdString(), "https://example.com/file.zip");
    EXPECT_EQ(captured.filename.toStdString(), "renamed.zip");
    EXPECT_EQ(captured.referrer.toStdString(), "https://example.com/page");
    EXPECT_EQ(captured.user_agent.toStdString(), "FalconExt/1.0");
    EXPECT_EQ(captured.cookies.toStdString(), "session=abc");
}

TEST_F(HttpIpcServerTest, PostAddRespondsBeforeDispatchingSignal)
{
    // 「先应答再派发」回归钉子：download_requested 是同线程直连，
    // 真实消费方（MainWindow）会弹模态对话框（嵌套事件循环）阻塞到用户
    // 操作——若 202 在 emit 之后才写出，发送方 1.5s 超时必然先到。
    // 此处以嵌套 processEvents 精确模拟模态对话框的等待形态。
    QTcpSocket client;
    QByteArray received;
    QObject::connect(&client, &QTcpSocket::readyRead,
                     [&client, &received] { received += client.readAll(); });

    QObject::connect(&server, &HttpIpcServer::download_requested,
                     [&received](const IncomingDownloadRequest&) {
                         // 模态对话框形态：在信号处理器内嵌套跑事件循环等应答
                         const bool got = spin_until(
                             [&] { return received.startsWith("HTTP/1.1 202 "); }, 2000);
                         EXPECT_TRUE(got)
                             << "202 必须先于 download_requested 派发到达 socket";
                     });

    QJsonObject payload;
    payload.insert("url", "https://example.com/nail.zip");
    client.connectToHost(QHostAddress::LocalHost, server.port());
    ASSERT_TRUE(spin_until([&] {
        return client.state() == QAbstractSocket::ConnectedState;
    }));
    client.write(add_request(payload));
    client.flush();
    EXPECT_TRUE(spin_until([&] { return response_complete(received); }));
    EXPECT_TRUE(received.startsWith("HTTP/1.1 202 ")) << received.toStdString();
}

TEST_F(HttpIpcServerTest, PostAddInvalidJsonReturns400)
{
    const QByteArray body = "not json";
    const QByteArray request = "POST /v1/add HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: "
        + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    const QByteArray raw = exchange(request);
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 400 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Invalid JSON"));
}

TEST_F(HttpIpcServerTest, PostAddNonObjectJsonReturns400)
{
    // 合法 JSON 但非 object（数组形态）
    const QByteArray body = "[1,2,3]";
    const QByteArray request = "POST /v1/add HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: "
        + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    const QByteArray raw = exchange(request);
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 400 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Invalid JSON"));
}

TEST_F(HttpIpcServerTest, PostAddMissingUrlReturns400)
{
    QJsonObject payload;
    payload.insert("filename", "x.zip");
    const QByteArray raw = exchange(add_request(payload));
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 400 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Missing url"));
}

TEST_F(HttpIpcServerTest, PostAddWhitespaceOnlyUrlReturns400)
{
    QJsonObject payload;
    payload.insert("url", "   ");
    const QByteArray raw = exchange(add_request(payload));
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 400 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Missing url"));
}

TEST_F(HttpIpcServerTest, PostWrongPathReturns404)
{
    const QByteArray body = R"({"url":"https://example.com/a.zip"})";
    const QByteArray request = "POST /v1/other HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    const QByteArray raw = exchange(request);
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 404 ")) << raw.toStdString();
}

// ---------- 请求解析边界 ----------

TEST_F(HttpIpcServerTest, IncompleteRequestHoldsUntilHeaderTerminator)
{
    QTcpSocket client;
    QByteArray received;
    QObject::connect(&client, &QTcpSocket::readyRead,
                     [&client, &received] { received += client.readAll(); });
    client.connectToHost(QHostAddress::LocalHost, server.port());
    ASSERT_TRUE(spin_until([&] {
        return client.state() == QAbstractSocket::ConnectedState;
    }));

    // 头区无 \r\n\r\n 终结：不得产生任何应答
    client.write("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n");
    client.flush();
    spin_until([] { return false; }, 300); // 泵事件 300ms
    EXPECT_TRUE(received.isEmpty()) << "半截请求不应有应答";

    // 补上终结空行 → 正常应答
    client.write("\r\n");
    client.flush();
    EXPECT_TRUE(spin_until([&] { return response_complete(received); }));
    EXPECT_TRUE(received.startsWith("HTTP/1.1 200 ")) << received.toStdString();
}

TEST_F(HttpIpcServerTest, GarbageRequestLineReturns400)
{
    const QByteArray raw = exchange("GARBAGE\r\n\r\n");
    EXPECT_TRUE(raw.startsWith("HTTP/1.1 400 ")) << raw.toStdString();
    EXPECT_TRUE(raw.contains("Invalid request line"));
}

TEST_F(HttpIpcServerTest, InvalidContentLengthReturns400)
{
    const QByteArray abc = exchange(
        "POST /v1/add HTTP/1.1\r\nContent-Length: abc\r\n\r\n");
    EXPECT_TRUE(abc.startsWith("HTTP/1.1 400 ")) << abc.toStdString();
    EXPECT_TRUE(abc.contains("Invalid Content-Length"));

    const QByteArray negative = exchange(
        "POST /v1/add HTTP/1.1\r\nContent-Length: -5\r\n\r\n");
    EXPECT_TRUE(negative.startsWith("HTTP/1.1 400 ")) << negative.toStdString();
    EXPECT_TRUE(negative.contains("Invalid Content-Length"));
}

TEST_F(HttpIpcServerTest, PartialBodyCompletesWhenRemainderArrives)
{
    QTcpSocket client;
    QByteArray received;
    QObject::connect(&client, &QTcpSocket::readyRead,
                     [&client, &received] { received += client.readAll(); });
    client.connectToHost(QHostAddress::LocalHost, server.port());
    ASSERT_TRUE(spin_until([&] {
        return client.state() == QAbstractSocket::ConnectedState;
    }));

    // 声明 12 字节 body 只发 5 字节：Incomplete,无应答
    const QByteArray body = R"({"url":"ab"})";
    ASSERT_EQ(body.size(), 12);
    client.write("POST /v1/add HTTP/1.1\r\nHost: localhost\r\n"
                 "Content-Length: 12\r\n\r\n");
    client.write(body.left(5));
    client.flush();
    spin_until([] { return false; }, 300); // 泵事件 300ms
    EXPECT_TRUE(received.isEmpty()) << "body 未收齐不应有应答";

    // 补齐余量 → 202
    client.write(body.mid(5));
    client.flush();
    EXPECT_TRUE(spin_until([&] { return response_complete(received); }));
    EXPECT_TRUE(received.startsWith("HTTP/1.1 202 ")) << received.toStdString();
}

TEST_F(HttpIpcServerTest, OversizeRequestReturns413)
{
    QTcpSocket client;
    QByteArray received;
    QObject::connect(&client, &QTcpSocket::readyRead,
                     [&client, &received] { received += client.readAll(); });
    client.connectToHost(QHostAddress::LocalHost, server.port());
    ASSERT_TRUE(spin_until([&] {
        return client.state() == QAbstractSocket::ConnectedState;
    }));

    // 300KB 无换行垃圾：超 256KB 上限即 413（尺寸检查先于解析，
    // 不需要合法请求结构）
    const QByteArray chunk(64 * 1024, 'A');
    for (int i = 0; i < 5; ++i) {
        client.write(chunk);
        client.flush();
    }
    EXPECT_TRUE(spin_until([&] { return response_complete(received); }));
    EXPECT_TRUE(received.startsWith("HTTP/1.1 413 ")) << received.toStdString();
    EXPECT_TRUE(received.contains("Request too large"));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
