/**
 * @file download_page.cpp
 * @brief Download Page Implementation (Xunlei-style)
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "download_page.hpp"

#include <QHeaderView>
#include <QProgressBar>
#include <QStyle>

namespace falcon::desktop {

namespace {
constexpr int kRowHeight = 56;
} // namespace

DownloadPage::DownloadPage(QWidget* parent)
    : QWidget(parent)
    , view_mode_(DownloadViewMode::Downloading)
    , header_layout_(nullptr)
    , status_label_(nullptr)
    , new_task_button_(nullptr)
    , refresh_button_(nullptr)
    , view_toggle_button_(nullptr)
    , more_button_(nullptr)
    , task_table_(nullptr)
    , pause_button_(nullptr)
    , delete_button_(nullptr)
    , refresh_timer_(nullptr)
{
    setup_ui();

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(500);
    connect(refresh_timer_, &QTimer::timeout, this, &DownloadPage::refresh_engine_tasks);
    refresh_timer_->start();
}

DownloadPage::~DownloadPage() = default;

void DownloadPage::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(16, 12, 16, 16);
    main_layout->setSpacing(0);

    create_header_bar();
    main_layout->addLayout(header_layout_);

    main_layout->addSpacing(8);

    create_task_table();
    main_layout->addWidget(task_table_);

    // 底部推广区域（预留）
    main_layout->addStretch();
}

void DownloadPage::create_header_bar()
{
    header_layout_ = new QHBoxLayout();
    header_layout_->setSpacing(12);

    // 状态标题
    status_label_ = new QLabel(tr("已暂停"), this);
    status_label_->setObjectName("headerLabel");
    auto title_font = status_label_->font();
    title_font.setPointSize(14);
    title_font.setBold(true);
    status_label_->setFont(title_font);
    header_layout_->addWidget(status_label_);

    header_layout_->addStretch();

    // 新建按钮
    new_task_button_ = new QPushButton(tr("+ 新建"), this);
    new_task_button_->setObjectName("primaryButton");
    connect(new_task_button_, &QPushButton::clicked, this, &DownloadPage::on_new_task_clicked);
    header_layout_->addWidget(new_task_button_);

    // 刷新按钮
    refresh_button_ = new QPushButton(tr("🔄"), this);
    refresh_button_->setObjectName("toolButton");
    refresh_button_->setFixedSize(32, 32);
    refresh_button_->setToolTip(tr("刷新"));
    connect(refresh_button_, &QPushButton::clicked, this, &DownloadPage::on_refresh_clicked);
    header_layout_->addWidget(refresh_button_);

    // 视图切换按钮
    view_toggle_button_ = new QPushButton(tr("▦"), this);
    view_toggle_button_->setObjectName("toolButton");
    view_toggle_button_->setFixedSize(32, 32);
    view_toggle_button_->setToolTip(tr("切换视图"));
    connect(view_toggle_button_, &QPushButton::clicked, this, &DownloadPage::on_view_toggle_clicked);
    header_layout_->addWidget(view_toggle_button_);

    // 更多选项按钮
    more_button_ = new QPushButton(tr("⋯"), this);
    more_button_->setObjectName("toolButton");
    more_button_->setFixedSize(32, 32);
    more_button_->setToolTip(tr("更多选项"));
    connect(more_button_, &QPushButton::clicked, this, &DownloadPage::on_more_options_clicked);
    header_layout_->addWidget(more_button_);
}

void DownloadPage::create_task_table()
{
    task_table_ = new QTableWidget(this);
    task_table_->setColumnCount(6);
    task_table_->setHorizontalHeaderLabels({
        tr("文件名"),
        tr("进度"),
        tr("大小"),
        tr("速度"),
        tr("状态"),
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

    // 设置列宽
    task_table_->setColumnWidth(0, 350);  // 文件名
    task_table_->setColumnWidth(1, 180);  // 进度
    task_table_->setColumnWidth(2, 100);  // 大小
    task_table_->setColumnWidth(3, 100);  // 速度
    task_table_->setColumnWidth(4, 80);   // 状态
    task_table_->setColumnWidth(5, 80);   // 操作

    task_table_->setObjectName("taskTable");

    connect(task_table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { update_action_buttons(); });
}

void DownloadPage::set_view_mode(DownloadViewMode mode)
{
    view_mode_ = mode;
    update_header_for_mode();

    // 清空并重新加载任务
    task_table_->setRowCount(0);
    row_by_task_id_.clear();

    // 根据模式重新填充任务
    for (auto it = task_records_.begin(); it != task_records_.end(); ++it) {
        sync_task_tables(it.value().task);
    }
}

void DownloadPage::update_header_for_mode()
{
    switch (view_mode_) {
        case DownloadViewMode::Downloading:
            status_label_->setText(tr("下载中"));
            break;
        case DownloadViewMode::Completed:
            status_label_->setText(tr("已完成"));
            break;
        case DownloadViewMode::CloudAdd:
            status_label_->setText(tr("云添加"));
            break;
    }
}

void DownloadPage::update_action_buttons()
{
    // TODO: 根据选中的任务状态更新按钮状态
}

falcon::DownloadTask::Ptr DownloadPage::selected_task() const
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

    const qulonglong key = item->data(Qt::UserRole).toULongLong();
    auto record_it = task_records_.constFind(key);
    if (record_it == task_records_.constEnd()) {
        return nullptr;
    }

    return record_it->task;
}

void DownloadPage::on_new_task_clicked()
{
    emit new_task_requested();
}

void DownloadPage::on_refresh_clicked()
{
    refresh_engine_tasks();
}

void DownloadPage::on_view_toggle_clicked()
{
    // TODO: 实现视图切换（列表/网格）
}

void DownloadPage::on_more_options_clicked()
{
    // TODO: 显示更多选项菜单
}

void DownloadPage::on_pause_selected()
{
    const auto task = selected_task();
    if (task) {
        (void)task->pause();
    }
}

void DownloadPage::on_resume_selected()
{
    const auto task = selected_task();
    if (task) {
        (void)task->resume();
    }
}

void DownloadPage::on_delete_selected()
{
    const auto task = selected_task();
    if (task) {
        emit remove_task_requested(task->id());
    }
}

QString DownloadPage::format_bytes(uint64_t bytes)
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

QString DownloadPage::format_speed(uint64_t bytes_per_second)
{
    if (bytes_per_second == 0) {
        return "0 B/s";
    }
    return format_bytes(bytes_per_second) + "/s";
}

void DownloadPage::add_engine_task(const falcon::DownloadTask::Ptr& task)
{
    if (!task) {
        return;
    }

    const qulonglong key = static_cast<qulonglong>(task->id());
    if (task_records_.contains(key)) {
        return;
    }

    const QString filename = !task->options().output_filename.empty()
        ? QString::fromStdString(task->options().output_filename)
        : QString::fromStdString(task->file_info().filename);

    TaskRecord record;
    record.task = task;
    record.filename = filename.isEmpty() ? tr("(unknown)") : filename;
    record.save_path = QString::fromStdString(task->options().output_directory);
    record.size_text = "-";
    record.status_text = QString::fromUtf8(falcon::to_string(task->status()));
    record.error_text = QString::fromStdString(task->error_message());
    task_records_.insert(key, record);

    sync_task_tables(task);
}

void DownloadPage::refresh_engine_tasks()
{
    for (auto it = task_records_.begin(); it != task_records_.end(); ++it) {
        TaskRecord& record = it.value();
        const auto& task = record.task;
        if (!task) {
            continue;
        }

        const uint64_t total = static_cast<uint64_t>(task->total_bytes());
        const uint64_t speed = static_cast<uint64_t>(task->speed());
        const int pct = static_cast<int>(task->progress() * 100.0f);
        record.size_text = total > 0 ? format_bytes(total) : "-";
        record.status_text = QString::fromUtf8(falcon::to_string(task->status()));
        record.error_text = QString::fromStdString(task->error_message());

        sync_task_tables(task);

        auto row_it = row_by_task_id_.find(it.key());
        if (row_it == row_by_task_id_.end()) {
            continue;
        }

        const int row = row_it.value();

        // 更新文件名
        if (auto* name_item = task_table_->item(row, 0)) {
            name_item->setText(record.filename);
        }

        // 更新大小
        if (auto* size_item = task_table_->item(row, 2)) {
            size_item->setText(record.size_text);
        }

        // 更新速度
        if (auto* speed_item = task_table_->item(row, 3)) {
            speed_item->setText(format_speed(speed));
        }

        // 更新状态
        if (auto* status_item = task_table_->item(row, 4)) {
            status_item->setText(record.status_text);
        }

        // 更新进度条
        if (auto* widget = task_table_->cellWidget(row, 1)) {
            if (auto* bar = qobject_cast<QProgressBar*>(widget)) {
                bar->setValue(std::max(0, std::min(100, pct)));
            }
        }
    }

    update_action_buttons();
}

void DownloadPage::sync_task_tables(const falcon::DownloadTask::Ptr& task)
{
    if (!task) {
        return;
    }

    const qulonglong key = static_cast<qulonglong>(task->id());
    auto record_it = task_records_.find(key);
    if (record_it == task_records_.end()) {
        return;
    }

    const TaskRecord& record = record_it.value();

    // 根据视图模式过滤任务
    const auto status = task->status();
    bool should_show = false;

    switch (view_mode_) {
        case DownloadViewMode::Downloading:
            should_show = (status == falcon::TaskStatus::Downloading ||
                          status == falcon::TaskStatus::Preparing ||
                          status == falcon::TaskStatus::Paused ||
                          status == falcon::TaskStatus::Pending);
            break;
        case DownloadViewMode::Completed:
            should_show = (status == falcon::TaskStatus::Completed);
            break;
        case DownloadViewMode::CloudAdd:
            should_show = false;  // TODO: 实现云添加任务过滤
            break;
    }

    if (!should_show) {
        // 移除行
        if (row_by_task_id_.contains(key)) {
            const int row = row_by_task_id_.value(key);
            task_table_->removeRow(row);
            row_by_task_id_.remove(key);
            for (auto it = row_by_task_id_.begin(); it != row_by_task_id_.end(); ++it) {
                if (it.value() > row) {
                    it.value() -= 1;
                }
            }
        }
        return;
    }

    // 添加或更新行
    if (row_by_task_id_.contains(key)) {
        return;  // 行已存在，由 refresh_engine_tasks 更新
    }

    const int row = task_table_->rowCount();
    task_table_->insertRow(row);
    task_table_->setRowHeight(row, kRowHeight);
    row_by_task_id_.insert(key, row);

    // 文件名（带图标）
    auto* name_item = new QTableWidgetItem("📄 " + record.filename);
    name_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    name_item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(key));
    task_table_->setItem(row, 0, name_item);

    // 进度条
    auto* progress_bar = new QProgressBar(this);
    progress_bar->setRange(0, 100);
    progress_bar->setValue(0);
    progress_bar->setTextVisible(true);
    progress_bar->setObjectName("taskProgressBar");
    task_table_->setCellWidget(row, 1, progress_bar);

    // 大小
    auto* size_item = new QTableWidgetItem(record.size_text);
    size_item->setTextAlignment(Qt::AlignCenter);
    task_table_->setItem(row, 2, size_item);

    // 速度
    auto* speed_item = new QTableWidgetItem("0 B/s");
    speed_item->setTextAlignment(Qt::AlignCenter);
    task_table_->setItem(row, 3, speed_item);

    // 状态
    auto* status_item = new QTableWidgetItem(record.status_text);
    status_item->setTextAlignment(Qt::AlignCenter);
    task_table_->setItem(row, 4, status_item);

    // 操作按钮
    auto* actions_widget = new QWidget(this);
    auto* actions_layout = new QHBoxLayout(actions_widget);
    actions_layout->setContentsMargins(4, 0, 4, 0);
    actions_layout->setSpacing(4);

    auto* pause_btn = new QPushButton(tr("⏸"), actions_widget);
    pause_btn->setObjectName("rowActionButton");
    pause_btn->setFixedSize(24, 24);
    pause_btn->setToolTip(tr("暂停"));
    connect(pause_btn, &QPushButton::clicked, this, &DownloadPage::on_pause_selected);
    actions_layout->addWidget(pause_btn);

    auto* delete_btn = new QPushButton(tr("✕"), actions_widget);
    delete_btn->setObjectName("rowActionButton");
    delete_btn->setFixedSize(24, 24);
    delete_btn->setToolTip(tr("删除"));
    connect(delete_btn, &QPushButton::clicked, this, &DownloadPage::on_delete_selected);
    actions_layout->addWidget(delete_btn);

    task_table_->setCellWidget(row, 5, actions_widget);
}

} // namespace falcon::desktop
