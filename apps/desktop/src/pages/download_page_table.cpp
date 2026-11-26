/**
 * @file download_page_table.cpp
 * @brief DownloadPage table view implementation
 */

#include "download_page.hpp"
#include "download_page_detail.hpp"
#include "services/task_order.hpp"
#include "utils/icon_utils.hpp"
#include "widgets/task_table_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHeaderView>
#include <QMenu>
#include <QProgressBar>
#include <QSet>
#include <QToolButton>

#include <algorithm>
#include <limits>

using namespace falcon::desktop;
using falcon::desktop::download_page_detail::kColAction;
using falcon::desktop::download_page_detail::kColFilename;
using falcon::desktop::download_page_detail::kColProgress;
using falcon::desktop::download_page_detail::kColSeed;
using falcon::desktop::download_page_detail::kColSize;
using falcon::desktop::download_page_detail::kColSpeed;
using falcon::desktop::download_page_detail::kColStatus;
using falcon::desktop::download_page_detail::kRowHeight;


void DownloadPage::create_task_table()
{
    task_table_ = new TaskTableWidget(this);
    task_table_->setColumnCount(7);
    task_table_->setHorizontalHeaderLabels({
        tr("文件名"),
        tr("进度"),
        tr("大小"),
        tr("速度"),
        tr("状态"),
        tr("做种"),
        tr("操作")
    });

    task_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    task_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    task_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    task_table_->setShowGrid(false);
    task_table_->setAlternatingRowColors(true);
    task_table_->verticalHeader()->setVisible(false);
    task_table_->horizontalHeader()->setStretchLastSection(false);
    task_table_->horizontalHeader()->setHighlightSections(false);
    // 表头与内容统一左对齐(扫描友好,表头不与内容错位)
    task_table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // 启用右键菜单
    task_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(task_table_, &QTableWidget::customContextMenuRequested,
            this, &DownloadPage::show_context_menu);

    // B18: 已完成任务双击 = 打开文件（迅雷语义；下载中视图双击无动作不变）
    connect(task_table_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
                if (view_mode_ != DownloadViewMode::Completed) {
                    return;
                }
                if (const auto* record = record_at_row(row)) {
                    handle_completed_double_click(*record);
                }
            });

    // 拖拽排序：落定换位 + 会话期间挂起快照刷新
    connect(task_table_, &TaskTableWidget::reorderRequested,
            this, &DownloadPage::on_table_reorder);
    connect(task_table_, &TaskTableWidget::dragSessionChanged,
            this, &DownloadPage::on_drag_session);

    // 列宽:文件名列弹性伸缩跟随窗口,其余列固定内容宽
    task_table_->horizontalHeader()->setSectionResizeMode(kColFilename, QHeaderView::Stretch);
    task_table_->setColumnWidth(kColProgress, 200);  // 进度(条 + 百分比)
    task_table_->setColumnWidth(kColSize, 100);      // 大小
    task_table_->setColumnWidth(kColSpeed, 100);     // 速度
    task_table_->setColumnWidth(kColStatus, 88);     // 状态
    task_table_->setColumnWidth(kColSeed, 200);      // 做种(状态 · ratio · 时长;单行不折)
    task_table_->setColumnWidth(kColAction, 80);     // 操作

    task_table_->setObjectName("taskTable");

    // 初始列显隐(构造默认视图 Downloading → 隐藏做种列)
    update_column_visibility();
}
void DownloadPage::set_view_mode(DownloadViewMode mode)
{
    const bool changed = (view_mode_ != mode);
    view_mode_ = mode;
    update_header_for_mode();
    // 列显隐随视图切换（重插行之前生效，避免错误列闪现）
    update_column_visibility();

    // 同步页头分段切换器选中态(setChecked 不触发 clicked,无递归)
    if (view_segment_downloading_ && view_segment_completed_) {
        view_segment_downloading_->setChecked(mode == DownloadViewMode::Downloading);
        view_segment_completed_->setChecked(mode == DownloadViewMode::Completed);
    }

    // 清空并按新过滤条件重新加载任务
    task_table_->setRowCount(0);
    row_by_task_id_.clear();
    rerender();

    // B20 双向联动（页签 → 侧栏方向）：视图模式实际变化才发信号，
    // 侧栏高亮由 MainWindow 接线跟随
    if (changed) {
        emit view_mode_changed(mode);
    }
}
void DownloadPage::sync_task_row(const TaskRecord& record)
{
    const qulonglong key = static_cast<qulonglong>(record.snapshot.id);

    if (!should_show(record.snapshot)) {
        remove_task_row(key);
        update_empty_state();
        return;
    }

    if (row_by_task_id_.contains(key)) {
        return;  // 行已存在，动态列由 rerender 统一更新
    }

    // 按显示顺序定位插入点——task_records_ 是 QHash 无序遍历,
    // 直接 append 会让行序随机(每次全量重建都可能变化)。
    // 排序键 = (order 位置, id)：未入 order 的任务排最后,之间按 id 升序
    const int kUnknownPos = std::numeric_limits<int>::max();
    const int key_pos = display_pos_.value(key, kUnknownPos);
    int row = task_table_->rowCount();
    for (auto it = row_by_task_id_.cbegin(); it != row_by_task_id_.cend(); ++it) {
        const int other_pos = display_pos_.value(it.key(), kUnknownPos);
        const bool other_above = (other_pos != key_pos)
                                     ? (other_pos < key_pos)
                                     : (it.key() < key);
        if (other_above) {
            continue;
        }
        row = std::min(row, it.value());
    }
    task_table_->insertRow(row);
    // 中间插入把后续行往下推,既有行号同步 +1(与 remove_task_row 的 -1 对称)
    for (auto it = row_by_task_id_.begin(); it != row_by_task_id_.end(); ++it) {
        if (it.value() >= row) {
            it.value() += 1;
        }
    }
    task_table_->setRowHeight(row, kRowHeight);
    row_by_task_id_.insert(key, row);

    // 文件名
    auto* name_item = new QTableWidgetItem(record.filename);
    name_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    name_item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(key));
    task_table_->setItem(row, 0, name_item);

    // 进度 = 进度条 + 右侧百分比标签(条内不放文字——6px 条高压不下 11px 文字)
    auto* progress_widget = new QWidget(this);
    auto* progress_layout = new QHBoxLayout(progress_widget);
    progress_layout->setContentsMargins(0, 0, 8, 0);
    progress_layout->setSpacing(8);

    auto* progress_bar = new QProgressBar(progress_widget);
    progress_bar->setRange(0, 100);
    progress_bar->setValue(0);
    progress_bar->setTextVisible(false);
    progress_bar->setObjectName("taskProgressBar");
    progress_layout->addWidget(progress_bar, 1);

    auto* pct_label = new QLabel("0%", progress_widget);
    pct_label->setObjectName("progressPctLabel");
    pct_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progress_layout->addWidget(pct_label);

    task_table_->setCellWidget(row, 1, progress_widget);

    // 大小
    auto* size_item = new QTableWidgetItem(record.size_text);
    size_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    task_table_->setItem(row, 2, size_item);

    // 速度
    auto* speed_item = new QTableWidgetItem(speed_display_text(record.snapshot));
    speed_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    task_table_->setItem(row, 3, speed_item);

    // 状态
    auto* status_item = new QTableWidgetItem(record.status_text);
    status_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    task_table_->setItem(row, 4, status_item);

    // 做种(状态 · ratio · 时长;上传/下载量收进 tooltip)
    auto* seed_item = new QTableWidgetItem(seed_column_text(record.snapshot));
    seed_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    task_table_->setItem(row, 5, seed_item);

    // 操作按钮(图标化;暂停⇄继续按状态切换,click 统一走状态分发)
    auto* actions_widget = new QWidget(this);
    auto* actions_layout = new QHBoxLayout(actions_widget);
    actions_layout->setContentsMargins(4, 0, 4, 0);
    actions_layout->setSpacing(4);

    // 暂停⇄继续按状态切换;终态(已完成/已取消)与排队中无暂停语义,按钮隐藏
    const auto row_status = record.snapshot.status;
    const bool pausable = row_status == falcon::TaskStatus::Downloading ||
                          row_status == falcon::TaskStatus::Preparing;
    const bool resumable = row_status == falcon::TaskStatus::Paused ||
                           row_status == falcon::TaskStatus::Failed;
    auto* pause_btn = new QToolButton(actions_widget);
    pause_btn->setObjectName("rowActionButton");
    pause_btn->setIcon(icons::themed(resumable ? icons::Id::Play : icons::Id::Pause,
                                     icons::ColorRole::TextSecondary));
    pause_btn->setAutoRaise(true);
    pause_btn->setToolTip(resumable ? tr("继续") : tr("暂停"));
    pause_btn->setProperty("taskId", QVariant::fromValue<qulonglong>(key));
    pause_btn->setVisible(pausable || resumable);
    connect(pause_btn, &QToolButton::clicked, this, &DownloadPage::on_pause_selected);
    actions_layout->addWidget(pause_btn);

    auto* delete_btn = new QToolButton(actions_widget);
    delete_btn->setObjectName("rowActionButton");
    delete_btn->setIcon(icons::themed(icons::Id::Trash, icons::ColorRole::TextSecondary));
    delete_btn->setAutoRaise(true);
    delete_btn->setToolTip(tr("删除"));
    delete_btn->setProperty("taskId", QVariant::fromValue<qulonglong>(key));
    connect(delete_btn, &QToolButton::clicked, this, &DownloadPage::on_delete_selected);
    actions_layout->addWidget(delete_btn);

    task_table_->setCellWidget(row, 6, actions_widget);
    update_row_texts(row, record);
    update_empty_state();
}
void DownloadPage::update_row_texts(int row, const TaskRecord& record)
{
    const auto& snap = record.snapshot;
    const int pct = static_cast<int>(snap.progress * 100.0);

    if (auto* name_item = task_table_->item(row, 0)) {
        name_item->setText(record.filename);
    }
    if (auto* size_item = task_table_->item(row, 2)) {
        size_item->setText(record.size_text);
    }
    if (auto* speed_item = task_table_->item(row, 3)) {
        speed_item->setText(speed_display_text(snap));
    }
    if (auto* status_item = task_table_->item(row, 4)) {
        status_item->setText(record.status_text);
    }
    if (auto* seed_item = task_table_->item(row, 5)) {
        seed_item->setText(seed_column_text(snap));
        seed_item->setToolTip(seed_column_tooltip(snap));
    }
    if (auto* widget = task_table_->cellWidget(row, 1)) {
        if (auto* bar = widget->findChild<QProgressBar*>()) {
            bar->setValue(std::max(0, std::min(100, pct)));
        }
        if (auto* pct_label = widget->findChild<QLabel*>()) {
            pct_label->setText(QString::number(pct) + QLatin1Char('%'));
        }
    }
}
void DownloadPage::remove_task_row(qulonglong key)
{
    if (!row_by_task_id_.contains(key)) {
        return;
    }
    const int row = row_by_task_id_.value(key);
    task_table_->removeRow(row);
    row_by_task_id_.remove(key);
    for (auto it = row_by_task_id_.begin(); it != row_by_task_id_.end(); ++it) {
        if (it.value() > row) {
            it.value() -= 1;
        }
    }
}
void DownloadPage::set_task_order(const std::vector<falcon::TaskId>& order)
{
    task_order_ = order;
    // 启动时装载可能早于首份快照(此时无行可排),rerender 时自然生效
    if (task_table_ && task_table_->rowCount() > 0) {
        rebuild_table_rows();
    }
}
void DownloadPage::rebuild_display_positions()
{
    display_pos_.clear();
    std::vector<falcon::TaskId> id_list;
    id_list.reserve(static_cast<std::size_t>(task_records_.size()));
    for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
        id_list.push_back(static_cast<falcon::TaskId>(it.key()));
    }
    const std::vector<falcon::TaskId> sorted =
        task_order::sort_ids(task_order_, id_list);
    display_pos_.reserve(static_cast<int>(sorted.size()) * 2);
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        display_pos_.insert(static_cast<qulonglong>(sorted[i]),
                            static_cast<int>(i));
    }
}
void DownloadPage::rebuild_table_rows()
{
    // 全量重插(与 set_view_mode 同款模式)——就地移动行的方案对
    // cellWidget(进度条/操作钮)搬迁过于侵入,收益不成比例
    task_table_->setRowCount(0);
    row_by_task_id_.clear();
    rerender();
}
void DownloadPage::on_table_reorder(int from_row, int to_row)
{
    // 当前可见行(含过滤/视图模式)的 id 序列——表格外(被过滤或另一
    // 视图)的任务保持各自位置不动,只有可见子集按新序重排
    std::vector<falcon::TaskId> visible;
    const int rows = task_table_->rowCount();
    visible.reserve(static_cast<std::size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        if (const auto* item = task_table_->item(r, 0)) {
            visible.push_back(static_cast<falcon::TaskId>(
                item->data(Qt::UserRole).toULongLong()));
        }
    }

    const std::vector<falcon::TaskId> moved = task_order::move_to(
        visible, static_cast<std::size_t>(from_row), static_cast<std::size_t>(to_row));
    task_order_ = task_order::apply_drag(task_order_, moved);

    // 剪掉已消失任务的 id:防 QSettings 无限增长 + 限制陈旧 id 影响面
    std::vector<falcon::TaskId> pruned;
    pruned.reserve(task_order_.size());
    for (const falcon::TaskId id : task_order_) {
        if (task_records_.contains(static_cast<qulonglong>(id))) {
            pruned.push_back(id);
        }
    }
    task_order_ = std::move(pruned);

    rebuild_table_rows();
    emit task_order_changed(
        QString::fromStdString(task_order::serialize(task_order_)));
}
void DownloadPage::on_drag_session(bool active)
{
    if (drag_active_ == active) {
        return;
    }
    drag_active_ = active;
    // 会话结束:补刷挂起的最后一份快照(期间可能有多份,只留最新即可)
    if (!active && has_pending_tasks_) {
        has_pending_tasks_ = false;
        update_tasks(pending_tasks_);
    }
}
const DownloadPage::TaskRecord* DownloadPage::record_by_id(qulonglong key) const
{
    auto it = task_records_.constFind(key);
    return it == task_records_.constEnd() ? nullptr : &it.value();
}

