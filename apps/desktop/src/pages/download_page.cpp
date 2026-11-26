/**
 * @file download_page.cpp
 * @brief Download Page Implementation (Xunlei-style)
 * @author Falcon Team
 * @date 2026-04-15
 */
#include "download_page.hpp"
#include "download_page_detail.hpp"
#include "services/task_visibility.hpp"
#include "widgets/task_table_widget.hpp"

#include <QButtonGroup>
#include <QFileInfo>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSet>

#include <algorithm>
#include <limits>

namespace falcon::desktop {

// 布局常量与列下标单一事实源在 download_page_detail.hpp，本 TU 与
// table/grid/formatting 三个拆分 TU 共用
using download_page_detail::kRowHeight;
using download_page_detail::kSummaryCardWidth;
using download_page_detail::kHeroHeight;
using download_page_detail::kSummaryCardHeight;
using download_page_detail::kEmptyStateMinHeight;
using download_page_detail::kColFilename;
using download_page_detail::kColSize;
using download_page_detail::kColStatus;
using download_page_detail::kColSpeed;
using download_page_detail::kColProgress;
using download_page_detail::kColSeed;
using download_page_detail::kColAction;
using download_page_detail::status_display_text;

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

    auto create_card = [this, summary_layout](const QString& caption,
                                               QLabel** value_label,
                                               QLabel** caption_label) {
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
        *caption_label = label;
        summary_layout->addWidget(card);
    };

    // 两张卡按视图分派 caption+value（update_summary_cards）：
    // 下载中 = 活跃任务 + 当前速度；已完成 = 已完成 + 总大小。
    // 初始 caption 对应构造默认视图（Downloading）
    create_card(tr("活跃任务"), &summary_value_first_, &summary_caption_first_);
    create_card(tr("当前速度"), &summary_value_second_, &summary_caption_second_);
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
    const auto slash = url.lastIndexOf('/');
    QString name = (slash >= 0 && slash < url.length() - 1) ? url.mid(slash + 1) : url;
    const auto query = name.indexOf('?');
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
    update_column_visibility();
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


void DownloadPage::update_summary_cards()
{
    // 两卡按视图分派（用户反馈：下载中视图出现已完成信息、反之亦然）：
    // - 下载中 = 活跃任务数 + 当前速度合计
    // - 已完成 = 已完成数 + 总大小合计
    // 计数口径 = should_show 全集（含 Failed 等 task_visibility 判定 +
    // 顶栏过滤词），卡数字与列表行数自洽，不再泄漏另一视图的信息
    int count = 0;
    uint64_t aggregate = 0;
    const bool downloading_view = (view_mode_ == DownloadViewMode::Downloading);

    for (auto it = task_records_.cbegin(); it != task_records_.cend(); ++it) {
        const auto& snap = it.value().snapshot;
        if (!should_show(snap)) {
            continue;
        }
        ++count;
        aggregate += downloading_view ? snap.speed : snap.total_bytes;
    }

    if (summary_value_first_) {
        summary_value_first_->setText(QString::number(count));
    }
    if (summary_value_second_) {
        summary_value_second_->setText(downloading_view ? format_speed(aggregate)
                                                        : format_bytes(aggregate));
    }

    // caption 随视图切换（标签构造期只建一次，文本在此统一更新）
    if (summary_caption_first_ && summary_caption_second_) {
        if (downloading_view) {
            summary_caption_first_->setText(tr("活跃任务"));
            summary_caption_second_->setText(tr("当前速度"));
        } else {
            summary_caption_first_->setText(tr("已完成"));
            summary_caption_second_->setText(tr("总大小"));
        }
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


} // namespace falcon::desktop
