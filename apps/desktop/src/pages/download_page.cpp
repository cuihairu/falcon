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
#include <QMenu>
#include <QAction>
#include <QDesktopServices>
#include <QApplication>
#include <algorithm>

namespace falcon::desktop {

namespace {
constexpr int kRowHeight = 56;
} // namespace

DownloadPage::DownloadPage(QWidget* parent)
    : QWidget(parent)
    , view_mode_(DownloadViewMode::Downloading)
    , display_style_(TaskDisplayStyle::Table)
    , header_layout_(nullptr)
    , status_label_(nullptr)
    , new_task_button_(nullptr)
    , refresh_button_(nullptr)
    , view_toggle_button_(nullptr)
    , style_toggle_button_(nullptr)
    , more_button_(nullptr)
    , task_table_(nullptr)
    , pause_button_(nullptr)
    , delete_button_(nullptr)
    , refresh_timer_(nullptr)
    , grid_container_(nullptr)
    , grid_scroll_area_(nullptr)
    , grid_widget_(nullptr)
    , grid_layout_(nullptr)
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

    // 创建表格视图
    create_task_table();
    main_layout->addWidget(task_table_);

    // 创建网格视图（初始隐藏）
    create_task_grid();
    main_layout->addWidget(grid_container_);
    grid_container_->hide();

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

    // 视图切换按钮（过滤模式）
    view_toggle_button_ = new QPushButton(tr("📋"), this);
    view_toggle_button_->setObjectName("toolButton");
    view_toggle_button_->setFixedSize(32, 32);
    view_toggle_button_->setToolTip(tr("切换过滤模式"));
    connect(view_toggle_button_, &QPushButton::clicked, this, &DownloadPage::on_view_toggle_clicked);
    header_layout_->addWidget(view_toggle_button_);

    // 显示样式切换按钮（表格/网格）
    style_toggle_button_ = new QPushButton(tr("▦"), this);
    style_toggle_button_->setObjectName("toolButton");
    style_toggle_button_->setFixedSize(32, 32);
    style_toggle_button_->setToolTip(tr("切换显示样式"));
    connect(style_toggle_button_, &QPushButton::clicked, this, &DownloadPage::on_style_toggle_clicked);
    header_layout_->addWidget(style_toggle_button_);

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

    // 启用右键菜单
    task_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(task_table_, &QTableWidget::customContextMenuRequested,
            this, &DownloadPage::show_context_menu);

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
    // 更新每行操作按钮的状态
    for (int row = 0; row < task_table_->rowCount(); ++row) {
        auto* name_item = task_table_->item(row, 0);
        if (!name_item) {
            continue;
        }

        const qulonglong key = name_item->data(Qt::UserRole).toULongLong();
        auto record_it = task_records_.find(key);
        if (record_it == task_records_.end()) {
            continue;
        }

        const auto& task = record_it->task;
        if (!task) {
            continue;
        }

        // 获取操作按钮容器
        auto* actions_widget = task_table_->cellWidget(row, 5);
        if (!actions_widget) {
            continue;
        }

        auto* layout = qobject_cast<QHBoxLayout*>(actions_widget->layout());
        if (!layout) {
            continue;
        }

        // 第一个按钮是暂停/继续按钮
        auto* pause_btn = qobject_cast<QPushButton*>(layout->itemAt(0)->widget());
        if (pause_btn) {
            const auto status = task->status();
            bool is_running = (status == falcon::TaskStatus::Downloading ||
                              status == falcon::TaskStatus::Preparing);
            bool is_paused = (status == falcon::TaskStatus::Paused);
            bool can_resume = (status == falcon::TaskStatus::Paused ||
                              status == falcon::TaskStatus::Failed);

            pause_btn->setEnabled(is_running || is_paused || can_resume);

            if (is_running) {
                pause_btn->setText(tr("⏸"));
                pause_btn->setToolTip(tr("暂停"));
            } else if (is_paused || can_resume) {
                pause_btn->setText(tr("▶"));
                pause_btn->setToolTip(tr("继续"));
            } else {
                pause_btn->setEnabled(false);
            }
        }
    }
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
    // 循环切换任务过滤模式
    switch (view_mode_) {
        case DownloadViewMode::Downloading:
            set_view_mode(DownloadViewMode::Completed);
            break;
        case DownloadViewMode::Completed:
            set_view_mode(DownloadViewMode::CloudAdd);
            break;
        case DownloadViewMode::CloudAdd:
            set_view_mode(DownloadViewMode::Downloading);
            break;
    }
}

