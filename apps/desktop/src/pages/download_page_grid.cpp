/**
 * @file download_page_grid.cpp
 * @brief DownloadPage grid view implementation
 */

#include "download_page.hpp"
#include "download_page_detail.hpp"
#include "services/task_order.hpp"
#include "utils/icon_utils.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGridLayout>
#include <QMenu>
#include <QProgressBar>
#include <QScrollArea>
#include <QToolButton>

#include <algorithm>

using namespace falcon::desktop;


void DownloadPage::create_task_grid()
{
    // 创建网格容器
    grid_container_ = new QWidget(this);

    auto* container_layout = new QVBoxLayout(grid_container_);
    container_layout->setContentsMargins(0, 0, 0, 0);
    container_layout->setSpacing(0);

    // 创建滚动区域
    grid_scroll_area_ = new QScrollArea(grid_container_);
    grid_scroll_area_->setWidgetResizable(true);
    grid_scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    grid_scroll_area_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    grid_scroll_area_->setObjectName("gridScrollArea");

    // 创建网格内容 widget
    grid_widget_ = new QWidget(grid_scroll_area_);
    grid_layout_ = new QGridLayout(grid_widget_);
    grid_layout_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    grid_layout_->setSpacing(12);
    grid_layout_->setContentsMargins(8, 8, 8, 8);

    grid_scroll_area_->setWidget(grid_widget_);
    container_layout->addWidget(grid_scroll_area_);

    // 启用右键菜单
    grid_widget_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_widget_, &QWidget::customContextMenuRequested,
            this, &DownloadPage::show_grid_context_menu);
}
void DownloadPage::set_display_style(TaskDisplayStyle style)
{
    if (display_style_ == style) {
        return;
    }

    display_style_ = style;

    rerender();
    update_empty_state();

    // 按钮文字恒为「点击后切换到的目标视图」——外部（设置页）切换同样要跟上
    if (style_toggle_button_) {
        style_toggle_button_->setText(display_style_ == TaskDisplayStyle::Table
                                          ? tr("卡片视图")
                                          : tr("列表视图"));
    }

    // 手动切换记忆：MainWindow 落 QSettings 并同步设置页（首次/无记录默认卡片）
    emit display_style_changed(display_style_ == TaskDisplayStyle::Grid);
}
void DownloadPage::set_text_filter(const QString& text)
{
    if (text_filter_ == text) {
        return;
    }
    text_filter_ = text;

    // rerender 内部 sync_task_row 按 should_show 双向增删行;
    // 网格视图在其尾部统一 sync_task_grid
    rerender();
}
void DownloadPage::toggle_display_style()
{
    set_display_style(display_style_ == TaskDisplayStyle::Table
                          ? TaskDisplayStyle::Grid
                          : TaskDisplayStyle::Table);
}

