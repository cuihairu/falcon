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

// 前向声明
enum class DownloadViewMode : int;

class SideBar;
class TopBar;
class StatusBar;
class DownloadPage;
class SettingsPage;
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
    void on_new_task_requested();
    void on_configured_download_requested(const QString& url);
    void on_direct_download_requested(const QString& url, bool start_immediately);
    void on_remove_task_requested(falcon::TaskId id);
    void on_remove_finished_tasks_requested();
    void on_minimize_requested();
    void on_maximize_requested();
    void on_close_requested();

private:
    void setup_ui();
    void create_top_bar();
    void create_side_bar();
    void create_content_area();
    void create_pages();
    void setup_clipboard_monitor();
    void setup_ipc_server();
    void ensure_download_engine();
    void show_add_download_dialog(UrlInfo url_info, const IncomingDownloadRequest* request_context);
    bool add_download_task(const QString& url, bool start_immediately);
    void load_settings();
    void save_settings() const;
    void apply_settings_to_runtime();

    // 顶部工具栏
    TopBar* top_bar_;

    // 侧边导航栏
    SideBar* side_bar_;

    // 底部状态栏
    StatusBar* status_bar_;

    // 内容区域
    QStackedWidget* content_stack_;

    // 页面实例
    DownloadPage* download_page_;
    SettingsPage* settings_page_;

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
