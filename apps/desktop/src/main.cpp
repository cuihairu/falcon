/**
 * @file main.cpp
 * @brief Falcon Desktop 应用入口
 * @author Falcon Team
 * @date 2025-12-27
 */

#include <QApplication>
#include <QTranslator>
#include <QLibraryInfo>
#include <QLocale>
#include <QUrl>
#include <QUrlQuery>
#include <cstdio>

#include <falcon/protocol_registry.hpp>

#include "main_window.hpp"

namespace {

QString extract_falcon_url_from_argv(const QStringList& args)
{
    for (const auto& arg : args) {
        if (arg.startsWith("falcon://", Qt::CaseInsensitive) || arg.startsWith("falcon:", Qt::CaseInsensitive)) {
            return arg;
        }
    }
    return {};
}

} // namespace

int main(int argc, char* argv[])
{
    // 关键:引用 protocols 真实实现的强符号,防止 GNU ld 归档单次扫描下
    // protocols 对象从未拉入、core 的 weak 空 stub 生效(症状:注册 0 个
    // 协议 handler,任何下载抛 UnsupportedProtocolException 后 terminate
    // 整个进程)。daemon 二进制因 RPC 层引用同一符号天然免疫,桌面此前
    // 无任何引用点。同时利用返回值做启动自检。
    const auto builtin_protocols = falcon::describe_builtin_protocols();
    if (builtin_protocols.empty()) {
        std::fprintf(stderr, "Falcon: no protocol handlers compiled in\n");
    }

    QApplication app(argc, argv);

    app.setApplicationName("Falcon");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("FalconTeam");

    // Qt base translations (widgets, dialogs, etc)
    QTranslator qt_translator;
    const QString locale = QLocale::system().name(); // e.g. zh_CN
    (void)qt_translator.load("qtbase_" + locale, QLibraryInfo::path(QLibraryInfo::TranslationsPath));
    app.installTranslator(&qt_translator);

    // App translations (optional; may not exist in dev builds)
    QTranslator app_translator;
    if (!app_translator.load(":/i18n/falcon_desktop_" + locale + ".qm")) {
        (void)app_translator.load("falcon_desktop_" + locale, QCoreApplication::applicationDirPath() + "/i18n");
    }
    app.installTranslator(&app_translator);

    falcon::desktop::MainWindow window;
    window.resize(1200, 800);
    window.show();

    // Optional: handle `falcon://add?url=...` deep-link
    const QString falcon_arg = extract_falcon_url_from_argv(app.arguments());
    if (!falcon_arg.isEmpty()) {
        const QUrl u(falcon_arg);
        const QUrlQuery q(u);
        const QString url = q.queryItemValue("url");
        if (!url.isEmpty()) {
            QMetaObject::invokeMethod(&window, [url, &window]() { window.open_url(url); }, Qt::QueuedConnection);
        }
    }

    return app.exec();
}
