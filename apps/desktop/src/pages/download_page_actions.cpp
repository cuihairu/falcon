/**
 * @file download_page_actions.cpp
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
#include <QKeySequence>
#include <QDesktopServices>
#include <QApplication>
#include <QClipboard>
#include <QScrollArea>
#include <QFrame>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QSet>
#include <QStringList>
#include <QSettings>
#include <QMessageBox>
#include <QPushButton>
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

// 表格列索引（与 create_task_table 的表头顺序对齐）：
// 下载中视图隐藏「做种」（本视图恒空），已完成视图隐藏「进度」「速度」（恒 100%/—）
// 追加列 kColExtra：下载中显示「来源」（URL 派生域名），已完成显示「保存位置」，
// 列名与内容随视图切换；追加在末尾保证原 0-6 列索引不变，拖拽/右键/持久化不受影响。
constexpr int kColFilename = 0;
constexpr int kColProgress = 1;
constexpr int kColSize = 2;
constexpr int kColSpeed = 3;
constexpr int kColStatus = 4;
constexpr int kColSeed = 5;
constexpr int kColAction = 6;
constexpr int kColExtra = 7;

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

// 追加列内容：下载中=来源域名（从 URL host 派生，无 host 时显示协议），
// 已完成=保存位置目录（output_path 的真实父目录）。均为真实数据，非占位。
QString DownloadPage::extra_column_text(const falcon::daemon::rpc::TaskSnapshot& snapshot) const
{
    if (view_mode_ == DownloadViewMode::Completed) {
        if (!snapshot.output_path.empty()) {
            const QFileInfo info(QString::fromStdString(snapshot.output_path));
            const QString dir = info.absolutePath();
            return dir.isEmpty() ? QStringLiteral("—") : dir;
        }
        return QStringLiteral("—");
    }
    const QString url = QString::fromStdString(snapshot.url);
    const QString host = QUrl(url).host();
    if (!host.isEmpty()) {
        return host;
    }
    const QString scheme = QUrl(url).scheme();
    return scheme.isEmpty() ? QStringLiteral("—") : scheme;
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
    // 列文本保持简短(做种中/已停止),停止原因(达标/手动)由 seed_stop_reason
    // 经 tooltip/托盘通知区分
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
        const QString reason =
            QString::fromStdString(snapshot.seed_stop_reason);
        if (reason == QLatin1String("manual")) {
            lines << tr("做种已手动停止");
        } else if (reason == QLatin1String("limit_reached")) {
            lines << tr("做种已达份额/时长上限，自动停止");
        } else {
            lines << tr("做种已结束(达标或手动停止)");
        }
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
        pause_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
        connect(pause_action, &QAction::triggered, this,
                [this, id = record->snapshot.id]() { emit pause_requested(id); });
    } else if (status == falcon::TaskStatus::Paused ||
               status == falcon::TaskStatus::Failed) {
        auto* resume_action = menu.addAction(tr("继续"));
        resume_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
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
        open_file_action->setShortcut(QKeySequence(QStringLiteral("Return")));
        connect(open_file_action, &QAction::triggered, this,
                [this, save_path_for_open]() {
                    open_completed_target(save_path_for_open, false);
                });
    }
    auto* open_dir_action = menu.addAction(tr("打开文件夹"));
    open_dir_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    connect(open_dir_action, &QAction::triggered, this,
            [this, save_path_for_open]() {
                open_completed_target(save_path_for_open, true);
            });

    const QString url_for_copy = QString::fromStdString(record->snapshot.url);
    auto* copy_url_action = menu.addAction(tr("复制下载链接"));
    copy_url_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+C")));
    connect(copy_url_action, &QAction::triggered, this, [url_for_copy]() {
        QApplication::clipboard()->setText(url_for_copy);
    });

    // ui-next 屏3/25：文件详情 / 收藏 / 添加到云盘 / 移动文件到 / 彻底删除。
    // 快照按值捕获——菜单 exec 期间 task_records_ 可能被轮询重建，不能捕获指针。
    const falcon::daemon::rpc::TaskSnapshot snapshot_for_menu = record->snapshot;
    const auto menu_task_id_for_detail = record->snapshot.id;

    auto* detail_action = menu.addAction(tr("文件详情"));
    connect(detail_action, &QAction::triggered, this, [this, snapshot_for_menu]() {
        emit file_details_requested(snapshot_for_menu);
    });

    auto* favorite_action = menu.addAction(tr("收藏至收藏夹"));
    connect(favorite_action, &QAction::triggered, this, [this, snapshot_for_menu]() {
        toggle_favorite(QString::fromStdString(snapshot_for_menu.output_path));
    });

    auto* add_cloud_action = menu.addAction(tr("添加到云盘"));
    connect(add_cloud_action, &QAction::triggered, this, [this]() {
        emit add_to_cloud_requested();
    });

    // 移动文件到：子菜单列预设目录（真实复制文件到目标目录，不破坏任务记录）
    auto* move_menu = menu.addMenu(tr("复制文件到"));
    const QString current_dir = QFileInfo(record->save_path).absolutePath();
    const QStringList dirs = { current_dir, current_dir + QStringLiteral("/Software"),
                               current_dir + QStringLiteral("/ISO") };
    for (const QString& dir : dirs) {
        auto* dir_action = move_menu->addAction(dir);
        connect(dir_action, &QAction::triggered, this, [this, record_save_path = record->save_path, dir]() {
            copy_file_to(record_save_path, dir);
        });
    }

    auto* purge_action = menu.addAction(tr("彻底删除"));
    // 非回收站任务：原型中「彻底删除」置灰（下载中视图）
    purge_action->setEnabled(view_mode_ == DownloadViewMode::Completed);
    connect(purge_action, &QAction::triggered, this, [this, menu_task_id_for_detail]() {
        const auto answer = QMessageBox::warning(
            this, tr("彻底删除"), tr("彻底删除不可恢复，确认移除该任务记录？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            emit remove_task_requested(static_cast<falcon::TaskId>(menu_task_id_for_detail));
        }
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

void DownloadPage::toggle_favorite(const QString& save_path)
{
    if (save_path.isEmpty()) {
        return;
    }
    QSettings settings;
    QStringList favorites = settings.value(QStringLiteral("favorites/paths")).toStringList();
    const QString key = QFileInfo(save_path).absoluteFilePath();
    if (favorites.contains(key)) {
        favorites.removeAll(key);
        QMessageBox::information(this, tr("收藏夹"), tr("已从收藏夹移除：%1").arg(key));
    } else {
        favorites.append(key);
        QMessageBox::information(this, tr("收藏夹"), tr("已加入收藏夹：%1").arg(key));
    }
    settings.setValue(QStringLiteral("favorites/paths"), favorites);
    settings.sync();
}

void DownloadPage::copy_file_to(const QString& save_path, const QString& target_dir)
{
    const QFileInfo source(save_path);
    if (!source.exists()) {
        QMessageBox::warning(this, tr("复制文件"),
                             tr("源文件不存在或已被移动：\n%1").arg(save_path));
        return;
    }
    QDir().mkpath(target_dir);
    const QString destination = QDir(target_dir).filePath(source.fileName());
    if (QFile::exists(destination)) {
        QMessageBox::warning(this, tr("复制文件"),
                             tr("目标已存在同名文件，未覆盖：\n%1").arg(destination));
        return;
    }
    if (QFile::copy(source.absoluteFilePath(), destination)) {
        QMessageBox::information(this, tr("复制文件"),
                                 tr("已复制到：\n%1\n（保留原任务记录，任务路径不变）").arg(destination));
    } else {
        QMessageBox::warning(this, tr("复制文件"), tr("复制失败：%1").arg(destination));
    }
}

} // namespace falcon::desktop
