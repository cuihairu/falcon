/**
 * @file trash_page.cpp
 * @brief 回收站页面实现
 * @author Falcon Team
 * @date 2026-09-28
 */

#include "trash_page.hpp"

#include "../utils/icon_utils.hpp"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>

namespace falcon::desktop {

TrashPage::TrashPage(QWidget* parent)
    : QWidget(parent)
{
    setup_ui();
}

void TrashPage::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(18, 18, 18, 18);
    main_layout->setSpacing(14);

    main_layout->addWidget(create_page_hero());

    // 工具行：左侧计数，右侧清空钮
    auto* toolbar = new QWidget(this);
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(2, 0, 2, 0);
    toolbar_layout->setSpacing(8);

    count_label_ = new QLabel(toolbar);
    toolbar_layout->addWidget(count_label_);
    toolbar_layout->addStretch();

    clear_button_ = new QPushButton(tr("清空回收站"), toolbar);
    clear_button_->setObjectName("toolButton");
    clear_button_->setCursor(Qt::PointingHandCursor);
    clear_button_->setEnabled(false);
    toolbar_layout->addWidget(clear_button_);
    connect(clear_button_, &QPushButton::clicked, this, &TrashPage::clear_requested);
    main_layout->addWidget(toolbar);

    // 条目表格
    table_ = new QTableWidget(this);
    table_->setObjectName("taskTable");
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({
        tr("文件名"),
        tr("大小"),
        tr("状态"),
        tr("原始位置"),
        tr("删除时间"),
        tr("操作")
    });
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->setColumnWidth(1, 100);
    table_->setColumnWidth(2, 100);
    table_->setColumnWidth(3, 260);
    table_->setColumnWidth(4, 150);
    table_->setColumnWidth(5, 150);
    main_layout->addWidget(table_, 1);

    // 空态
    empty_state_ = new QWidget(this);
    empty_state_->setObjectName("summaryCard");
    auto* empty_layout = new QVBoxLayout(empty_state_);
    empty_layout->setContentsMargins(20, 28, 20, 28);
    empty_layout->setSpacing(8);
    empty_layout->setAlignment(Qt::AlignCenter);
    auto* empty_title = new QLabel(tr("回收站是空的"), empty_state_);
    empty_title->setObjectName("emptyStateTitle");
    empty_title->setAlignment(Qt::AlignCenter);
    empty_layout->addWidget(empty_title);
    auto* empty_body = new QLabel(
        tr("删除的任务会先放在这里，保留天数过后自动清理。"), empty_state_);
    empty_body->setObjectName("emptyStateBody");
    empty_body->setAlignment(Qt::AlignCenter);
    empty_body->setWordWrap(true);
    empty_layout->addWidget(empty_body);
    main_layout->addWidget(empty_state_);
    empty_state_->setVisible(false);
}

QWidget* TrashPage::create_page_hero()
{
    auto* hero = new QWidget(this);
    hero->setObjectName("downloadHero");

    auto* layout = new QVBoxLayout(hero);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(4);

    auto* title = new QLabel(tr("回收站"), hero);
    title->setObjectName("heroTitle");
    layout->addWidget(title);

    auto* desc = new QLabel(tr("已删除的任务与文件暂时保存在这里，可以恢复或彻底删除。"), hero);
    desc->setObjectName("heroDescription");
    desc->setWordWrap(true);
    layout->addWidget(desc);

    return hero;
}

void TrashPage::set_entries(const std::vector<TrashEntry>& entries)
{
    entries_ = entries;
    rebuild_table();
}

void TrashPage::rebuild_table()
{
    table_->setRowCount(0);
    for (const auto& entry : entries_) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(
            QString::fromStdString(entry.file_name)));
        table_->setItem(row, 1, new QTableWidgetItem(
            entry.total_bytes > 0 ? format_bytes(entry.total_bytes) : QStringLiteral("-")));
        table_->setItem(row, 2, new QTableWidgetItem(status_text(entry)));
        table_->setItem(row, 3, new QTableWidgetItem(
            QString::fromStdString(entry.output_path)));
        table_->setItem(row, 4, new QTableWidgetItem(
            format_deleted_at(entry.deleted_at)));

        auto* op_widget = new QWidget(table_);
        auto* op_layout = new QHBoxLayout(op_widget);
        op_layout->setContentsMargins(5, 2, 5, 2);
        op_layout->setSpacing(5);

        auto* restore_btn = new QPushButton(tr("恢复"), op_widget);
        restore_btn->setObjectName("rowActionButton");
        restore_btn->setCursor(Qt::PointingHandCursor);
        restore_btn->setProperty("entryId", qulonglong{entry.id});
        op_layout->addWidget(restore_btn);

        auto* purge_btn = new QPushButton(tr("删除"), op_widget);
        purge_btn->setObjectName("rowActionButton");
        purge_btn->setCursor(Qt::PointingHandCursor);
        purge_btn->setProperty("entryId", qulonglong{entry.id});
        op_layout->addWidget(purge_btn);
        op_layout->addStretch();
        table_->setCellWidget(row, 5, op_widget);

        // 行号会随重建漂移，回调里按 id 从 entries_ 现查
        connect(restore_btn, &QPushButton::clicked, this, [this, restore_btn] {
            emit restore_requested(restore_btn->property("entryId").toULongLong());
        });
        connect(purge_btn, &QPushButton::clicked, this, [this, purge_btn] {
            emit purge_requested(purge_btn->property("entryId").toULongLong());
        });
    }

    const bool empty = entries_.empty();
    table_->setVisible(!empty);
    empty_state_->setVisible(empty);
    clear_button_->setEnabled(!empty);
    count_label_->setText(empty ? QString()
                                : tr("共 %1 项").arg(entries_.size()));
}

QString TrashPage::format_bytes(std::uint64_t bytes)
{
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QString::number(bytes) + " B";
    }
    return QString("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

QString TrashPage::format_deleted_at(std::int64_t epoch_seconds)
{
    if (epoch_seconds <= 0) {
        return QStringLiteral("-");
    }
    return QDateTime::fromSecsSinceEpoch(epoch_seconds)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString TrashPage::status_text(const TrashEntry& entry)
{
    if (entry.status == "completed") {
        return entry.file_in_trash ? tr("已下载完成") : tr("已完成（文件不在了）");
    }
    if (entry.status == "failed") {
        return tr("下载失败");
    }
    return tr("已取消");
}

} // namespace falcon::desktop
