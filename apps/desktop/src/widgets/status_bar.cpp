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

} // namespace falcon::desktop
