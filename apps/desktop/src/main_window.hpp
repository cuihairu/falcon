/**
 * @file main_window.hpp
 * @brief 主窗口类
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <QMainWindow>
#include <QHash>
#include <QString>
#include <memory>
#include <QStackedWidget>
#include <QSystemTrayIcon>

#include <falcon/types.hpp>

#include <rpc/aria2_snapshots.hpp>
#include <vector>

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
class ThemeManager;
class DownloadService;
struct UrlInfo;
struct IncomingDownloadRequest;

/**
 * @brief 主窗口类
 *
 * 包含可收放的侧边导航栏和内容区域。
 * 下载能力经 DownloadService 提供（进程内引擎或 daemon RPC 后端），
 * 本类不再直接持有 DownloadEngine。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    /** 窗口最大化状态变化时同步顶栏按钮图标(最大化/还原) */
    void changeEvent(QEvent* event) override;

    /**
     * @brief qApp 级事件过滤器:无边框窗口 8 向边缘缩放
     *
     * 必须挂在 qApp 上——中央控件(表格/滚动区)会吞掉自身鼠标事件,
     * MainWindow 的 mousePressEvent 收不到;而对单个子控件装过滤器
     * 只拦发给该对象的事件,拦不到孙子辈。谓词保持廉价
     * (isWidgetType → window()==this → 非最大化)。
     */
    bool eventFilter(QObject* obj, QEvent* event) override;

public slots:
    void open_url(const QString& url);

private slots:
    /**
     * @brief Handle detected URL from clipboard
     */
    void on_url_detected(const UrlInfo& url_info);

    // 系统托盘槽函数
    void on_tray_activated(QSystemTrayIcon::ActivationReason reason);
    void on_tray_show_clicked();
    void on_tray_quit_clicked();

    // 主题切换槽函数
    void on_theme_toggle_requested();

    void on_download_requested(const IncomingDownloadRequest& request);
    void on_new_task_requested();
    void on_configured_download_requested(const QString& url);
    void on_direct_download_requested(const QString& url, bool start_immediately);
    void on_remove_task_requested(falcon::TaskId id);
    void on_remove_finished_tasks_requested();
    void on_priority_changed(falcon::TaskId id, falcon::TaskPriority priority);
    void on_minimize_requested();
    void on_maximize_requested();
    void on_close_requested();

    // DownloadService 事件（已投递到 GUI 线程）
    void on_tasks_refreshed(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks);
    void on_stats_refreshed(falcon::daemon::rpc::GlobalStats stats);
    void on_task_add_failed(const QString& url, const QString& reason);
    void on_task_completed(falcon::TaskId id, const QString& output_path);
    void on_task_failed(falcon::TaskId id, const QString& error_message);

private:
    void setup_ui();
    void create_top_bar();
    void create_side_bar();
    void create_content_area();
    void create_pages();
    void setup_clipboard_monitor();
    void setup_ipc_server();
    void setup_system_tray();
    void ensure_download_service();
    void show_add_download_dialog(UrlInfo url_info, const IncomingDownloadRequest* request_context);
    bool add_download_task(const QString& url, bool start_immediately);
    void load_settings();
    void save_settings() const;
    void apply_settings_to_runtime();

    /** 命中窗口边缘 6px 缩放带时返回对应边(无边框窗口 resize 光标与拖拽判定) */
    Qt::Edges resize_edge_for(const QPoint& global_pos) const;

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

    // 系统托盘
    QSystemTrayIcon* system_tray_;
    QMenu* tray_menu_;

    // 主题管理
    ThemeManager* theme_manager_;

    // 下载服务（worker 线程 + 后端抽象）
    DownloadService* download_service_;

    // 最近一轮任务快照的 URL 映射（错误通知里显示文件名用）
    QHash<qulonglong, QString> task_url_by_id_;

    // 边缘缩放光标当前是否由本类设置(true 才在离开边缘带时恢复箭头)
    bool resize_cursor_active_ = false;

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
