/**
 * @file sidebar.hpp
 * @brief 侧边导航栏组件（迅雷风格）
 * @author Falcon Team
 * @date 2026-04-15
 */

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QButtonGroup>

namespace falcon::desktop {

/**
 * @brief 侧边导航栏（迅雷风格）
 *
 * 结构：
 * - 顶部：我的下载（标签页：下载中/已完成/云添加）
 * - 中间：常用工具（片库、三方网盘、回收站）
 * - 底部：私人空间（下载中、已完成）
 */
class SideBar : public QWidget
{
    Q_OBJECT

public:
    explicit SideBar(QWidget* parent = nullptr);
    ~SideBar() override;

signals:
    void downloadClicked();
    void cloudClicked();
    void discoveryClicked();
    void settingsClicked();
    void downloadingTabClicked();
    void completedTabClicked();
    void cloudAddTabClicked();

private:
    void setup_ui();
    void create_my_download_section();
    void create_common_tools_section();
    void create_private_space_section();

    // 主布局
    QVBoxLayout* main_layout_;

    // 我的下载区域
    QLabel* my_download_label_;
    QButtonGroup* my_download_tabs_;
    QPushButton* downloading_tab_;
    QPushButton* completed_tab_;
    QPushButton* cloud_add_tab_;

    // 常用工具区域
    QButtonGroup* common_tools_;
    QPushButton* library_button_;
    QPushButton* third_party_button_;
    QPushButton* recycle_bin_button_;

    // 私人空间区域
    QLabel* private_space_label_;
    QButtonGroup* private_space_tools_;
    QPushButton* private_downloading_button_;
    QPushButton* private_completed_button_;
};

} // namespace falcon::desktop