void DownloadPage::on_style_toggle_clicked()
{
    toggle_display_style();
}
void DownloadPage::show_grid_context_menu(const QPoint& pos)
{
    // 找到点击的任务卡片
    QWidget* clicked_widget = grid_widget_->childAt(pos);
    if (!clicked_widget) {
        return;
    }

    // 从 widget 的 property 获取任务 ID
    QVariant task_id_var = clicked_widget->property("taskId");
    if (!task_id_var.isValid()) {
        // 尝试从父 widget 获取
        if (clicked_widget->parentWidget()) {
            task_id_var = clicked_widget->parentWidget()->property("taskId");
        }
    }

    if (!task_id_var.isValid()) {
        return;
    }

    const auto* record = record_by_id(task_id_var.toULongLong());
    if (!record) {
        return;
    }

    const auto& snapshot = record->snapshot;

    QMenu menu(this);

    // 根据任务状态显示不同菜单项
    const auto status = snapshot.status;

    // 暂停/继续
    if (status == falcon::TaskStatus::Downloading ||
        status == falcon::TaskStatus::Preparing) {
        auto* pause_action = menu.addAction(tr("暂停"));
        connect(pause_action, &QAction::triggered, this,
                [this, id = snapshot.id]() { emit pause_requested(id); });
    } else if (status == falcon::TaskStatus::Paused ||
               status == falcon::TaskStatus::Failed) {
        auto* resume_action = menu.addAction(tr("继续"));
        connect(resume_action, &QAction::triggered, this,
                [this, id = snapshot.id]() { emit resume_requested(id); });
    }

    // 停止做种(同 show_context_menu)
    if (snapshot.seeding_active) {
        auto* stop_seed_action = menu.addAction(tr("停止做种"));
        connect(stop_seed_action, &QAction::triggered, this,
                [this, id = snapshot.id]() { emit stop_seeding_requested(id); });
    }

    menu.addSeparator();

    // 按值捕获快照内容，理由同 show_context_menu（B18: 打开文件/文件夹
    // 统一走 open_completed_target，路径缺失明确提示）
    const QString save_path_for_open = record->save_path;
    if (snapshot.status == falcon::TaskStatus::Completed) {
        auto* open_file_action = menu.addAction(tr("打开文件"));
        connect(open_file_action, &QAction::triggered, this,
                [this, save_path_for_open]() {
                    open_completed_target(save_path_for_open, false);
                });
    }
    auto* open_dir_action = menu.addAction(tr("打开文件夹"));
    connect(open_dir_action, &QAction::triggered, this,
            [this, save_path_for_open]() {
                open_completed_target(save_path_for_open, true);
            });

    const QString url_for_copy = QString::fromStdString(record->snapshot.url);
    auto* copy_url_action = menu.addAction(tr("复制下载链接"));
    connect(copy_url_action, &QAction::triggered, this, [url_for_copy]() {
        QApplication::clipboard()->setText(url_for_copy);
    });

    menu.addSeparator();

    // 优先级子菜单
    auto* priority_menu = menu.addMenu(tr("优先级"));
    const auto current_priority = snapshot.priority;
    const auto task_id = snapshot.id;

    auto add_priority_action = [this, priority_menu, task_id, current_priority](
                                   const char* label_text, falcon::TaskPriority priority) {
        auto* action = priority_menu->addAction(tr(label_text));
        action->setCheckable(true);
        action->setChecked(current_priority == priority);
        connect(action, &QAction::triggered, this, [this, task_id, priority]() {
            emit priority_changed(task_id, priority);
        });
    };
    add_priority_action("低", falcon::TaskPriority::Low);
    add_priority_action("普通", falcon::TaskPriority::Normal);
    add_priority_action("高", falcon::TaskPriority::High);
    add_priority_action("紧急", falcon::TaskPriority::Critical);

    menu.addSeparator();

    // 删除任务
    auto* delete_action = menu.addAction(tr("删除任务"));
    connect(delete_action, &QAction::triggered, this, [this, id = snapshot.id]() {
        emit remove_task_requested(id);
    });

    menu.exec(grid_widget_->mapToGlobal(pos));
}
void DownloadPage::sync_task_grid()
{
    // 清空现有网格内容
    while (grid_layout_->count() > 0) {
        auto* item = grid_layout_->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    // 根据视图模式过滤任务
    int column = 0;
    int row = 0;
    constexpr int kColumns = 3;  // 每行显示3个卡片

    // QHash 迭代无序——按手动排序（order 位置, id）保证卡片顺序稳定，
    // 与表格视图共用同一 task_order_（空序退化为 id 升序）
    std::vector<falcon::TaskId> id_list;
    id_list.reserve(static_cast<std::size_t>(task_records_.size()));
    for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
        id_list.push_back(static_cast<falcon::TaskId>(it.key()));
    }
    const std::vector<falcon::TaskId> sorted_ids = task_order::sort_ids(task_order_, id_list);

    for (const falcon::TaskId id : sorted_ids) {
        const TaskRecord& record = task_records_[static_cast<qulonglong>(id)];
        if (!should_show(record.snapshot)) {
            continue;
        }

        // 创建任务卡片
        auto* card = create_task_card(record);
        if (card) {
            card->setProperty("taskId", QVariant::fromValue<qulonglong>(id));
            grid_layout_->addWidget(card, row, column);

            ++column;
            if (column >= kColumns) {
                column = 0;
                ++row;
            }
        }
    }
}
QWidget* DownloadPage::create_task_card(const TaskRecord& record)
{
    const auto& snap = record.snapshot;

    auto* card = new QWidget(grid_widget_);
    card->setObjectName("taskCard");
    card->setFixedSize(280, 148);

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(12, 12, 12, 12);
    card_layout->setSpacing(8);

    // 文件名(字号/字重由 QSS #cardFileName 统一管控)
    auto* name_label = new QLabel(record.filename, card);
    name_label->setObjectName("cardFileName");
    name_label->setWordWrap(true);
    name_label->setMaximumHeight(40);
    card_layout->addWidget(name_label);

    // 进度条
    auto* progress_bar = new QProgressBar(card);
    progress_bar->setObjectName("cardProgressBar");
    progress_bar->setRange(0, 100);
    progress_bar->setTextVisible(true);
    progress_bar->setMaximumHeight(20);
    progress_bar->setValue(std::max(0, std::min(100, static_cast<int>(snap.progress * 100.0))));
    card_layout->addWidget(progress_bar);

    // 信息行
    auto* info_layout = new QHBoxLayout();
    info_layout->setSpacing(12);

    auto* size_label = new QLabel(record.size_text, card);
    size_label->setObjectName("cardInfoLabel");
    info_layout->addWidget(size_label);

    // 做种任务:速度槽位(终态恒为 "—")改显做种摘要,明细收进 tooltip
    const QString seed_summary = seed_column_text(snap);
    auto* speed_label = new QLabel(
        seed_summary.isEmpty() ? speed_display_text(snap) : seed_summary, card);
    speed_label->setObjectName("cardInfoLabel");
    if (!seed_summary.isEmpty()) {
        speed_label->setToolTip(seed_column_tooltip(snap));
    }
    info_layout->addWidget(speed_label);

    auto* status_label = new QLabel(record.status_text, card);
    status_label->setObjectName("cardInfoLabel");
    info_layout->addWidget(status_label);

    card_layout->addLayout(info_layout);

    // 操作按钮(图标化)
    auto* actions_layout = new QHBoxLayout();
    actions_layout->setSpacing(8);

    auto* pause_btn = new QToolButton(card);
    pause_btn->setObjectName("cardActionButton");
    pause_btn->setAutoRaise(true);
    if (snap.status == falcon::TaskStatus::Downloading ||
        snap.status == falcon::TaskStatus::Preparing) {
        pause_btn->setIcon(icons::themed(icons::Id::Pause, icons::ColorRole::TextSecondary));
        pause_btn->setToolTip(tr("暂停"));
        connect(pause_btn, &QToolButton::clicked, this,
                [this, id = snap.id]() { emit pause_requested(id); });
    } else if (snap.status == falcon::TaskStatus::Paused ||
               snap.status == falcon::TaskStatus::Failed) {
        pause_btn->setIcon(icons::themed(icons::Id::Play, icons::ColorRole::TextSecondary));
        pause_btn->setToolTip(tr("继续"));
        connect(pause_btn, &QToolButton::clicked, this,
                [this, id = snap.id]() { emit resume_requested(id); });
    } else {
        // 终态/排队中无暂停语义,隐藏而非禁用占位
        pause_btn->setVisible(false);
    }
    actions_layout->addWidget(pause_btn);

    auto* delete_btn = new QToolButton(card);
    delete_btn->setObjectName("cardActionButton");
    delete_btn->setAutoRaise(true);
    delete_btn->setIcon(icons::themed(icons::Id::Trash, icons::ColorRole::TextSecondary));
    delete_btn->setToolTip(tr("删除"));
    connect(delete_btn, &QToolButton::clicked, this, [this, id = snap.id]() {
        emit remove_task_requested(id);
    });
    actions_layout->addWidget(delete_btn);

    actions_layout->addStretch();
    card_layout->addLayout(actions_layout);

    // B18: 双击卡片 = 已完成任务的「打开文件」语义（事件过滤器捕获；
    // taskId 属性由 sync_task_grid 在装卡时打上，事件时读取）
    card->installEventFilter(this);

    return card;
}
