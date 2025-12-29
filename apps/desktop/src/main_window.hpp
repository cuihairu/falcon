/**
 * @file main_window.hpp
 * @brief 主窗口类
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <QMainWindow>
#include <memory>
#include <QStackedWidget>

#include <falcon/download_engine.hpp>

namespace falcon::desktop {

class SideBar;
class DownloadPage;
class ClipboardMonitor;
class HttpIpcServer;
struct UrlInfo;
struct IncomingDownloadRequest;

/**
 * @brief 主窗口类
 *
 * 包含可收放的侧边导航栏和内容区域
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

public slots:
    void open_url(const QString& url);

private slots:
    /**
     * @brief Handle detected URL from clipboard
     */
    void on_url_detected(const UrlInfo& url_info);

    void on_download_requested(const IncomingDownloadRequest& request);

private:
    void setup_ui();
    void create_side_bar();
    void create_content_area();
    void create_pages();
    void setup_clipboard_monitor();
    void setup_ipc_server();
    void ensure_download_engine();
    void show_add_download_dialog(UrlInfo url_info, const IncomingDownloadRequest* request_context);

    // 侧边导航栏
    SideBar* side_bar_;

    // 内容区域
    QStackedWidget* content_stack_;

    // 页面实例
    DownloadPage* download_page_;

    // 剪切板监听
    ClipboardMonitor* clipboard_monitor_;

    // 浏览器扩展 IPC
    HttpIpcServer* ipc_server_;

    // 核心下载引擎
    std::unique_ptr<falcon::DownloadEngine> download_engine_;

    // 页面索引
    enum PageIndex {
        PAGE_DOWNLOAD = 0,
        PAGE_CLOUD,
        PAGE_DISCOVERY,
        PAGE_SETTINGS,
        PAGE_COUNT
    };
};

} // namespace falcon::desktop
