/**
 * @file sidebar.cpp
 * @brief Sidebar implementation (platform-native look)
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "sidebar.hpp"

#include <QListWidgetItem>
#include <QSignalBlocker>
#include <QStyle>
#include <QEasingCurve>

namespace falcon::desktop {

namespace {
constexpr int kNavItemHeight = 44;
constexpr int kNavIconSizeExpanded = 18;
constexpr int kNavIconSizeCollapsed = 22;
} // namespace

SideBar::SideBar(QWidget* parent)
    : QWidget(parent)
    , layout_(nullptr)
    , nav_list_(nullptr)
    , toggle_button_(nullptr)
    , width_animation_(new QPropertyAnimation(this, "maximumWidth"))
{
    setup_ui();

    // Animation config
    width_animation_->setDuration(200);
    width_animation_->setEasingCurve(QEasingCurve::OutCubic);
}

SideBar::~SideBar() = default;

void SideBar::setup_ui()
{
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(8, 8, 8, 8);
    layout_->setSpacing(8);

    create_nav_list();
    layout_->addWidget(nav_list_, 1);

    toggle_button_ = new QToolButton(this);
    toggle_button_->setAutoRaise(true);
    toggle_button_->setCursor(Qt::PointingHandCursor);
    layout_->addWidget(toggle_button_, 0, Qt::AlignHCenter);
    connect(toggle_button_, &QToolButton::clicked, this, &SideBar::toggle);

    setMaximumWidth(expanded_width_);
    setMinimumWidth(expanded_width_);
    set_expanded(true);

    nav_list_->setCurrentRow(0);
}

void SideBar::create_nav_list()
{
    nav_list_ = new QListWidget(this);
    nav_list_->setFrameShape(QFrame::NoFrame);
    nav_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    nav_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav_list_->setFocusPolicy(Qt::NoFocus);
    nav_list_->setUniformItemSizes(true);

    auto add_item = [this](const QString& title, QStyle::StandardPixmap icon) {
        auto* item = new QListWidgetItem(style()->standardIcon(icon), title);
        item->setData(Qt::UserRole, title);
        item->setToolTip(title);
        item->setSizeHint(QSize(0, kNavItemHeight));
        nav_list_->addItem(item);
    };

    add_item(tr("Downloads"), QStyle::SP_ArrowDown);
    add_item(tr("Cloud"), QStyle::SP_DriveNetIcon);
    add_item(tr("Discovery"), QStyle::SP_FileDialogContentsView);
    add_item(tr("Settings"), QStyle::SP_FileDialogDetailedView);

    connect(nav_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        switch (row) {
        case 0: emit downloadClicked(); break;
        case 1: emit cloudClicked(); break;
        case 2: emit discoveryClicked(); break;
        case 3: emit settingsClicked(); break;
        default: break;
        }
    });
}

void SideBar::expand()
{
    if (expanded_) {
        return;
    }
    set_expanded(true);

    width_animation_->stop();
    setMinimumWidth(collapsed_width_);
    QObject::disconnect(width_animation_, nullptr, this, nullptr);
    width_animation_->setStartValue(maximumWidth());
    width_animation_->setEndValue(expanded_width_);
    connect(width_animation_, &QPropertyAnimation::finished, this, [this]() {
        setMinimumWidth(expanded_width_);
        setMaximumWidth(expanded_width_);
    });
    width_animation_->start();
}

void SideBar::collapse()
{
    if (!expanded_) {
        return;
    }
    set_expanded(false);

    width_animation_->stop();
    setMinimumWidth(collapsed_width_);
    QObject::disconnect(width_animation_, nullptr, this, nullptr);
    width_animation_->setStartValue(maximumWidth());
    width_animation_->setEndValue(collapsed_width_);
    connect(width_animation_, &QPropertyAnimation::finished, this, [this]() {
        setMinimumWidth(collapsed_width_);
        setMaximumWidth(collapsed_width_);
    });
    width_animation_->start();
}

void SideBar::toggle()
{
    if (expanded_) {
        collapse();
    } else {
        expand();
    }
}

void SideBar::set_expanded(bool expanded)
{
    expanded_ = expanded;

    const QSignalBlocker blocker(nav_list_);
    if (expanded_) {
        toggle_button_->setToolTip(tr("Collapse sidebar"));
        toggle_button_->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));
        nav_list_->setViewMode(QListView::ListMode);
        nav_list_->setIconSize(QSize(kNavIconSizeExpanded, kNavIconSizeExpanded));
        nav_list_->setSpacing(2);

        for (int i = 0; i < nav_list_->count(); ++i) {
            auto* item = nav_list_->item(i);
            item->setText(item->data(Qt::UserRole).toString());
        }
    } else {
        toggle_button_->setToolTip(tr("Expand sidebar"));
        toggle_button_->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
        nav_list_->setViewMode(QListView::IconMode);
        nav_list_->setIconSize(QSize(kNavIconSizeCollapsed, kNavIconSizeCollapsed));
        nav_list_->setGridSize(QSize(collapsed_width_ - 16, kNavItemHeight + 10));
        nav_list_->setMovement(QListView::Static);
        nav_list_->setSpacing(4);

        for (int i = 0; i < nav_list_->count(); ++i) {
            auto* item = nav_list_->item(i);
            item->setText(QString());
        }
    }
}

} // namespace falcon::desktop
