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
#include <QClipboard>
#include <QScrollArea>
#include <QFrame>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace falcon::desktop {

namespace {
constexpr int kRowHeight = 56;
constexpr int kSummaryCardWidth = 168;

// 云盘域名列表（用于识别云添加任务）
const QStringList& cloud_domains()
{
    static const QStringList domains = {
        "pan.baidu.com",
        "pan.quark.cn",
        "cloud.189.cn",
        "www.alipan.com",
        "www.aliyundrive.com",
        "www.115.com",
        "disk.pikpak.com"
    };
    return domains;
}
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
    , grid_container_(nullptr)
    , grid_scroll_area_(nullptr)
    , grid_widget_(nullptr)
    , grid_layout_(nullptr)
{
    setup_ui();
}

DownloadPage::~DownloadPage() = default;

void DownloadPage::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(18, 18, 18, 18);
    main_layout->setSpacing(14);

    create_hero_section();

    create_summary_cards();
    main_layout->addSpacing(4);

    create_header_bar();
    main_layout->addLayout(header_layout_);

    create_empty_state();
    main_layout->addWidget(empty_state_widget_);

    // 创建表格视图
    create_task_table();
    main_layout->addWidget(task_table_);

    // 创建网格视图（初始隐藏）
    create_task_grid();
    main_layout->addWidget(grid_container_);
    grid_container_->hide();

    main_layout->addStretch(1);
    update_empty_state();
}

void DownloadPage::create_hero_section()
{
    auto* hero_container = new QWidget(this);
    hero_container->setObjectName("downloadHero");

    auto* hero_layout = new QHBoxLayout(hero_container);
    hero_layout->setContentsMargins(20, 18, 20, 18);
    hero_layout->setSpacing(16);

    auto* text_layout = new QVBoxLayout();
    text_layout->setSpacing(4);

    auto* eyebrow = new QLabel(tr("DOWNLOAD CENTER"), hero_container);
    eyebrow->setObjectName("heroEyebrow");
    text_layout->addWidget(eyebrow);

    hero_title_label_ = new QLabel(tr("下载中"), hero_container);
    hero_title_label_->setObjectName("heroTitle");
    text_layout->addWidget(hero_title_label_);

    hero_description_label_ = new QLabel(tr("集中管理任务、速度与完成状态。"), hero_container);
    hero_description_label_->setObjectName("heroDescription");
    text_layout->addWidget(hero_description_label_);

    hero_layout->addLayout(text_layout, 1);

    new_task_button_ = new QPushButton(tr("新建下载"), hero_container);
    new_task_button_->setObjectName("primaryButton");
    new_task_button_->setMinimumHeight(40);
    connect(new_task_button_, &QPushButton::clicked, this, &DownloadPage::on_new_task_clicked);
    hero_layout->addWidget(new_task_button_);

    auto* root_layout = qobject_cast<QVBoxLayout*>(layout());
    if (root_layout) {
        root_layout->addWidget(hero_container);
    }
}

void DownloadPage::create_summary_cards()
{
    auto* summary_layout = new QHBoxLayout();
    summary_layout->setSpacing(12);

    auto create_card = [this, summary_layout](const QString& caption, QLabel** value_label) {
        auto* card = new QWidget(this);
        card->setObjectName("summaryCard");
        card->setFixedWidth(kSummaryCardWidth);

        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(16, 14, 16, 14);
        card_layout->setSpacing(2);

        auto* value = new QLabel("0", card);
        value->setObjectName("summaryValue");
        card_layout->addWidget(value);

        auto* label = new QLabel(caption, card);
        label->setObjectName("summaryCaption");
        card_layout->addWidget(label);

        *value_label = value;
        summary_layout->addWidget(card);
    };

    create_card(tr("活跃任务"), &active_summary_value_);
    create_card(tr("已完成"), &completed_summary_value_);
    create_card(tr("当前速度"), &speed_summary_value_);
    summary_layout->addStretch();

    auto* root_layout = qobject_cast<QVBoxLayout*>(layout());
    if (root_layout) {
        root_layout->addLayout(summary_layout);
    }
}

