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

namespace falcon::desktop {

class SideBar;
class ClipboardMonitor;
struct UrlInfo;

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

private slots:
    /**
     * @brief Handle detected URL from clipboard
     */
    void on_url_detected(const UrlInfo& url_info);

private:
    void setup_ui();
    void create_side_bar();
    void create_content_area();
    void create_pages();
    void setup_clipboard_monitor();

    // 侧边导航栏
    SideBar* side_bar_;

    // 内容区域
    QStackedWidget* content_stack_;

    // 剪切板监听
    ClipboardMonitor* clipboard_monitor_;

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
