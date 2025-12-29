/**
 * @file sidebar.hpp
 * @brief 侧边导航栏组件
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <QWidget>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QPropertyAnimation>

namespace falcon::desktop {

/**
 * @brief 侧边导航栏
 *
 * 可收放的侧边导航栏，包含下载、云盘、发现等入口
 */
class SideBar : public QWidget
{
    Q_OBJECT

public:
    explicit SideBar(QWidget* parent = nullptr);
    ~SideBar() override;

    // 展开和收起侧边栏
    void expand();
    void collapse();
    void toggle();

signals:
    void downloadClicked();
    void cloudClicked();
    void discoveryClicked();
    void settingsClicked();

private:
    void setup_ui();
    void create_nav_list();
    void set_expanded(bool expanded);

    // 布局
    QVBoxLayout* layout_;

    // 导航列表
    QListWidget* nav_list_;

    // 收起按钮
    QToolButton* toggle_button_;

    // 状态
    bool expanded_ = true;
    int expanded_width_ = 180;
    int collapsed_width_ = 60;

    // 动画
    QPropertyAnimation* width_animation_;
};

} // namespace falcon::desktop