const DownloadPage::TaskRecord* DownloadPage::selected_record() const
{
    if (!task_table_ || !task_table_->selectionModel()) {
        return nullptr;
    }

    const auto selected_rows = task_table_->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        return nullptr;
    }

    auto* item = task_table_->item(selected_rows.first().row(), 0);
    if (!item) {
        return nullptr;
    }

    return record_by_id(item->data(Qt::UserRole).toULongLong());
}
const DownloadPage::TaskRecord* DownloadPage::record_from_sender() const
{
    const QObject* sender_object = sender();
    if (!sender_object) {
        return nullptr;
    }

    const QVariant task_id_var = sender_object->property("taskId");
    if (!task_id_var.isValid()) {
        return nullptr;
    }

    return record_by_id(task_id_var.toULongLong());
}
const DownloadPage::TaskRecord* DownloadPage::record_at_row(int row) const
{
    if (row < 0 || row >= task_table_->rowCount()) {
        return nullptr;
    }

    auto* item = task_table_->item(row, 0);
    if (!item) {
        return nullptr;
    }

    return record_by_id(item->data(Qt::UserRole).toULongLong());
}
void DownloadPage::update_column_visibility()
{
    if (!task_table_) {
        return;
    }
    const bool completed = (view_mode_ == DownloadViewMode::Completed);
    // 已完成视图：进度（恒 100%）与速度（恒 "—"）是无信息量噪音 → 隐藏；
    // 下载中视图：做种列恒空（做种任务只出现在已完成视图）→ 隐藏
    task_table_->setColumnHidden(kColProgress, completed);
    task_table_->setColumnHidden(kColSpeed, completed);
    task_table_->setColumnHidden(kColSeed, !completed);
}
void DownloadPage::on_more_options_clicked()
{
    // B16：批量菜单按当前 tab 区分——「下载中」= 启停/取消类，
    // 「已完成」= 记录管理类（绝无开始/暂停）。菜单每次点击即席构建，
    // 切 tab 后下一次点击自然换套，无需增量维护。
    QMenu menu(this);

    // 选中类条目统一按值捕获（菜单 exec 期间快照表可能被 update_tasks
    // 整体重建，指针不可跨 exec 存活）；网格视图无选中模型，无选中时
    // 这些条目禁用（诚实呈现，而非点了没反应）
    const TaskRecord* selected = selected_record();
    const qulonglong selected_id = selected ? selected->snapshot.id : 0;
    const QString selected_url =
        selected ? QString::fromStdString(selected->snapshot.url) : QString();
    const QString selected_dir = selected
        ? QFileInfo(selected->save_path.isEmpty()
                        ? QString::fromStdString(selected->snapshot.url)
                        : selected->save_path).absolutePath()
        : QString();

    if (view_mode_ == DownloadViewMode::Completed) {
        // ---- 已完成：清空完成记录 / 删除选中 / 重新下载 / 打开所在文件夹 ----
        auto* clear_finished_action = menu.addAction(tr("清空完成记录"));
        connect(clear_finished_action, &QAction::triggered, this, [this]() {
            emit remove_finished_tasks_requested();
        });

        auto* delete_selected_action = menu.addAction(tr("删除选中"));
        delete_selected_action->setEnabled(selected_id != 0);
        connect(delete_selected_action, &QAction::triggered, this, [this, selected_id]() {
            emit remove_task_requested(static_cast<falcon::TaskId>(selected_id));
        });

        menu.addSeparator();

        auto* redownload_action = menu.addAction(tr("重新下载"));
        redownload_action->setEnabled(selected_id != 0 && !selected_url.isEmpty());
        connect(redownload_action, &QAction::triggered, this,
                [this, selected_url]() { emit redownload_requested(selected_url); });

        auto* open_dir_action = menu.addAction(tr("打开所在文件夹"));
        open_dir_action->setEnabled(selected_id != 0 && !selected_dir.isEmpty());
        connect(open_dir_action, &QAction::triggered, this, [selected_dir]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(selected_dir));
        });
    } else {
        // ---- 下载中：全部开始 / 全部暂停 / 全部取消 / 删除任务 / 清空列表 ----
        auto* start_all_action = menu.addAction(tr("全部开始"));
        connect(start_all_action, &QAction::triggered, this, [this]() {
            for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
                const auto& snap = it.value().snapshot;
                if (snap.status == falcon::TaskStatus::Paused ||
                    snap.status == falcon::TaskStatus::Failed ||
                    snap.status == falcon::TaskStatus::Pending) {
                    emit resume_requested(snap.id);
                }
            }
        });

        auto* pause_all_action = menu.addAction(tr("全部暂停"));
        connect(pause_all_action, &QAction::triggered, this, [this]() {
            for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
                const auto& snap = it.value().snapshot;
                if (snap.status == falcon::TaskStatus::Downloading ||
                    snap.status == falcon::TaskStatus::Preparing) {
                    emit pause_requested(snap.id);
                }
            }
        });

        // 全部取消：活动/暂停/排队任务移出列表并进回收站（有回收站时
        // 先取消再移除，可恢复；daemon 路径 aria2.remove 本就对活动
        // 任务生效）。与「清空列表」的差别：不动失败任务。
        auto* cancel_all_action = menu.addAction(tr("全部取消"));
        connect(cancel_all_action, &QAction::triggered, this, [this]() {
            for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
                const auto& snap = it.value().snapshot;
                if (snap.status == falcon::TaskStatus::Pending ||
                    snap.status == falcon::TaskStatus::Preparing ||
                    snap.status == falcon::TaskStatus::Downloading ||
                    snap.status == falcon::TaskStatus::Paused) {
                    emit remove_task_requested(snap.id);
                }
            }
        });

        menu.addSeparator();

        auto* delete_selected_action = menu.addAction(tr("删除任务"));
        delete_selected_action->setEnabled(selected_id != 0);
        connect(delete_selected_action, &QAction::triggered, this, [this, selected_id]() {
            emit remove_task_requested(static_cast<falcon::TaskId>(selected_id));
        });

        // 清空列表：下载中视图全部可见任务（含失败）移出列表并回收
        auto* clear_list_action = menu.addAction(tr("清空列表"));
        connect(clear_list_action, &QAction::triggered, this, [this]() {
            for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
                if (should_show(it.value().snapshot)) {
                    emit remove_task_requested(it.value().snapshot.id);
                }
            }
        });
    }

    menu.exec(more_button_->mapToGlobal(more_button_->rect().bottomLeft()));
}
void DownloadPage::on_pause_selected()
{
    const auto* record = record_from_sender();
    if (!record) {
        record = selected_record();
    }
    if (!record) {
        return;
    }

    const auto status = record->snapshot.status;
    if (status == falcon::TaskStatus::Downloading ||
        status == falcon::TaskStatus::Preparing) {
        emit pause_requested(record->snapshot.id);
    } else if (status == falcon::TaskStatus::Paused ||
               status == falcon::TaskStatus::Failed) {
        emit resume_requested(record->snapshot.id);
    }
}

