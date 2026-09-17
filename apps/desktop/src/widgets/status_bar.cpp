/**
 * @file status_bar.cpp
 * @brief Status Bar Implementation
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "status_bar.hpp"

#include <QHBoxLayout>

namespace falcon::desktop {

namespace {
constexpr int kStatusBarHeight = 40;
} // namespace

StatusBar::StatusBar(QWidget* parent)
    : QWidget(parent)
    , main_layout_(nullptr)
    , speed_label_(nullptr)
    , task_count_label_(nullptr)
{
    setup_ui();
}

StatusBar::~StatusBar() = default;

void StatusBar::setup_ui()
{
    setFixedHeight(kStatusBarHeight);
    setObjectName("statusBar");

    main_layout_ = new QHBoxLayout(this);
    main_layout_->setContentsMargins(16, 4, 16, 4);
    main_layout_->setSpacing(16);

    speed_label_ = new QLabel(tr("速度: 0 B/s"), this);
    speed_label_->setObjectName("statusBarLabel");
    main_layout_->addWidget(speed_label_);

    task_count_label_ = new QLabel(tr("任务: 0/0"), this);
    task_count_label_->setObjectName("statusBarLabel");
    main_layout_->addWidget(task_count_label_);

    main_layout_->addStretch();
}

void StatusBar::set_download_speed(uint64_t bytes_per_second)
{
    speed_label_->setText(tr("速度: %1").arg(format_speed(bytes_per_second)));
}

void StatusBar::set_task_counts(int downloading, int completed)
{
    task_count_label_->setText(tr("任务: %1/%2").arg(downloading).arg(completed));
}

QString StatusBar::format_speed(uint64_t bytes_per_second)
{
    if (bytes_per_second == 0) {
        return "0 B/s";
    }

    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit = 0;
    double speed = static_cast<double>(bytes_per_second);

    while (speed >= 1024.0 && unit < 3) {
        speed /= 1024.0;
        ++unit;
    }

    if (unit == 0) {
        return QString("%1 B/s").arg(bytes_per_second);
    }

    return QString("%1 %2").arg(speed, 0, 'f', 1).arg(units[unit]);
}

} // namespace falcon::desktop
