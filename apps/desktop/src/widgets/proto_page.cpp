/**
 * @file proto_page.cpp
 * @brief ui-next 原型页面基类实现
 */

#include "proto_page.hpp"

#include "proto_widgets.hpp"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLayoutItem>
#include <QScrollArea>
#include <QFrame>
#include <QFont>

namespace falcon::desktop::proto {

ProtoPage::ProtoPage(QWidget* parent)
    : QWidget(parent)
{
    setup_ui();
}

void ProtoPage::setup_ui()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    header_host_ = new QWidget(this);
    header_top_ = new QHBoxLayout(header_host_);
    header_top_->setContentsMargins(20, 18, 20, 18);
    header_top_->setSpacing(12);

    header_layout_ = new QVBoxLayout();
    header_layout_->setSpacing(4);
    header_top_->addLayout(header_layout_, 1);

    root->addWidget(header_host_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* scroll_body = new QWidget(scroll);
    content_layout_ = new QVBoxLayout(scroll_body);
    content_layout_->setContentsMargins(18, 4, 18, 18);
    content_layout_->setSpacing(14);
    content_layout_->addStretch(1);
    scroll->setWidget(scroll_body);

    root->addWidget(scroll, 1);
}

void ProtoPage::set_header(const QString& title, const QString& subtitle,
                           const QString& badge_text, bool badge_warning)
{
    if (!header_layout_) {
        return;
    }
    // 清空旧标题区（重复调用时）
    while (QLayoutItem* item = header_layout_->takeAt(0)) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    auto* title_label = new QLabel(title, header_host_);
    title_label->setObjectName("heroTitle");
    header_layout_->addWidget(title_label);

    auto* subtitle_label = new QLabel(subtitle, header_host_);
    subtitle_label->setObjectName("heroDescription");
    subtitle_label->setWordWrap(true);
    header_layout_->addWidget(subtitle_label);

    if (!badge_text.isEmpty()) {
        if (!badge_) {
            badge_ = new QWidget(header_host_);
            badge_->setObjectName("summaryCard");
            badge_->setAttribute(Qt::WA_StyledBackground, true);
            auto* badge_layout = new QHBoxLayout(badge_);
            badge_layout->setContentsMargins(10, 6, 10, 6);
            badge_label_ = new QLabel(badge_);
            badge_label_->setObjectName("sectionLabel");
            badge_layout->addWidget(badge_label_);
            header_top_->addWidget(badge_, 0, Qt::AlignTop);
        }
        badge_label_->setText(badge_text);
        badge_label_->setObjectName(badge_warning ? "heroTitle" : "summaryValue");
        badge_->setVisible(true);
    } else if (badge_) {
        badge_->setVisible(false);
    }
}

void ProtoPage::clear_content()
{
    if (!content_layout_) {
        return;
    }
    while (QLayoutItem* item = content_layout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        } else if (QLayout* nested = item->layout()) {
            // 嵌套布局里的 widget 随父 widget 销毁；此处仅释放布局项
            while (QLayoutItem* child = nested->takeAt(0)) {
                delete child;
            }
        }
        delete item;
    }
}

QPushButton* ProtoPage::add_header_button(const QString& text)
{
    auto* button = new QPushButton(text, header_host_);
    button->setObjectName("primaryButton");
    header_top_->addWidget(button, 0, Qt::AlignTop);
    return button;
}

} // namespace falcon::desktop::proto
