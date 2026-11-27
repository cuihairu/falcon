/**
 * @file download_page_formatting.cpp
 * @brief DownloadPage formatting and file-open helpers
 */

#include "download_page.hpp"
#include "download_page_detail.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QUrl>

using namespace falcon::desktop;
using falcon::desktop::download_page_detail::status_display_text;


bool DownloadPage::eventFilter(QObject* watched, QEvent* event)
{
    // B18: 网格卡片双击——卡片本体是普通 QWidget（taskId 属性在卡上，
    // 子控件忽略鼠标事件后冒泡回卡片），只在已完成视图响应
    if (event->type() == QEvent::MouseButtonDblClick &&
        view_mode_ == DownloadViewMode::Completed) {
        if (auto* card = qobject_cast<QWidget*>(watched)) {
            const QVariant task_id_var = card->property("taskId");
            if (task_id_var.isValid()) {
                if (const auto* record = record_by_id(task_id_var.toULongLong())) {
                    handle_completed_double_click(*record);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
void DownloadPage::handle_completed_double_click(const TaskRecord& record)
{
    // 双击行为每次事件时即时读取设置（设置页改动即时落盘，无需额外接线）：
    // 0 = 打开文件（默认） / 1 = 打开所在文件夹
    QSettings settings;
    const int action =
        settings.value("desktop/completed_double_click_action", 0).toInt();
    open_completed_target(record.save_path, action == 1);
}

void DownloadPage::open_completed_target(const QString& save_path, bool open_folder)
{
    if (save_path.isEmpty()) {
        QMessageBox::warning(this, tr("无法打开"),
                             tr("无法确定该任务的文件位置。"));
        return;
    }

    const QFileInfo file_info(save_path);
    const QString target = open_folder ? file_info.absolutePath()
                                       : file_info.absoluteFilePath();
    const QString target_kind = open_folder ? tr("文件夹") : tr("文件");

    if (!QFileInfo::exists(target)) {
        // B18: 文件/文件夹不存在或已被移动时明确提示（不静默），顺带给
        // 「打开所在文件夹」出口
        QMessageBox box(QMessageBox::Warning, tr("%1不存在").arg(target_kind),
                        tr("%1不存在或已被移动：\n%2").arg(target_kind, target),
                        QMessageBox::Close, this);
        QPushButton* open_dir_btn =
            box.addButton(tr("打开所在文件夹"), QMessageBox::ActionRole);
        box.exec();
        if (box.clickedButton() == open_dir_btn) {
            const QString dir = file_info.absolutePath();
            if (QFileInfo::exists(dir)) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            } else {
                QMessageBox::warning(
                    this, tr("文件夹不存在"),
                    tr("文件夹不存在或已被移动：\n%1").arg(dir));
            }
        }
        return;
    }

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        QMessageBox::warning(this, tr("打开失败"),
                             tr("系统没有找到能打开「%1」的程序。").arg(target));
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
