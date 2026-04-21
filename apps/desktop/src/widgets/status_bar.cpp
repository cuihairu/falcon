/**
 * @file status_bar.cpp
 * @brief Status Bar Implementation (Xunlei-style)
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "status_bar.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace falcon::desktop {

namespace {
constexpr int kStatusBarHeight = 40;
} // namespace

StatusBar::StatusBar(QWidget* parent)
    : QWidget(parent)
    , main_layout_(nullptr)
    , speed_label_(nullptr)
    , task_count_label_(nullptr)
    , plan_button_(nullptr)
    , remote_button_(nullptr)
    , plugin_button_(nullptr)
    , detection_button_(nullptr)
    , detection_badge_(nullptr)
    , detection_count_(0)
    , detection_active_(false)
{
    setup_ui();
}

StatusBar::~StatusBar() = default;

void StatusBar::setup_ui()
{
    setFixedHeight(kStatusBarHeight);
    setObjectName("statusBar");

    main_layout_ = new QHBoxLayout(this);
    main_layout_->setContentsMargins(16, 0, 16, 0);
    main_layout_->setSpacing(8);

    // 左侧工具按钮
    plan_button_ = new QPushButton(tr("🕐"), this);
    plan_button_->setObjectName("statusBarButton");
    plan_button_->setToolTip(tr("下载计划"));
    connect(plan_button_, &QPushButton::clicked, this, &StatusBar::downloadPlanClicked);
    main_layout_->addWidget(plan_button_);

    remote_button_ = new QPushButton(tr("☁️"), this);
    remote_button_->setObjectName("statusBarButton");
    remote_button_->setToolTip(tr("远程下载"));
    connect(remote_button_, &QPushButton::clicked, this, &StatusBar::remoteDownloadClicked);
    main_layout_->addWidget(remote_button_);

    plugin_button_ = new QPushButton(tr("🔌"), this);
    plugin_button_->setObjectName("statusBarButton");
    plugin_button_->setToolTip(tr("万能下载插件"));
    connect(plugin_button_, &QPushButton::clicked, this, &StatusBar::pluginClicked);
    main_layout_->addWidget(plugin_button_);

    // 添加统计信息
    main_layout_->addSpacing(16);

    speed_label_ = new QLabel(tr("速度: 0 B/s"), this);
    speed_label_->setObjectName("statusBarLabel");
    main_layout_->addWidget(speed_label_);

    task_count_label_ = new QLabel(tr("任务: 0/0"), this);
    task_count_label_->setObjectName("statusBarLabel");
    main_layout_->addWidget(task_count_label_);

    main_layout_->addStretch();

    // 右侧下载检测按钮
    auto* detection_layout = new QHBoxLayout();
    detection_layout->setSpacing(4);
    main_layout_->addLayout(detection_layout);

    detection_button_ = new QPushButton(tr("⚠️ 下载检测"), this);
    detection_button_->setObjectName("statusBarButton");
    connect(detection_button_, &QPushButton::clicked, this, &StatusBar::downloadDetectionClicked);
    detection_layout->addWidget(detection_button_);

    detection_badge_ = new QLabel(this);
    detection_badge_->setObjectName("detectionBadge");
    detection_badge_->setFixedSize(16, 16);
    detection_badge_->setAlignment(Qt::AlignCenter);
    update_detection_badge();
    detection_layout->addWidget(detection_badge_);
}

void StatusBar::set_download_detection_count(int count)
{
    detection_count_ = count;
    update_detection_badge();
}

void StatusBar::set_download_detection_active(bool active)
{
    detection_active_ = active;
    update_detection_badge();
}

void StatusBar::update_detection_badge()
{
    if (detection_count_ > 0) {
        detection_badge_->setText(QString::number(detection_count_));
        detection_badge_->setStyleSheet(R"(
            QLabel#detectionBadge {
                background-color: #F44336;
                color: white;
                border-radius: 8px;
                font-size: 10px;
                font-weight: bold;
            }
        )");
        detection_badge_->show();
    } else {
        detection_badge_->hide();
    }
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
