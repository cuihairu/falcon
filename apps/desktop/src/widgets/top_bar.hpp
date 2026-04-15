/**
 * @file top_bar.hpp
 * @brief 顶部工具栏组件（迅雷风格）
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
 * 包含：
 * - 搜索框
 * - 用户头像 + 登录按钮
 * - 功能按钮组（刷新、视图切换、更多选项）
 * - 窗口控制按钮（最小化、最大化、关闭）
 */
class TopBar : public QWidget {
    Q_OBJECT

public:
    explicit TopBar(QWidget* parent = nullptr);
    ~TopBar() override;

signals:
    void searchRequested(const QString& text);
    void refreshClicked();
    void viewToggleClicked();
    void moreOptionsClicked();
    void minimizeClicked();
    void maximizeClicked();
    void closeClicked();

private:
    void setup_ui();
    void setup_styles();

    // 搜索相关
    QLineEdit* search_edit_ = nullptr;

    // 功能按钮
    QPushButton* refresh_button_ = nullptr;
    QPushButton* view_toggle_button_ = nullptr;
    QPushButton* more_button_ = nullptr;

    // 窗口控制
    QPushButton* minimize_button_ = nullptr;
    QPushButton* maximize_button_ = nullptr;
    QPushButton* close_button_ = nullptr;
};

} // namespace falcon::desktop
