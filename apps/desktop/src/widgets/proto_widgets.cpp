/**
 * @file proto_widgets.cpp
 * @brief ui-next 原型通用组件实现
 */

#include "proto_widgets.hpp"

#include "../utils/icon_utils.hpp"

#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSizePolicy>
#include <QFont>

namespace falcon::desktop::proto {
namespace {

/** 画布白色卡片：QFrame + StyledBackground，用 QSS 已有卡片视觉 */
QFrame* card_frame(QWidget* parent)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName("summaryCard");
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setAttribute(Qt::WA_StyledBackground, true);
    return frame;
}

} // namespace

QWidget* page_header(const QString& title, const QString& subtitle, QWidget* parent)
{
    auto* hero = new QWidget(parent);
    hero->setObjectName("downloadHero");
    auto* layout = new QVBoxLayout(hero);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(4);

    auto* title_label = new QLabel(title, hero);
    title_label->setObjectName("heroTitle");
    layout->addWidget(title_label);

    auto* subtitle_label = new QLabel(subtitle, hero);
    subtitle_label->setObjectName("heroDescription");
    subtitle_label->setWordWrap(true);
    layout->addWidget(subtitle_label);
    return hero;
}

QWidget* notice(const QString& text, QWidget* parent, bool accent)
{
    auto* bar = new QWidget(parent);
    bar->setObjectName("summaryCard");
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(10);

    auto* icon = new QLabel(accent ? QStringLiteral("●") : QStringLiteral("i"), bar);
    QFont f = icon->font();
    f.setBold(true);
    icon->setFont(f);
    icon->setFixedWidth(16);
    icon->setObjectName(accent ? "summaryValue" : "sectionLabel");
    layout->addWidget(icon, 0, Qt::AlignTop);

    auto* body = new QLabel(text, bar);
    body->setWordWrap(true);
    body->setObjectName("sectionLabel");
    layout->addWidget(body, 1);
    return bar;
}

QWidget* section_card(const QString& title, QWidget* parent, int margin)
{
    auto* card = card_frame(parent);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(10);

    if (!title.isEmpty()) {
        auto* title_label = new QLabel(title, card);
        title_label->setObjectName("sectionLabel");
        QFont f = title_label->font();
        f.setPointSizeF(f.pointSizeF() + 1.5);
        f.setBold(true);
        title_label->setFont(f);
        layout->addWidget(title_label);
    }
    return card;
}

QWidget* stat_card(const QString& value, const QString& caption, QWidget* parent)
{
    auto* card = card_frame(parent);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(4);
    layout->setAlignment(Qt::AlignCenter);

    auto* value_label = new QLabel(value, card);
    value_label->setObjectName("summaryValue");
    layout->addWidget(value_label, 0, Qt::AlignCenter);

    auto* caption_label = new QLabel(caption, card);
    caption_label->setObjectName("summaryCaption");
    layout->addWidget(caption_label, 0, Qt::AlignCenter);
    return card;
}

QWidget* action_card(const QString& glyph, const QString& title,
                     const QString& subtitle, QWidget* parent,
                     QObject* receiver, const char* member)
{
    auto* button = new QPushButton(parent);
    button->setObjectName("navTab");
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(button);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(12);

    auto* icon = new QLabel(glyph, button);
    icon->setFixedWidth(24);
    icon->setAlignment(Qt::AlignCenter);
    icon->setObjectName("summaryValue");
    layout->addWidget(icon);

    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* title_label = new QLabel(title, button);
    title_label->setObjectName("sectionLabel");
    QFont f = title_label->font();
    f.setBold(true);
    title_label->setFont(f);
    text->addWidget(title_label);
    auto* sub_label = new QLabel(subtitle, button);
    sub_label->setObjectName("summaryCaption");
    sub_label->setWordWrap(true);
    text->addWidget(sub_label);
    layout->addLayout(text);

    if (receiver && member) {
        QObject::connect(button, &QPushButton::clicked, receiver, member);
    }
    return button;
}