void DownloadPage::create_empty_state()
{
    empty_state_widget_ = new QWidget(this);
    empty_state_widget_->setObjectName("summaryCard");

    auto* layout = new QVBoxLayout(empty_state_widget_);
    layout->setContentsMargins(20, 28, 20, 28);
    layout->setSpacing(8);
    layout->setAlignment(Qt::AlignCenter);

    empty_state_title_ = new QLabel(tr("还没有任务"), empty_state_widget_);
    empty_state_title_->setObjectName("emptyStateTitle");
    empty_state_title_->setAlignment(Qt::AlignCenter);
    layout->addWidget(empty_state_title_);

    empty_state_body_ = new QLabel(tr("点击“新建下载”，或在顶部直接粘贴链接开始。"), empty_state_widget_);
    empty_state_body_->setObjectName("emptyStateBody");
    empty_state_body_->setAlignment(Qt::AlignCenter);
    empty_state_body_->setWordWrap(true);
    layout->addWidget(empty_state_body_);
}

void DownloadPage::create_header_bar()
{
    header_layout_ = new QHBoxLayout();
    header_layout_->setSpacing(10);
    header_layout_->setContentsMargins(2, 0, 2, 0);
    header_layout_->setObjectName("downloadToolbar");

    status_label_ = new QLabel(tr("已暂停"), this);
    status_label_->setObjectName("headerLabel");
    header_layout_->addWidget(status_label_);

    header_layout_->addStretch();

    refresh_button_ = new QPushButton(tr("刷新列表"), this);
    refresh_button_->setObjectName("toolButton");
    refresh_button_->setFixedHeight(34);
    connect(refresh_button_, &QPushButton::clicked, this, &DownloadPage::on_refresh_clicked);
    header_layout_->addWidget(refresh_button_);

    view_toggle_button_ = new QPushButton(tr("切换分组"), this);
    view_toggle_button_->setObjectName("toolButton");
    view_toggle_button_->setFixedHeight(34);
    connect(view_toggle_button_, &QPushButton::clicked, this, &DownloadPage::on_view_toggle_clicked);
    header_layout_->addWidget(view_toggle_button_);

    style_toggle_button_ = new QPushButton(tr("卡片视图"), this);
    style_toggle_button_->setObjectName("toolButton");
    style_toggle_button_->setFixedHeight(34);
    connect(style_toggle_button_, &QPushButton::clicked, this, &DownloadPage::on_style_toggle_clicked);
    header_layout_->addWidget(style_toggle_button_);

    more_button_ = new QPushButton(tr("批量操作"), this);
    more_button_->setObjectName("toolButton");
    more_button_->setFixedHeight(34);
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

    // 清空并按新过滤条件重新加载任务
    task_table_->setRowCount(0);
    row_by_task_id_.clear();
    rerender();
}

void DownloadPage::update_header_for_mode()
{
    switch (view_mode_) {
        case DownloadViewMode::Downloading:
            status_label_->setText(tr("下载中"));
            hero_title_label_->setText(tr("下载中"));
            hero_description_label_->setText(tr("优先关注活跃任务、速度与剩余进度。"));
            break;
        case DownloadViewMode::Completed:
            status_label_->setText(tr("已完成"));
            hero_title_label_->setText(tr("已完成"));
            hero_description_label_->setText(tr("快速回看已完成内容，清理或打开文件目录。"));
            break;
        case DownloadViewMode::CloudAdd:
            status_label_->setText(tr("云添加"));
            hero_title_label_->setText(tr("云添加"));
            hero_description_label_->setText(tr("整理来自网盘与转存链路的下载任务。"));
            break;
    }
}

