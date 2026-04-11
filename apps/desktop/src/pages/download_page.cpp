/**
 * @file download_page.cpp
 * @brief Download Management Page Implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "download_page.hpp"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QProgressBar>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

#include <falcon/event_listener.hpp>

namespace falcon::desktop {

DownloadPage::DownloadPage(QWidget* parent)
    : QWidget(parent)
    , tab_widget_(nullptr)
    , downloading_table_(nullptr)
    , completed_table_(nullptr)
    , trash_table_(nullptr)
    , history_table_(nullptr)
    , new_task_button_(nullptr)
    , pause_button_(nullptr)
    , resume_button_(nullptr)
    , cancel_button_(nullptr)
    , delete_button_(nullptr)
    , clean_button_(nullptr)
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
    main_layout->setContentsMargins(32, 32, 32, 32);
    main_layout->setSpacing(24);

    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(16);

    auto* title_label = new QLabel(tr("Downloads"), this);
    auto title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
    header_layout->addWidget(title_label);
    header_layout->addStretch();
    main_layout->addLayout(header_layout);

    main_layout->addWidget(create_toolbar());
    create_tab_widget();
    main_layout->addWidget(tab_widget_);
}

QWidget* DownloadPage::create_toolbar()
{
    auto* toolbar = new QWidget(this);
    auto* layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    new_task_button_ = new QPushButton(tr("New Task"), toolbar);
    new_task_button_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    new_task_button_->setCursor(Qt::PointingHandCursor);
    new_task_button_->setMinimumHeight(36);
    connect(new_task_button_, &QPushButton::clicked, this, &DownloadPage::new_task_requested);
    layout->addWidget(new_task_button_);

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(spacer);

    auto* actions_container = new QWidget(toolbar);
    auto* actions_layout = new QHBoxLayout(actions_container);
    actions_layout->setContentsMargins(0, 0, 0, 0);
    actions_layout->setSpacing(4);

    auto create_action_btn = [this, actions_layout](QPushButton*& btn, QStyle::StandardPixmap icon,
                                                     const QString& tooltip) {
        btn = new QPushButton(this);
        btn->setEnabled(false);
        btn->setIcon(style()->standardIcon(icon));
        btn->setFlat(true);
        btn->setToolTip(tooltip);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedSize(32, 32);
        actions_layout->addWidget(btn);
    };

    create_action_btn(pause_button_, QStyle::SP_MediaPause, tr("Pause"));
    create_action_btn(resume_button_, QStyle::SP_MediaPlay, tr("Resume"));
    create_action_btn(cancel_button_, QStyle::SP_BrowserStop, tr("Cancel"));

    auto* line = new QFrame(actions_container);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setFixedHeight(20);
    actions_layout->addWidget(line);

    create_action_btn(delete_button_, QStyle::SP_TrashIcon, tr("Delete"));

    clean_button_ = new QPushButton(this);
    clean_button_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    clean_button_->setFlat(true);
    clean_button_->setToolTip(tr("Clear Completed"));
    clean_button_->setCursor(Qt::PointingHandCursor);
    clean_button_->setFixedSize(32, 32);
    actions_layout->addWidget(clean_button_);

    connect(pause_button_, &QPushButton::clicked, this, &DownloadPage::pause_selected_task);
    connect(resume_button_, &QPushButton::clicked, this, &DownloadPage::resume_selected_task);
    connect(cancel_button_, &QPushButton::clicked, this, &DownloadPage::cancel_selected_task);
    connect(delete_button_, &QPushButton::clicked, this, &DownloadPage::delete_selected_task);
    connect(clean_button_, &QPushButton::clicked, this, &DownloadPage::clear_finished_tasks);

    layout->addWidget(actions_container);
    return toolbar;
}

void DownloadPage::create_tab_widget()
{
    tab_widget_ = new QTabWidget(this);
    tab_widget_->setTabPosition(QTabWidget::North);
#ifdef Q_OS_MAC
    tab_widget_->setDocumentMode(true);
#else
    tab_widget_->setDocumentMode(false);
#endif

    create_downloading_tab();
    create_completed_tab();
    create_trash_tab();
    create_history_tab();

    connect(tab_widget_, &QTabWidget::currentChanged, this, [this]() {
        update_action_buttons();
    });
}

void DownloadPage::create_downloading_tab()
{
    downloading_table_ = new QTableWidget(this);
    downloading_table_->setColumnCount(7);
    downloading_table_->setHorizontalHeaderLabels({
        tr("File Name"),
        tr("Size"),
        tr("Progress"),
        tr("Speed"),
        tr("Status"),
        tr("Save Path"),
        tr("Actions")
    });

    downloading_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    downloading_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    downloading_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    downloading_table_->setShowGrid(false);
    downloading_table_->setAlternatingRowColors(false);
    downloading_table_->verticalHeader()->setVisible(false);
    downloading_table_->horizontalHeader()->setStretchLastSection(true);
    downloading_table_->horizontalHeader()->setHighlightSections(false);

    downloading_table_->setColumnWidth(0, 300);
    downloading_table_->setColumnWidth(1, 100);
    downloading_table_->setColumnWidth(2, 200);
    downloading_table_->setColumnWidth(3, 100);
    downloading_table_->setColumnWidth(4, 100);
    downloading_table_->setColumnWidth(5, 250);

    connect(downloading_table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { update_action_buttons(); });

    tab_widget_->addTab(downloading_table_, tr("Downloading"));
}

void DownloadPage::create_completed_tab()
{
    completed_table_ = new QTableWidget(this);
    completed_table_->setColumnCount(6);
    completed_table_->setHorizontalHeaderLabels({
        tr("File Name"),
        tr("Size"),
        tr("Completed At"),
        tr("Save Path"),
        tr("Type"),
        tr("Actions")
    });

    completed_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    completed_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    completed_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    completed_table_->setShowGrid(false);
    completed_table_->verticalHeader()->setVisible(false);
    completed_table_->horizontalHeader()->setStretchLastSection(true);
    connect(completed_table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { update_action_buttons(); });

    tab_widget_->addTab(completed_table_, tr("Completed"));
}

void DownloadPage::create_trash_tab()
{
    trash_table_ = new QTableWidget(this);
    trash_table_->setColumnCount(5);
    trash_table_->setHorizontalHeaderLabels({
        tr("File Name"),
        tr("Size"),
        tr("Deleted At"),
        tr("Reason"),
        tr("Actions")
    });

    trash_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    trash_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    trash_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    trash_table_->setShowGrid(false);
    trash_table_->verticalHeader()->setVisible(false);
    trash_table_->horizontalHeader()->setStretchLastSection(true);
    connect(trash_table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { update_action_buttons(); });

    tab_widget_->addTab(trash_table_, tr("Trash"));
}

void DownloadPage::create_history_tab()
{
    history_table_ = new QTableWidget(this);
    history_table_->setColumnCount(6);
    history_table_->setHorizontalHeaderLabels({
        tr("File Name"),
        tr("Size"),
        tr("Started At"),
        tr("Finished At"),
        tr("Status"),
        tr("Actions")
    });

    history_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    history_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    history_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    history_table_->setShowGrid(false);
    history_table_->verticalHeader()->setVisible(false);
    history_table_->horizontalHeader()->setStretchLastSection(true);
    connect(history_table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { update_action_buttons(); });

    tab_widget_->addTab(history_table_, tr("History"));
}

void DownloadPage::pause_selected_task()
{
    const auto task = selected_task();
    if (!task) {
        return;
    }

    (void)task->pause();
    update_action_buttons();
}

void DownloadPage::resume_selected_task()
{
    const auto task = selected_task();
    if (!task) {
        return;
    }

    (void)task->resume();
    update_action_buttons();
}

void DownloadPage::cancel_selected_task()
{
    const auto task = selected_task();
    if (!task) {
        return;
    }

    (void)task->cancel();
    update_action_buttons();
}

void DownloadPage::delete_selected_task()
{
    if (!tab_widget_) {
        return;
    }

    QTableWidget* current_table = qobject_cast<QTableWidget*>(tab_widget_->currentWidget());
    if (!current_table) {
        return;
    }

    const falcon::TaskId id = selected_archived_task_id(current_table);
    if (id == falcon::INVALID_TASK_ID) {
        return;
    }

    if (current_table == trash_table_) {
        const qulonglong key = static_cast<qulonglong>(id);
        auto row_it = trash_row_by_task_id_.find(key);
        if (row_it == trash_row_by_task_id_.end()) {
            return;
        }

        const int removed_row = row_it.value();
        trash_table_->removeRow(removed_row);
        trash_row_by_task_id_.erase(row_it);
        for (auto it = trash_row_by_task_id_.begin(); it != trash_row_by_task_id_.end(); ++it) {
            if (it.value() > removed_row) {
                it.value() -= 1;
            }
        }
    } else {
        append_trash_entry(id, tr("Removed from downloads"));
        emit remove_task_requested(id);
        remove_completed_row(id);
        remove_downloading_row(id);
        remove_tracked_task(id);
    }

    update_action_buttons();
}

void DownloadPage::clear_finished_tasks()
{
    QList<falcon::TaskId> finished_ids;
    for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
        const auto& record = it.value();
        if (!record.task) {
            continue;
        }

        const auto status = record.task->status();
        if (status == falcon::TaskStatus::Completed ||
            status == falcon::TaskStatus::Failed ||
            status == falcon::TaskStatus::Cancelled) {
            finished_ids.append(record.task->id());
        }
    }

    for (falcon::TaskId id : finished_ids) {
        append_trash_entry(id, tr("Cleared finished task"));
        remove_completed_row(id);
        remove_downloading_row(id);
        remove_tracked_task(id);
    }

    if (!finished_ids.isEmpty()) {
        emit remove_finished_tasks_requested();
    }

    update_action_buttons();
}

void DownloadPage::update_action_buttons()
{
    pause_button_->setEnabled(false);
    resume_button_->setEnabled(false);
    cancel_button_->setEnabled(false);
    delete_button_->setEnabled(false);

    bool has_finished = false;
    for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
        const auto& record = it.value();
        if (!record.task) {
            continue;
        }

        const auto status = record.task->status();
        if (status == falcon::TaskStatus::Completed ||
            status == falcon::TaskStatus::Failed ||
            status == falcon::TaskStatus::Cancelled) {
            has_finished = true;
            break;
        }
    }
    clean_button_->setEnabled(has_finished);

    if (!tab_widget_) {
        return;
    }

    QTableWidget* current_table = qobject_cast<QTableWidget*>(tab_widget_->currentWidget());
    if (current_table == downloading_table_) {
        const auto task = selected_task();
        if (!task) {
            return;
        }

        switch (task->status()) {
            case falcon::TaskStatus::Downloading:
            case falcon::TaskStatus::Preparing:
                pause_button_->setEnabled(true);
                cancel_button_->setEnabled(true);
                break;
            case falcon::TaskStatus::Paused:
            case falcon::TaskStatus::Pending:
                resume_button_->setEnabled(true);
                cancel_button_->setEnabled(true);
                break;
            case falcon::TaskStatus::Completed:
            case falcon::TaskStatus::Failed:
            case falcon::TaskStatus::Cancelled:
                delete_button_->setEnabled(true);
                break;
        }
        return;
    }

    if (current_table == completed_table_ || current_table == history_table_ || current_table == trash_table_) {
        if (selected_archived_task_id(current_table) != falcon::INVALID_TASK_ID) {
            delete_button_->setEnabled(true);
        }
    }
}

falcon::DownloadTask::Ptr DownloadPage::selected_task() const
{
    if (!downloading_table_ || !downloading_table_->selectionModel()) {
        return nullptr;
    }

    const auto selected_rows = downloading_table_->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        return nullptr;
    }

    auto* item = downloading_table_->item(selected_rows.first().row(), 0);
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

falcon::TaskId DownloadPage::selected_archived_task_id(QTableWidget* table) const
{
    if (!table || !table->selectionModel()) {
        return falcon::INVALID_TASK_ID;
    }

    const auto selected_rows = table->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        return falcon::INVALID_TASK_ID;
    }

    auto* item = table->item(selected_rows.first().row(), 0);
    if (!item) {
        return falcon::INVALID_TASK_ID;
    }

    return static_cast<falcon::TaskId>(item->data(Qt::UserRole).toULongLong());
}

void DownloadPage::remove_downloading_row(falcon::TaskId id)
{
    const qulonglong key = static_cast<qulonglong>(id);
    auto row_it = row_by_task_id_.find(key);
    if (row_it == row_by_task_id_.end()) {
        return;
    }

    const int removed_row = row_it.value();
    downloading_table_->removeRow(removed_row);
    row_by_task_id_.erase(row_it);
    for (auto it = row_by_task_id_.begin(); it != row_by_task_id_.end(); ++it) {
        if (it.value() > removed_row) {
            it.value() -= 1;
        }
    }
}

void DownloadPage::append_trash_entry(falcon::TaskId id, const QString& reason)
{
    const qulonglong key = static_cast<qulonglong>(id);
    auto record_it = task_records_.find(key);
    if (record_it == task_records_.end()) {
        return;
    }

    TaskRecord& record = record_it.value();
    if (record.in_trash) {
        return;
    }

    const int row = trash_table_->rowCount();
    trash_table_->insertRow(row);
    trash_row_by_task_id_.insert(key, row);
    record.in_trash = true;

    auto set_item = [&](int col, const QString& text) {
        auto* item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(key));
        trash_table_->setItem(row, col, item);
    };

    set_item(0, record.filename);
    set_item(1, record.size_text);
    set_item(2, format_timestamp(QDateTime::currentDateTime()));
    set_item(3, reason);
    set_item(4, "...");
}

void DownloadPage::upsert_completed_row(falcon::TaskId id)
{
    const qulonglong key = static_cast<qulonglong>(id);
    auto record_it = task_records_.find(key);
    if (record_it == task_records_.end()) {
        return;
    }

    const TaskRecord& record = record_it.value();
    const int row = completed_row_by_task_id_.contains(key)
        ? completed_row_by_task_id_.value(key)
        : completed_table_->rowCount();

    if (!completed_row_by_task_id_.contains(key)) {
        completed_table_->insertRow(row);
        completed_row_by_task_id_.insert(key, row);
    }

    auto set_item = [&](int col, const QString& text) {
        auto* item = completed_table_->item(row, col);
        if (!item) {
            item = new QTableWidgetItem();
            item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(key));
            completed_table_->setItem(row, col, item);
        }
        item->setText(text);
    };

    set_item(0, record.filename);
    set_item(1, record.size_text);
    set_item(2, format_timestamp(record.finished_at));
    set_item(3, record.save_path);
    set_item(4, tr("Download"));
    set_item(5, "...");
}

void DownloadPage::upsert_history_row(falcon::TaskId id)
{
    const qulonglong key = static_cast<qulonglong>(id);
    auto record_it = task_records_.find(key);
    if (record_it == task_records_.end()) {
        return;
    }

    const TaskRecord& record = record_it.value();
    const int row = history_row_by_task_id_.contains(key)
        ? history_row_by_task_id_.value(key)
        : history_table_->rowCount();

    if (!history_row_by_task_id_.contains(key)) {
        history_table_->insertRow(row);
        history_row_by_task_id_.insert(key, row);
    }

    auto set_item = [&](int col, const QString& text) {
        auto* item = history_table_->item(row, col);
        if (!item) {
            item = new QTableWidgetItem();
            item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(key));
            history_table_->setItem(row, col, item);
        }
        item->setText(text);
    };

    const QString status = record.error_text.isEmpty()
        ? record.status_text
        : tr("%1 (%2)").arg(record.status_text, record.error_text);
    set_item(0, record.filename);
    set_item(1, record.size_text);
    set_item(2, format_timestamp(record.added_at));
    set_item(3, format_timestamp(record.finished_at));
    set_item(4, status);
    set_item(5, "...");
}

void DownloadPage::remove_completed_row(falcon::TaskId id)
{
    const qulonglong key = static_cast<qulonglong>(id);
    auto row_it = completed_row_by_task_id_.find(key);
    if (row_it == completed_row_by_task_id_.end()) {
        return;
    }

    const int removed_row = row_it.value();
    completed_table_->removeRow(removed_row);
    completed_row_by_task_id_.erase(row_it);
    for (auto it = completed_row_by_task_id_.begin(); it != completed_row_by_task_id_.end(); ++it) {
        if (it.value() > removed_row) {
            it.value() -= 1;
        }
    }
}

void DownloadPage::remove_tracked_task(falcon::TaskId id)
{
    const qulonglong key = static_cast<qulonglong>(id);
    auto record_it = task_records_.find(key);
    if (record_it == task_records_.end()) {
        return;
    }

    record_it->task.reset();
    row_by_task_id_.remove(key);
    completed_row_by_task_id_.remove(key);
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

    TaskRecord& record = record_it.value();
    const auto status = task->status();
    if (status == falcon::TaskStatus::Completed ||
        status == falcon::TaskStatus::Failed ||
        status == falcon::TaskStatus::Cancelled) {
        if (!record.finished_at.isValid()) {
            record.finished_at = QDateTime::currentDateTime();
        }
        remove_downloading_row(task->id());
        if (status == falcon::TaskStatus::Completed) {
            upsert_completed_row(task->id());
        } else {
            remove_completed_row(task->id());
        }
        upsert_history_row(task->id());
        return;
    }

    if (row_by_task_id_.contains(key)) {
        return;
    }

    const int row = downloading_table_->rowCount();
    downloading_table_->insertRow(row);
    row_by_task_id_.insert(key, row);

    auto set_item = [&](int col, const QString& text) {
        auto* item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(key));
        downloading_table_->setItem(row, col, item);
    };

    set_item(0, record.filename);
    set_item(1, record.size_text);
    set_item(3, "0 B/s");
    set_item(4, record.status_text);
    set_item(5, record.save_path);
    set_item(6, "...");

    auto* progress_bar = new QProgressBar(this);
    progress_bar->setRange(0, 100);
    progress_bar->setValue(0);
    progress_bar->setTextVisible(false);
    downloading_table_->setCellWidget(row, 2, progress_bar);
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

QString DownloadPage::format_timestamp(const QDateTime& timestamp)
{
    return timestamp.isValid() ? timestamp.toString("yyyy-MM-dd HH:mm:ss") : "-";
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
    record.added_at = QDateTime::currentDateTime();
    task_records_.insert(key, record);

    sync_task_tables(task);
    update_action_buttons();
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

        if (record.save_path.isEmpty()) {
            record.save_path = QString::fromStdString(task->options().output_directory);
        }
        if (record.filename == tr("(unknown)")) {
            const QString resolved_filename = !task->options().output_filename.empty()
                ? QString::fromStdString(task->options().output_filename)
                : QString::fromStdString(task->file_info().filename);
            if (!resolved_filename.isEmpty()) {
                record.filename = resolved_filename;
            }
        }

        sync_task_tables(task);

        auto row_it = row_by_task_id_.find(it.key());
        if (row_it == row_by_task_id_.end()) {
            continue;
        }

        const int row = row_it.value();
        if (auto* name_item = downloading_table_->item(row, 0)) {
            name_item->setText(record.filename);
        }
        if (auto* size_item = downloading_table_->item(row, 1)) {
            size_item->setText(record.size_text);
        }
        if (auto* speed_item = downloading_table_->item(row, 3)) {
            speed_item->setText(format_speed(speed));
        }
        if (auto* status_item = downloading_table_->item(row, 4)) {
            status_item->setText(record.status_text);
        }
        if (auto* path_item = downloading_table_->item(row, 5)) {
            path_item->setText(record.save_path);
        }
        if (auto* widget = downloading_table_->cellWidget(row, 2)) {
            if (auto* bar = qobject_cast<QProgressBar*>(widget)) {
                bar->setValue(std::max(0, std::min(100, pct)));
            }
        }
    }

    update_action_buttons();
}

} // namespace falcon::desktop
