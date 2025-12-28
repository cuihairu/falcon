/**
 * @file sidebar.hpp
 * @brief 侧边导航栏组件
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QPropertyAnimation>

namespace falcon::desktop {

/**
 * @brief 侧边导航按钮
 */
class SideBarButton : public QPushButton
{
    Q_OBJECT

public:
    explicit SideBarButton(const QString& icon_text,
                          const QString& tooltip,
                          QWidget* parent = nullptr);

    void setActive(bool active);
    bool isActive() const { return active_; }

private:
    void update_style(bool active);
    bool active_ = false;
};

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

private:
    void setup_ui();
    void create_buttons();
    void update_button_states();

    // 布局
    QVBoxLayout* layout_;

    // 按钮
    SideBarButton* download_button_;
    SideBarButton* cloud_button_;
    SideBarButton* discovery_button_;

    // 收起按钮
    QPushButton* toggle_button_;

    // 状态
    bool expanded_ = true;
    int expanded_width_ = 180;
    int collapsed_width_ = 60;

    // 动画
    QPropertyAnimation* width_animation_;
};

} // namespace falcon::desktop