QString DownloadPage::filename_for(const falcon::daemon::rpc::TaskSnapshot& snapshot)
{
    if (!snapshot.output_path.empty()) {
        const QFileInfo info(QString::fromStdString(snapshot.output_path));
        const QString name = info.fileName();
        if (!name.isEmpty()) {
            return name;
        }
    }
    // 回退：从 URL 取最后一段（去掉查询串）
    const QString url = QString::fromStdString(snapshot.url);
    const int slash = url.lastIndexOf('/');
    QString name = (slash >= 0 && slash < url.length() - 1) ? url.mid(slash + 1) : url;
    const int query = name.indexOf('?');
    if (query >= 0) {
        name = name.left(query);
    }
    return name;
}

void DownloadPage::update_tasks(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks)
{
    // 重建记录表：不再存在的任务随之消失
    QHash<qulonglong, TaskRecord> fresh;
    fresh.reserve(static_cast<int>(tasks.size()) * 2);
    for (const auto& snap : tasks) {
        TaskRecord record;
        record.snapshot = snap;
        record.filename = filename_for(snap);
        if (record.filename.isEmpty()) {
            record.filename = tr("(unknown)");
        }
        record.save_path = QString::fromStdString(snap.output_path);
        record.size_text = snap.total_bytes > 0 ? format_bytes(snap.total_bytes) : "-";
        record.status_text = QString::fromUtf8(falcon::to_string(snap.status));
        record.error_text = QString::fromStdString(snap.error_message);
        fresh.insert(static_cast<qulonglong>(snap.id), record);
    }
    task_records_ = std::move(fresh);

    // 删除已消失任务的行
    QSet<qulonglong> live_keys;
    for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
        live_keys.insert(it.key());
    }
    for (auto it = row_by_task_id_.begin(); it != row_by_task_id_.end();) {
        if (!live_keys.contains(it.key())) {
            const int row = it.value();
            task_table_->removeRow(row);
            it = row_by_task_id_.erase(it);
            for (auto row_it = row_by_task_id_.begin(); row_it != row_by_task_id_.end(); ++row_it) {
                if (row_it.value() > row) {
                    row_it.value() -= 1;
                }
            }
        } else {
            ++it;
        }
    }

    rerender();
}

void DownloadPage::rerender()
{
    for (auto it = task_records_.begin(); it != task_records_.end(); ++it) {
        TaskRecord& record = it.value();
        const auto& snap = record.snapshot;
        record.size_text = snap.total_bytes > 0 ? format_bytes(snap.total_bytes) : "-";
        record.status_text = QString::fromUtf8(falcon::to_string(snap.status));
        record.error_text = QString::fromStdString(snap.error_message);
        sync_task_row(record);
    }

    // 刷新可见行的动态列（大小/速度/状态/进度）
    for (auto it = row_by_task_id_.cbegin(); it != row_by_task_id_.cend(); ++it) {
        const auto* record = record_by_id(it.key());
        if (record) {
            update_row_texts(it.value(), *record);
        }
    }

    update_action_buttons();
    update_summary_cards();
    update_empty_state();

    // 如果是网格视图，刷新网格显示
    if (display_style_ == TaskDisplayStyle::Grid) {
        sync_task_grid();
    }
}

