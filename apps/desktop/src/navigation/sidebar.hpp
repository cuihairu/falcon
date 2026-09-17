/**
 * @file sidebar.hpp
 * @brief 侧边导航栏组件（Fluent 风格,单排他导航组）
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
 * @brief 侧边导航栏
 *
 * 结构：
 * - 我的下载（标签页：下载中/已完成/云添加）
 * - 发现与空间（资源发现、云盘空间、偏好设置）
 * - 私人空间（隐私下载、隐私完成）
 *
 * 全部 7 个导航钮同属一个 exclusive QButtonGroup——任意时刻有且仅有
 * 一个高亮,与内容页一一对应。
 */
class SideBar : public QWidget
{
    Q_OBJECT

public:
    explicit SideBar(QWidget* parent = nullptr);
    ~SideBar() override;

    /** 底部统计卡显示当前活跃任务数(DownloadService stats 推送) */
    void set_queue_count(int count);

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
    void create_download_section();
    void create_space_section();
    QPushButton* create_nav_button(const QString& text);
    QWidget* create_separator();

    // 主布局
    QVBoxLayout* main_layout_;

    // 唯一排他导航组(全部导航钮共用,替代旧的三组互不排他)
    QButtonGroup* nav_group_;

    QLabel* library_label_ = nullptr;
    QLabel* transfer_label_ = nullptr;
    QLabel* my_download_label_;
    QPushButton* downloading_tab_;
    QPushButton* completed_tab_;
    QPushButton* cloud_add_tab_;

    // 发现与空间区域
    QPushButton* library_button_;
    QPushButton* third_party_button_;
    QPushButton* recycle_bin_button_;

    // 私人空间区域
    QLabel* private_space_label_;
    QPushButton* private_downloading_button_;
    QPushButton* private_completed_button_;

    QWidget* footer_card_ = nullptr;
    QLabel* footer_value_ = nullptr;
    QLabel* footer_label_ = nullptr;
};

} // namespace falcon::desktop
