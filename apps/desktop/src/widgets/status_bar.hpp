/**
 * @file status_bar.hpp
 * @brief 底部状态栏组件（迅雷风格）
 * @author Falcon Team
 * @date 2026-04-15
 */

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>

namespace falcon::desktop {

/**
 * @brief 底部状态栏（迅雷风格）
 *
 * 包含：
 * - 左侧：下载计划、远程下载、万能下载插件
 * - 右侧：下载检测（带红点提示）
 */
class StatusBar : public QWidget
{
    Q_OBJECT

public:
    explicit StatusBar(QWidget* parent = nullptr);
    ~StatusBar() override;

    void set_download_detection_count(int count);
    void set_download_detection_active(bool active);

    // 更新下载统计信息
    void set_download_speed(uint64_t bytes_per_second);
    void set_task_counts(int downloading, int completed);

signals:
    void downloadPlanClicked();
    void remoteDownloadClicked();
    void pluginClicked();
    void downloadDetectionClicked();

private:
    void setup_ui();
    void update_detection_badge();
    static QString format_speed(uint64_t bytes_per_second);

    QHBoxLayout* main_layout_;

    // 统计信息标签
    QLabel* speed_label_;
    QLabel* task_count_label_;

    // 左侧按钮
    QPushButton* plan_button_;
    QPushButton* remote_button_;
    QPushButton* plugin_button_;

    // 右侧按钮
    QPushButton* detection_button_;
    QLabel* detection_badge_;

    // 状态
    int detection_count_ = 0;
    bool detection_active_ = false;
};

} // namespace falcon::desktop
