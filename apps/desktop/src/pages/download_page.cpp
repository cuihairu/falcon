/**
 * @file download_page.cpp
 * @brief Download Page Implementation (Xunlei-style)
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "download_page.hpp"
#include "services/task_order.hpp"
#include "services/task_visibility.hpp"
#include "utils/icon_utils.hpp"
#include "widgets/task_table_widget.hpp"

#include <QButtonGroup>
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
#include <QStringList>
#include <QToolButton>
#include <algorithm>
#include <limits>

namespace falcon::desktop {

namespace {
constexpr int kRowHeight = 56;
constexpr int kSummaryCardWidth = 168;
// 紧凑横条标准（用户 bug 反馈）：hero 与统计卡固定高度，纵向空白不随窗口拉伸
constexpr int kHeroHeight = 78;
constexpr int kSummaryCardHeight = 72;
// 空态卡最小展示高度（用户 bug 反馈：自然高度 ~90px 塌缩挤压，给足 ≥200px）
constexpr int kEmptyStateMinHeight = 232;

// 快照状态 → 界面中文文案（引擎状态名不外露）
QString status_display_text(falcon::TaskStatus status)
{
    switch (status) {
        case falcon::TaskStatus::Pending:    return QObject::tr("等待中");
        case falcon::TaskStatus::Preparing:  return QObject::tr("准备中");
        case falcon::TaskStatus::Downloading: return QObject::tr("下载中");
        case falcon::TaskStatus::Paused:     return QObject::tr("已暂停");
        case falcon::TaskStatus::Completed:  return QObject::tr("已完成");
        case falcon::TaskStatus::Failed:     return QObject::tr("失败");
        case falcon::TaskStatus::Cancelled:  return QObject::tr("已取消");
    }
    return QObject::tr("未知");
}
} // namespace

DownloadPage::DownloadPage(QWidget* parent)
    : QWidget(parent)
    , view_mode_(DownloadViewMode::Downloading)
    , header_layout_(nullptr)
    , new_task_button_(nullptr)
    , refresh_button_(nullptr)
    , style_toggle_button_(nullptr)
    , more_button_(nullptr)
    , task_table_(nullptr)
    // 默认卡片视图（B15：首次/无记录进卡片；手动切换经 QSettings 记忆）
    , display_style_(TaskDisplayStyle::Grid)
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
    create_task_table();
    create_task_grid();

    // 内容区三页栈（空态/表格/网格）：三态恒占同一 stretch=1 区域，
    // 有无任务页面占用体积一致——旧布局空态卡片自然高度直挂、表格
    // stretch=1，任务出现/清空时内容区高度跳变（用户实测「有任务和
    // 没有任务占的体积不一样、窗口跳来跳去」）。空态页卡片垂直居中。
    // 表格/网格页必须由 stack 全高承载：无 stretch 时 QScrollArea
    // 初始 sizeHint 近 0，网格视口被压扁卡片裁剪（2026-09-17 教训）
    content_stack_ = new QStackedWidget(this);
    empty_page_ = new QWidget(content_stack_);
    auto* empty_layout = new QVBoxLayout(empty_page_);
    empty_layout->setContentsMargins(0, 0, 0, 0);
    empty_layout->addStretch(1);
    empty_layout->addWidget(empty_state_widget_);
    empty_layout->addStretch(1);
    content_stack_->addWidget(empty_page_);
    content_stack_->addWidget(task_table_);
    content_stack_->addWidget(grid_container_);
    main_layout->addWidget(content_stack_, 1);

    update_empty_state();
}

void DownloadPage::create_hero_section()
{
    auto* hero_container = new QWidget(this);
    hero_container->setObjectName("downloadHero");
    // 紧凑横条：固定高度，禁止被布局纵向拉伸（用户 bug 反馈）
    hero_container->setFixedHeight(kHeroHeight);

    auto* hero_layout = new QHBoxLayout(hero_container);
    hero_layout->setContentsMargins(20, 12, 20, 12);
    hero_layout->setSpacing(16);

    auto* text_layout = new QVBoxLayout();
    text_layout->setSpacing(4);

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
        // 紧凑数值卡：固定高度，禁止被布局纵向拉伸（用户 bug 反馈）
        card->setFixedHeight(kSummaryCardHeight);

        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(16, 10, 16, 10);
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
    // 空态卡给足展示高度（≥200px）：此前自然高度 ~90px 塌缩、标题正文挤
    // 成一行（用户 bug 反馈）。宽度由 empty_layout 撑满列表区，卡片在本页
    // 内垂直居中悬浮（两 stretch 包夹）
    empty_state_widget_->setMinimumHeight(kEmptyStateMinHeight);

    auto* layout = new QVBoxLayout(empty_state_widget_);
    layout->setContentsMargins(24, 36, 24, 36);
    layout->setSpacing(12);
    // 不在 layout 层设 AlignCenter：会给标签按 sizeHint 紧凑宽度分配，
    // 开了 wordWrap 的正文立即缩成窄列三行折行。标签各自内部居中 +
    // layout 默认横向撑满，正文按卡片全宽折行（正常单行）

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

    // 分段切换器(下载中|已完成):与侧栏 tab 双向同步的唯一页内入口
    view_segmented_ = new QWidget(this);
    view_segmented_->setObjectName("viewSegmented");
    auto* seg_layout = new QHBoxLayout(view_segmented_);
    seg_layout->setContentsMargins(3, 3, 3, 3);
    seg_layout->setSpacing(2);

    auto* seg_group = new QButtonGroup(view_segmented_);
    seg_group->setExclusive(true);

    view_segment_downloading_ = new QPushButton(tr("下载中"), view_segmented_);
    view_segment_downloading_->setObjectName("viewSegmentButton");
    view_segment_downloading_->setCheckable(true);
    view_segment_downloading_->setChecked(true);
    seg_group->addButton(view_segment_downloading_);
    connect(view_segment_downloading_, &QPushButton::clicked, this, [this]() {
        set_view_mode(DownloadViewMode::Downloading);
    });
    seg_layout->addWidget(view_segment_downloading_);

    view_segment_completed_ = new QPushButton(tr("已完成"), view_segmented_);
    view_segment_completed_->setObjectName("viewSegmentButton");
    view_segment_completed_->setCheckable(true);
    seg_group->addButton(view_segment_completed_);
    connect(view_segment_completed_, &QPushButton::clicked, this, [this]() {
        set_view_mode(DownloadViewMode::Completed);
    });
    seg_layout->addWidget(view_segment_completed_);

    header_layout_->addWidget(view_segmented_);
    header_layout_->addStretch();

    refresh_button_ = new QPushButton(tr("刷新"), this);
    refresh_button_->setObjectName("toolButton");
    connect(refresh_button_, &QPushButton::clicked, this, &DownloadPage::on_refresh_clicked);
    header_layout_->addWidget(refresh_button_);

    // 按钮文字 = 点击后切换到的目标视图（默认卡片视图 → 提示「列表视图」）
    style_toggle_button_ = new QPushButton(
        display_style_ == TaskDisplayStyle::Table ? tr("卡片视图") : tr("列表视图"), this);
    style_toggle_button_->setObjectName("toolButton");
    connect(style_toggle_button_, &QPushButton::clicked, this, &DownloadPage::on_style_toggle_clicked);
    header_layout_->addWidget(style_toggle_button_);

    more_button_ = new QPushButton(tr("批量操作"), this);
    more_button_->setObjectName("toolButton");
    connect(more_button_, &QPushButton::clicked, this, &DownloadPage::on_more_options_clicked);
    header_layout_->addWidget(more_button_);
}

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

    // 拖拽排序：落定换位 + 会话期间挂起快照刷新
    connect(task_table_, &TaskTableWidget::reorderRequested,
            this, &DownloadPage::on_table_reorder);
    connect(task_table_, &TaskTableWidget::dragSessionChanged,
            this, &DownloadPage::on_drag_session);

    // 列宽:文件名列弹性伸缩跟随窗口,其余列固定内容宽
    task_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    task_table_->setColumnWidth(1, 200);  // 进度(条 + 百分比)
    task_table_->setColumnWidth(2, 100);  // 大小
    task_table_->setColumnWidth(3, 100);  // 速度
    task_table_->setColumnWidth(4, 88);   // 状态
    task_table_->setColumnWidth(5, 200);  // 做种(状态 · ratio · 时长;单行不折)
    task_table_->setColumnWidth(6, 80);   // 操作

    task_table_->setObjectName("taskTable");
}

void DownloadPage::set_view_mode(DownloadViewMode mode)
{
    const bool changed = (view_mode_ != mode);
    view_mode_ = mode;
    update_header_for_mode();

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

void DownloadPage::update_header_for_mode()
{
    switch (view_mode_) {
        case DownloadViewMode::Downloading:
            hero_title_label_->setText(tr("下载中"));
            hero_description_label_->setText(tr("优先关注活跃任务、速度与剩余进度。"));
            break;
        case DownloadViewMode::Completed:
            hero_title_label_->setText(tr("已完成"));
            hero_description_label_->setText(tr("快速回看已完成内容，清理或打开文件目录。"));
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
    // 拖拽会话期间挂起刷新：行被重建会使落点行号失真，
    // 会话结束（on_drag_session false）时补刷最后一份快照
    if (drag_active_) {
        pending_tasks_ = tasks;
        has_pending_tasks_ = true;
        return;
    }

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
        record.status_text = status_display_text(snap.status);
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
    rebuild_display_positions();

    for (auto it = task_records_.begin(); it != task_records_.end(); ++it) {
        TaskRecord& record = it.value();
        const auto& snap = record.snapshot;
        record.size_text = snap.total_bytes > 0 ? format_bytes(snap.total_bytes) : "-";
        record.status_text = status_display_text(snap.status);
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

    update_summary_cards();
    update_empty_state();

    // 如果是网格视图，刷新网格显示
    if (display_style_ == TaskDisplayStyle::Grid) {
        sync_task_grid();
    }
}

bool DownloadPage::should_show(const falcon::daemon::rpc::TaskSnapshot& snapshot) const
{
    // 顶栏搜索框过滤(单点入口:表格与网格视图共用本谓词)
    if (!text_filter_.isEmpty()
            && !filename_for(snapshot).contains(text_filter_, Qt::CaseInsensitive)) {
        return false;
    }

    switch (view_mode_) {
        case DownloadViewMode::Downloading:
            // Failed 归入下载中视图（语义见 services/task_visibility.hpp）：
            // 失败任务可「继续」重试，两个视图都不可见 = 凭空消失
            return task_visibility::visible_in_downloading(snapshot.status);
        case DownloadViewMode::Completed:
            return task_visibility::visible_in_completed(snapshot.status);
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
        display_pos_.insert(static_cast<qulonglong>(sorted[static_cast<int>(i)]),
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
    // 空态文案按视图区分（用户 bug 反馈：两个 tab 共用「还没有任务」）。
    // 构造早期 empty_state_title_ 尚未建立，title/body 指针判空兜底。
    if (empty_state_title_) {
        if (view_mode_ == DownloadViewMode::Completed) {
            empty_state_title_->setText(tr("还没有已完成的任务"));
            empty_state_body_->setText(tr("完成的下载会出现在这里，可重新下载或打开文件目录。"));
        } else {
            empty_state_title_->setText(tr("还没有任务"));
            empty_state_body_->setText(tr("点击“新建下载”，或在顶部直接粘贴链接开始。"));
        }
    }

    // 三页栈互斥切换（页由 setup_ui 构建完成后才可达——构造早期防御）
    if (!content_stack_) {
        return;
    }
    const bool has_rows = task_table_ && task_table_->rowCount() > 0;
    if (!has_rows) {
        content_stack_->setCurrentWidget(empty_page_);
    } else if (display_style_ == TaskDisplayStyle::Grid) {
        content_stack_->setCurrentWidget(grid_container_);
    } else {
        content_stack_->setCurrentWidget(task_table_);
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

QString DownloadPage::speed_display_text(
    const falcon::daemon::rpc::TaskSnapshot& snapshot)
{
    const bool active = snapshot.status == falcon::TaskStatus::Downloading ||
                        snapshot.status == falcon::TaskStatus::Preparing;
    return active ? format_speed(snapshot.speed) : QStringLiteral("—");
}

QString DownloadPage::seeding_text(
    const falcon::daemon::rpc::TaskSnapshot& snapshot)
{
    // 快照只有 seeding_active 布尔量,无法区分"达标停止"与"手动停止"——
    // 统一显示"已停止",细节(达标或手动)由 tooltip/托盘通知说明
    if (snapshot.seeding_active) {
        return QObject::tr("做种中");
    }
    if (snapshot.seed_uploaded_bytes > 0) {
        return QObject::tr("已停止");
    }
    return QString();
}

QString DownloadPage::seed_ratio_text(
    const falcon::daemon::rpc::TaskSnapshot& snapshot)
{
    const double ratio = snapshot.seed_ratio();
    if (ratio <= 0.0) {
        return QString();
    }
    return QString::number(ratio, 'f', 2);
}

QString DownloadPage::seed_duration_text(
    const falcon::daemon::rpc::TaskSnapshot& snapshot)
{
    if (snapshot.seeded_seconds < 60.0) {
        return QString();
    }
    const auto total_minutes = static_cast<long long>(snapshot.seeded_seconds / 60.0);
    if (total_minutes >= 60) {
        return QObject::tr("%1时%2分")
            .arg(total_minutes / 60)
            .arg(total_minutes % 60);
    }
    return QObject::tr("%1分").arg(total_minutes);
}

// 做种列汇总文本:"做种中 · 1.20 · 2时15分";无做种信息返回空串
QString DownloadPage::seed_column_text(
    const falcon::daemon::rpc::TaskSnapshot& snapshot)
{
    const QString status = seeding_text(snapshot);
    if (status.isEmpty()) {
        return QString();
    }
    QString text = status;
    if (const QString ratio = seed_ratio_text(snapshot); !ratio.isEmpty()) {
        text += QStringLiteral(" · ") + ratio;
    }
    if (const QString duration = seed_duration_text(snapshot); !duration.isEmpty()) {
        text += QStringLiteral(" · ") + duration;
    }
    return text;
}

// 做种 tooltip:上传/下载量明细(表格列宽装不下,收进悬浮提示)
QString DownloadPage::seed_column_tooltip(
    const falcon::daemon::rpc::TaskSnapshot& snapshot)
{
    QStringList lines;
    lines << tr("上传 %1 / 下载 %2")
                 .arg(format_bytes(snapshot.seed_uploaded_bytes),
                      format_bytes(snapshot.seed_downloaded_bytes));
    if (!snapshot.seeding_active) {
        lines << tr("做种已结束(达标或手动停止)");
    }
    return lines.join(QLatin1Char('\n'));
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

    return card;
}

} // namespace falcon::desktop
