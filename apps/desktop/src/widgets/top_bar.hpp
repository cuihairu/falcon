/**
 * @file top_bar.hpp
 * @brief 顶部工具栏组件(Fluent 风格,兼作无边框窗口拖动区)
 * @author Falcon Team
 * @date 2026-04-15
 */

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>

namespace falcon::desktop {

/**
 * @brief 顶部工具栏
 *
 * 包含:
 * - 品牌标识 + 搜索框
 * - 功能按钮组(刷新、视图切换)
 * - 窗口控制按钮(最小化、最大化/还原、关闭,SVG 图标)
 *
 * 空白区左键拖动移动窗口,双击切换最大化(QLineEdit/QPushButton
 * 自消费鼠标事件,天然不冲突)。
 */
class TopBar : public QWidget {
    Q_OBJECT

public:
    explicit TopBar(QWidget* parent = nullptr);
    ~TopBar() override;

    /** 窗口最大化状态变化时由 MainWindow 同步(切换最大化/还原图标) */
    void set_maximized(bool maximized);

    /** 内容页切换时由 MainWindow 同步(视图切换仅对下载页有意义) */
    void set_view_toggle_enabled(bool enabled);

signals:
    void searchRequested(const QString& text);
    void refreshClicked();
    void viewToggleClicked();
    void minimizeClicked();
    void maximizeClicked();
    void closeClicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void setup_ui();
    void update_window_button_icons();

    QLabel* brand_mark_ = nullptr;
    QLabel* brand_title_ = nullptr;
    QLabel* brand_subtitle_ = nullptr;
    // 搜索相关
    QLineEdit* search_edit_ = nullptr;

    // 功能按钮
    QPushButton* refresh_button_ = nullptr;
    QPushButton* view_toggle_button_ = nullptr;

    // 窗口控制
    QPushButton* minimize_button_ = nullptr;
    QPushButton* maximize_button_ = nullptr;
    QPushButton* close_button_ = nullptr;

    bool maximized_ = false;
};

} // namespace falcon::desktop