bool DownloadPage::should_show(const falcon::daemon::rpc::TaskSnapshot& snapshot) const
{
    switch (view_mode_) {
        case DownloadViewMode::Downloading:
            return snapshot.status == falcon::TaskStatus::Downloading ||
                   snapshot.status == falcon::TaskStatus::Preparing ||
                   snapshot.status == falcon::TaskStatus::Paused ||
                   snapshot.status == falcon::TaskStatus::Pending;
        case DownloadViewMode::Completed:
            return snapshot.status == falcon::TaskStatus::Completed;
        case DownloadViewMode::CloudAdd: {
            const QString url = QString::fromStdString(snapshot.url);
            const auto& domains = cloud_domains();
            return std::any_of(domains.begin(), domains.end(),
                [&url](const QString& domain) { return url.contains(domain); });
        }
    }
    return false;
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

    const int row = task_table_->rowCount();
    task_table_->insertRow(row);
    task_table_->setRowHeight(row, kRowHeight);
    row_by_task_id_.insert(key, row);

    // 文件名（带图标）
    auto* name_item = new QTableWidgetItem(record.filename);
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

    auto* pause_btn = new QPushButton(tr("暂停"), actions_widget);
    pause_btn->setObjectName("rowActionButton");
    pause_btn->setFixedHeight(28);
    pause_btn->setToolTip(tr("暂停"));
    pause_btn->setProperty("taskId", QVariant::fromValue<qulonglong>(key));
    connect(pause_btn, &QPushButton::clicked, this, &DownloadPage::on_pause_selected);
    actions_layout->addWidget(pause_btn);

    auto* delete_btn = new QPushButton(tr("删除"), actions_widget);
    delete_btn->setObjectName("rowActionButton");
    delete_btn->setFixedHeight(28);
    delete_btn->setToolTip(tr("删除"));
    delete_btn->setProperty("taskId", QVariant::fromValue<qulonglong>(key));
    connect(delete_btn, &QPushButton::clicked, this, &DownloadPage::on_delete_selected);
    actions_layout->addWidget(delete_btn);

    task_table_->setCellWidget(row, 5, actions_widget);
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
        speed_item->setText(format_speed(snap.speed));
    }
    if (auto* status_item = task_table_->item(row, 4)) {
        status_item->setText(record.status_text);
    }
    if (auto* widget = task_table_->cellWidget(row, 1)) {
        if (auto* bar = qobject_cast<QProgressBar*>(widget)) {
            bar->setValue(std::max(0, std::min(100, pct)));
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

void DownloadPage::update_summary_cards()
{
    int active_count = 0;
    int completed_count = 0;
    uint64_t total_speed = 0;

    for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
        const auto& snap = it.value().snapshot;
        if (snap.status == falcon::TaskStatus::Downloading ||
            snap.status == falcon::TaskStatus::Preparing) {
            ++active_count;
        }
        if (snap.status == falcon::TaskStatus::Completed) {
            ++completed_count;
        }
        total_speed += snap.speed;
    }

    if (active_summary_value_) {
        active_summary_value_->setText(QString::number(active_count));
    }
    if (completed_summary_value_) {
        completed_summary_value_->setText(QString::number(completed_count));
    }
    if (speed_summary_value_) {
        speed_summary_value_->setText(format_speed(total_speed));
    }
}

void DownloadPage::update_empty_state()
{
    const bool has_rows = task_table_ && task_table_->rowCount() > 0;
    if (empty_state_widget_) {
        empty_state_widget_->setVisible(!has_rows);
    }
    if (task_table_) {
        task_table_->setVisible(has_rows && display_style_ == TaskDisplayStyle::Table);
    }
    if (grid_container_) {
        grid_container_->setVisible(has_rows && display_style_ == TaskDisplayStyle::Grid);
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

        const auto* record = record_by_id(name_item->data(Qt::UserRole).toULongLong());
        if (!record) {
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
        if (!pause_btn) {
            continue;
        }

        const auto status = record->snapshot.status;
        const bool is_running = (status == falcon::TaskStatus::Downloading ||
                                 status == falcon::TaskStatus::Preparing);
        const bool is_paused = (status == falcon::TaskStatus::Paused);
        const bool can_resume = (status == falcon::TaskStatus::Paused ||
                                 status == falcon::TaskStatus::Failed);

        if (is_running) {
            pause_btn->setEnabled(true);
            pause_btn->setText(tr("暂停"));
            pause_btn->setToolTip(tr("暂停"));
        } else if (is_paused || can_resume) {
            pause_btn->setEnabled(true);
            pause_btn->setText(tr("继续"));
            pause_btn->setToolTip(tr("继续"));
        } else {
            pause_btn->setEnabled(false);
        }
    }
}

void DownloadPage::on_new_task_clicked()
{
    emit new_task_requested();
}

void DownloadPage::on_refresh_clicked()
{
    // 数据由 DownloadService 周期推送；此处重渲染当前快照
    rerender();
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

    menu.addSeparator();

    auto* remove_finished_action = menu.addAction(tr("清除已完成"));
    connect(remove_finished_action, &QAction::triggered, this, [this]() {
        emit remove_finished_tasks_requested();
    });

    menu.addSeparator();

    auto* open_dir_action = menu.addAction(tr("打开保存目录"));
    connect(open_dir_action, &QAction::triggered, this, [this]() {
        if (const auto* record = selected_record()) {
            const QString path = record->save_path.isEmpty()
                ? QString::fromStdString(record->snapshot.url)
                : record->save_path;
            const QString dir = QFileInfo(path).absolutePath();
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    });

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

    menu.addSeparator();

    // 打开文件夹与复制链接按值捕获快照内容：菜单 exec 期间任务表可能被
    // update_tasks 整体重建，不能捕获指向 task_records_ 的指针
    const QString dir_for_open = QFileInfo(
        record->save_path.isEmpty() ? QString::fromStdString(record->snapshot.url)
                                    : record->save_path).absolutePath();
    auto* open_dir_action = menu.addAction(tr("打开文件夹"));
    connect(open_dir_action, &QAction::triggered, this, [dir_for_open]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir_for_open));
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
}

void DownloadPage::on_style_toggle_clicked()
{
    // 切换显示样式
    switch (display_style_) {
        case TaskDisplayStyle::Table:
            set_display_style(TaskDisplayStyle::Grid);
            style_toggle_button_->setText(tr("列表视图"));
            break;
        case TaskDisplayStyle::Grid:
            set_display_style(TaskDisplayStyle::Table);
            style_toggle_button_->setText(tr("卡片视图"));
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

    menu.addSeparator();

    // 按值捕获快照内容，理由同 show_context_menu
    const QString dir_for_open = QFileInfo(
        record->save_path.isEmpty() ? QString::fromStdString(record->snapshot.url)
                                    : record->save_path).absolutePath();
    auto* open_dir_action = menu.addAction(tr("打开文件夹"));
    connect(open_dir_action, &QAction::triggered, this, [dir_for_open]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir_for_open));
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

    for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
        const TaskRecord& record = it.value();
        if (!should_show(record.snapshot)) {
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
    const auto& snap = record.snapshot;

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
    progress_bar->setValue(std::max(0, std::min(100, static_cast<int>(snap.progress * 100.0))));
    card_layout->addWidget(progress_bar);

    // 信息行
    auto* info_layout = new QHBoxLayout();
    info_layout->setSpacing(12);

    auto* size_label = new QLabel(record.size_text, card);
    size_label->setObjectName("cardInfoLabel");
    info_layout->addWidget(size_label);

    auto* speed_label = new QLabel(format_speed(snap.speed), card);
    speed_label->setObjectName("cardInfoLabel");
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
    if (snap.status == falcon::TaskStatus::Downloading ||
        snap.status == falcon::TaskStatus::Preparing) {
        pause_btn->setText(tr("暂停"));
        connect(pause_btn, &QPushButton::clicked, this,
                [this, id = snap.id]() { emit pause_requested(id); });
    } else if (snap.status == falcon::TaskStatus::Paused ||
               snap.status == falcon::TaskStatus::Failed) {
        pause_btn->setText(tr("继续"));
        connect(pause_btn, &QPushButton::clicked, this,
                [this, id = snap.id]() { emit resume_requested(id); });
    } else {
        pause_btn->setEnabled(false);
    }
    actions_layout->addWidget(pause_btn);

    auto* delete_btn = new QPushButton(tr("删除"), card);
    delete_btn->setObjectName("cardActionButton");
    delete_btn->setFixedSize(60, 26);
    connect(delete_btn, &QPushButton::clicked, this, [this, id = snap.id]() {
        emit remove_task_requested(id);
    });
    actions_layout->addWidget(delete_btn);

    actions_layout->addStretch();
    card_layout->addLayout(actions_layout);

    return card;
}

} // namespace falcon::desktop