void DownloadPage::on_resume_selected()
{
    if (const auto* record = selected_record()) {
        emit resume_requested(record->snapshot.id);
    }
}
void DownloadPage::on_delete_selected()
{
    const auto* record = record_from_sender();
    if (!record) {
        record = selected_record();
    }
    if (record) {
        emit remove_task_requested(record->snapshot.id);
    }
}
void DownloadPage::show_context_menu(const QPoint& pos)
{
    const auto* item = task_table_->itemAt(pos);
    if (!item) {
        return;
    }

    const auto* record = record_at_row(item->row());
    if (!record) {
        return;
    }

    QMenu menu(this);

    // 根据任务状态显示不同菜单项
    const auto status = record->snapshot.status;

    // 暂停/继续
    if (status == falcon::TaskStatus::Downloading ||
        status == falcon::TaskStatus::Preparing) {
        auto* pause_action = menu.addAction(tr("暂停"));
        connect(pause_action, &QAction::triggered, this,
                [this, id = record->snapshot.id]() { emit pause_requested(id); });
    } else if (status == falcon::TaskStatus::Paused ||
               status == falcon::TaskStatus::Failed) {
        auto* resume_action = menu.addAction(tr("继续"));
        connect(resume_action, &QAction::triggered, this,
                [this, id = record->snapshot.id]() { emit resume_requested(id); });
    }

    // 停止做种(做种中的 BitTorrent 任务;终态停止,任务保持已完成)
    if (record->snapshot.seeding_active) {
        auto* stop_seed_action = menu.addAction(tr("停止做种"));
        connect(stop_seed_action, &QAction::triggered, this,
                [this, id = record->snapshot.id]() { emit stop_seeding_requested(id); });
    }

    menu.addSeparator();

    // 打开文件/文件夹与复制链接按值捕获快照内容：菜单 exec 期间任务表可能被
    // update_tasks 整体重建，不能捕获指向 task_records_ 的指针
    const QString save_path_for_open = record->save_path;
    if (record->snapshot.status == falcon::TaskStatus::Completed) {
        // B18: 已完成任务右键补「打开文件」（系统默认程序）
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
    const auto current_priority = record->snapshot.priority;
    const auto task_id = record->snapshot.id;

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

    // 删除任务（按值捕获：菜单 exec 期间行号可能被 update_tasks 重排）
    const auto menu_task_id = record->snapshot.id;
    auto* delete_action = menu.addAction(tr("删除任务"));
    connect(delete_action, &QAction::triggered, this,
            [this, menu_task_id]() { emit remove_task_requested(menu_task_id); });

    menu.exec(task_table_->mapToGlobal(pos));
}