void DownloadPage::on_more_options_clicked()
{
    QMenu menu(this);

    auto* start_all_action = menu.addAction(tr("全部开始"));
    connect(start_all_action, &QAction::triggered, this, [this]() {
        for (auto it = task_records_.begin(); it != task_records_.end(); ++it) {
            const auto& task = it.value().task;
            if (task) {
                (void)task->resume();
            }
        }
    });

    auto* pause_all_action = menu.addAction(tr("全部暂停"));
    connect(pause_all_action, &QAction::triggered, this, [this]() {
        for (auto it = task_records_.begin(); it != task_records_.end(); ++it) {
            const auto& task = it.value().task;
            if (task) {
                (void)task->pause();
            }
        }
    });

    menu.addSeparator();

    auto* remove_finished_action = menu.addAction(tr("清除已完成"));
    connect(remove_finished_action, &QAction::triggered, this, [this]() {
        emit remove_finished_tasks_requested();
    });

    menu.addSeparator();

    auto* open_dir_action = menu.addAction(tr("打开保存目录"));
    connect(open_dir_action, &QAction::triggered, this, [this]() {
        const auto task = selected_task();
        if (task) {
            const QString path = QString::fromStdString(task->options().output_directory);
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    });

    menu.exec(more_button_->mapToGlobal(more_button_->rect().bottomLeft()));
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

    // 如果是网格视图，刷新网格显示
    if (display_style_ == TaskDisplayStyle::Grid) {
        sync_task_grid();
    }
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

    // 云盘域名列表（用于识别云添加任务）
    static const QStringList cloud_domains = {
        "pan.baidu.com",
        "pan.quark.cn",
        "cloud.189.cn",
        "www.alipan.com",
        "www.aliyundrive.com",
        "www.115.com",
        "disk.pikpak.com"
    };

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
            // 显示 URL 包含云盘域名的任务
            {
                const QString url = QString::fromStdString(task->url());
                should_show = std::any_of(cloud_domains.begin(), cloud_domains.end(),
                    [&url](const QString& domain) {
                        return url.contains(domain);
                    });
            }
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

falcon::DownloadTask::Ptr DownloadPage::task_at_row(int row) const
{
    if (row < 0 || row >= task_table_->rowCount()) {
        return nullptr;
    }

    auto* item = task_table_->item(row, 0);
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

void DownloadPage::show_context_menu(const QPoint& pos)
{
    const auto* item = task_table_->itemAt(pos);
    if (!item) {
        return;
    }

    const int row = item->row();
    auto task = task_at_row(row);
    if (!task) {
        return;
    }

    QMenu menu(this);

    // 根据任务状态显示不同菜单项
    const auto status = task->status();

    // 暂停/继续
    if (status == falcon::TaskStatus::Downloading ||
        status == falcon::TaskStatus::Preparing) {
        auto* pause_action = menu.addAction(tr("暂停"));
        connect(pause_action, &QAction::triggered, this, &DownloadPage::on_pause_selected);
    } else if (status == falcon::TaskStatus::Paused ||
               status == falcon::TaskStatus::Failed) {
        auto* resume_action = menu.addAction(tr("继续"));
        connect(resume_action, &QAction::triggered, this, &DownloadPage::on_resume_selected);
    }

    menu.addSeparator();

    // 打开文件夹
    auto* open_dir_action = menu.addAction(tr("打开文件夹"));
    connect(open_dir_action, &QAction::triggered, this, [this]() {
        const auto task = selected_task();
        if (task) {
            const QString path = QString::fromStdString(task->options().output_directory);
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    });

    // 复制下载链接
    auto* copy_url_action = menu.addAction(tr("复制下载链接"));
    connect(copy_url_action, &QAction::triggered, this, [task]() {
        QApplication::clipboard()->setText(QString::fromStdString(task->url()));
    });

    menu.addSeparator();

    // 删除任务
    auto* delete_action = menu.addAction(tr("删除任务"));
    delete_action->setStyleSheet("color: red;");
    connect(delete_action, &QAction::triggered, this, &DownloadPage::on_delete_selected);

    menu.exec(task_table_->mapToGlobal(pos));
}

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

    // 切换视图可见性
    if (style == TaskDisplayStyle::Table) {
        task_table_->show();
        grid_container_->hide();
    } else {
        task_table_->hide();
        grid_container_->show();
        refresh_display();  // 刷新网格内容
    }
}

void DownloadPage::on_style_toggle_clicked()
{
    // 切换显示样式
    switch (display_style_) {
        case TaskDisplayStyle::Table:
            set_display_style(TaskDisplayStyle::Grid);
            break;
        case TaskDisplayStyle::Grid:
            set_display_style(TaskDisplayStyle::Table);
            break;
    }
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

    const qulonglong key = task_id_var.toULongLong();
    auto record_it = task_records_.constFind(key);
    if (record_it == task_records_.constEnd()) {
        return;
    }

    const auto& task = record_it->task;
    if (!task) {
        return;
    }

    QMenu menu(this);

    // 根据任务状态显示不同菜单项
    const auto status = task->status();

    // 暂停/继续
    if (status == falcon::TaskStatus::Downloading ||
        status == falcon::TaskStatus::Preparing) {
        auto* pause_action = menu.addAction(tr("暂停"));
        connect(pause_action, &QAction::triggered, this, [this, task]() {
            (void)task->pause();
        });
    } else if (status == falcon::TaskStatus::Paused ||
               status == falcon::TaskStatus::Failed) {
        auto* resume_action = menu.addAction(tr("继续"));
        connect(resume_action, &QAction::triggered, this, [this, task]() {
            (void)task->resume();
        });
    }

    menu.addSeparator();

    // 打开文件夹
    auto* open_dir_action = menu.addAction(tr("打开文件夹"));
    connect(open_dir_action, &QAction::triggered, this, [task]() {
        const QString path = QString::fromStdString(task->options().output_directory);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    // 复制下载链接
    auto* copy_url_action = menu.addAction(tr("复制下载链接"));
    connect(copy_url_action, &QAction::triggered, this, [task]() {
        QApplication::clipboard()->setText(QString::fromStdString(task->url()));
    });

    menu.addSeparator();

    // 删除任务
    auto* delete_action = menu.addAction(tr("删除任务"));
    delete_action->setStyleSheet("color: red;");
    connect(delete_action, &QAction::triggered, this, [this, task]() {
        emit remove_task_requested(task->id());
    });

    menu.exec(grid_widget_->mapToGlobal(pos));
}

void DownloadPage::refresh_display()
{
    if (display_style_ == TaskDisplayStyle::Grid) {
        sync_task_grid();
    }
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

    for (auto it = task_records_.begin(); it != task_records_.end(); ++it) {
        const TaskRecord& record = it.value();
        const auto& task = record.task;
        if (!task) {
            continue;
        }

        // 根据视图模式过滤
        const auto status = task->status();
        bool should_show = false;

        static const QStringList cloud_domains = {
            "pan.baidu.com", "pan.quark.cn", "cloud.189.cn",
            "www.alipan.com", "www.aliyundrive.com", "www.115.com", "disk.pikpak.com"
        };

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
                {
                    const QString url = QString::fromStdString(task->url());
                    should_show = std::any_of(cloud_domains.begin(), cloud_domains.end(),
                        [&url](const QString& domain) {
                            return url.contains(domain);
                        });
                }
                break;
        }

        if (!should_show) {
            continue;
        }

        // 创建任务卡片
        auto* card = create_task_card(record);
        if (card) {
            card->setProperty("taskId", QVariant::fromValue<qulonglong>(it.key()));
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
    auto* card = new QWidget(grid_widget_);
    card->setObjectName("taskCard");
    card->setFixedSize(280, 140);

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(12, 12, 12, 12);
    card_layout->setSpacing(8);

    // 文件名
    auto* name_label = new QLabel(record.filename, card);
    name_label->setObjectName("cardFileName");
    name_label->setWordWrap(true);
    name_label->setMaximumHeight(40);
    auto name_font = name_label->font();
    name_font.setBold(true);
    name_font.setPointSize(10);
    name_label->setFont(name_font);
    card_layout->addWidget(name_label);

    // 进度条
    auto* progress_bar = new QProgressBar(card);
    progress_bar->setObjectName("cardProgressBar");
    progress_bar->setRange(0, 100);
    progress_bar->setTextVisible(true);
    progress_bar->setMaximumHeight(20);
    if (record.task) {
        const int pct = static_cast<int>(record.task->progress() * 100.0f);
        progress_bar->setValue(std::max(0, std::min(100, pct)));
    }
    card_layout->addWidget(progress_bar);

    // 信息行
    auto* info_layout = new QHBoxLayout();
    info_layout->setSpacing(12);

    auto* size_label = new QLabel(record.size_text, card);
    size_label->setObjectName("cardInfoLabel");
    info_layout->addWidget(size_label);

    auto* speed_label = new QLabel(card);
    speed_label->setObjectName("cardInfoLabel");
    if (record.task) {
        const uint64_t speed = static_cast<uint64_t>(record.task->speed());
        speed_label->setText(format_speed(speed));
    }
    info_layout->addWidget(speed_label);

    auto* status_label = new QLabel(record.status_text, card);
    status_label->setObjectName("cardInfoLabel");
    info_layout->addWidget(status_label);

    card_layout->addLayout(info_layout);

    // 操作按钮
    auto* actions_layout = new QHBoxLayout();
    actions_layout->setSpacing(8);

    auto* pause_btn = new QPushButton(card);
    pause_btn->setObjectName("cardActionButton");
    pause_btn->setFixedSize(60, 26);
    const auto status = record.task ? record.task->status() : falcon::TaskStatus::Pending;
    if (status == falcon::TaskStatus::Downloading ||
        status == falcon::TaskStatus::Preparing) {
        pause_btn->setText(tr("暂停"));
        connect(pause_btn, &QPushButton::clicked, this, [this, record]() {
            if (record.task) (void)record.task->pause();
        });
    } else if (status == falcon::TaskStatus::Paused ||
               status == falcon::TaskStatus::Failed) {
        pause_btn->setText(tr("继续"));
        connect(pause_btn, &QPushButton::clicked, this, [this, record]() {
            if (record.task) (void)record.task->resume();
        });
    } else {
        pause_btn->setEnabled(false);
    }
    actions_layout->addWidget(pause_btn);

    auto* delete_btn = new QPushButton(tr("删除"), card);
    delete_btn->setObjectName("cardActionButton");
    delete_btn->setFixedSize(60, 26);
    connect(delete_btn, &QPushButton::clicked, this, [this, record]() {
        if (record.task) {
            emit remove_task_requested(record.task->id());
        }
    });
    actions_layout->addWidget(delete_btn);

    actions_layout->addStretch();
    card_layout->addLayout(actions_layout);

    return card;
}

} // namespace falcon::desktop