QWidget* flow_steps(const QStringList& steps, QWidget* parent)
{
    auto* bar = new QWidget(parent);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(8);

    for (int i = 0; i < steps.size(); ++i) {
        if (i > 0) {
            auto* arrow = new QLabel(QStringLiteral("→"), bar);
            arrow->setObjectName("summaryCaption");
            layout->addWidget(arrow, 0, Qt::AlignCenter);
        }
        auto* step = new QLabel(steps.at(i), bar);
        step->setObjectName("sectionLabel");
        step->setWordWrap(true);
        layout->addWidget(step, 1);
    }
    return bar;
}

QWidget* info_cell(const QString& label, const QString& value, QWidget* parent)
{
    auto* cell = new QWidget(parent);
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* label_widget = new QLabel(label, cell);
    label_widget->setObjectName("summaryCaption");
    layout->addWidget(label_widget);

    auto* value_widget = new QLabel(value, cell);
    value_widget->setObjectName("sectionLabel");
    value_widget->setWordWrap(true);
    value_widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(value_widget);
    return cell;
}

QWidget* node_card(const QString& glyph, const QString& name,
                   const QString& subtitle, const QString& status, bool status_ok,
                   const QStringList& capabilities, const QString& footer,
                   QWidget* parent)
{
    auto* card = card_frame(parent);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* top = new QHBoxLayout();
    top->setSpacing(10);
    auto* icon = new QLabel(glyph, card);
    icon->setFixedSize(30, 30);
    icon->setAlignment(Qt::AlignCenter);
    icon->setObjectName("summaryValue");
    top->addWidget(icon);

    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* name_label = new QLabel(name, card);
    name_label->setObjectName("sectionLabel");
    QFont f = name_label->font();
    f.setBold(true);
    name_label->setFont(f);
    text->addWidget(name_label);
    auto* sub_label = new QLabel(subtitle, card);
    sub_label->setObjectName("summaryCaption");
    text->addWidget(sub_label);
    top->addLayout(text, 1);

    if (!status.isEmpty()) {
        auto* status_label = new QLabel(status, card);
        status_label->setObjectName(status_ok ? "summaryValue" : "sectionLabel");
        top->addWidget(status_label, 0, Qt::AlignTop);
    }
    layout->addLayout(top);

    if (!capabilities.isEmpty()) {
        auto* caps = new QHBoxLayout();
        caps->setSpacing(6);
        for (const QString& cap : capabilities) {
            auto* cap_label = new QLabel(cap, card);
            cap_label->setObjectName("summaryCaption");
            caps->addWidget(cap_label);
        }
        caps->addStretch(1);
        layout->addLayout(caps);
    }

    if (!footer.isEmpty()) {
        auto* footer_label = new QLabel(footer, card);
        footer_label->setObjectName("summaryCaption");
        footer_label->setWordWrap(true);
        layout->addWidget(footer_label);
    }
    return card;
}

QTableWidget* data_table(const QStringList& headers, const QStringList& rows,
                         QWidget* parent)
{
    auto* table = new QTableWidget(parent);
    table->setObjectName("taskTable");
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setRowCount(rows.size());
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);

    for (int r = 0; r < rows.size(); ++r) {
        const QStringList cells = rows.at(r).split('\t');
        for (int c = 0; c < headers.size() && c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells.at(c));
            item->setToolTip(cells.at(c));
            table->setItem(r, c, item);
        }
    }
    return table;
}

QWidget* setting_row(const QString& label, const QString& description,
                     QWidget* control, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(12);

    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* label_widget = new QLabel(label, row);
    label_widget->setObjectName("sectionLabel");
    text->addWidget(label_widget);
    if (!description.isEmpty()) {
        auto* desc_widget = new QLabel(description, row);
        desc_widget->setObjectName("summaryCaption");
        desc_widget->setWordWrap(true);
        text->addWidget(desc_widget);
    }
    layout->addLayout(text, 1);
    if (control) {
        layout->addWidget(control, 0, Qt::AlignRight);
    }
    return row;
}

} // namespace falcon::desktop::proto
