/**
 * @file status_bar.hpp
 * @brief 底部状态栏组件(纯状态展示,无假按钮)
 * @author Falcon Team
 * @date 2026-04-15
 */

#pragma once

#include <QWidget>
#include <QLabel>
#include <QHBoxLayout>

namespace falcon::desktop {

/**
 * @brief 底部状态栏
 *
 * 只承载真实数据:全局下载速度与任务计数,
 * 由 MainWindow 经 DownloadService 的 stats 快照推送。
 */
class StatusBar : public QWidget
{
    Q_OBJECT

public:
    explicit StatusBar(QWidget* parent = nullptr);
    ~StatusBar() override;

    // 更新下载统计信息
    void set_download_speed(uint64_t bytes_per_second);
    void set_task_counts(int downloading, int completed);

private:
    void setup_ui();
    static QString format_speed(uint64_t bytes_per_second);

    QHBoxLayout* main_layout_;

    // 统计信息标签
    QLabel* speed_label_;
    QLabel* task_count_label_;
};

} // namespace falcon::desktop
