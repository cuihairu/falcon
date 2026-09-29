/**
 * @file trash_page.hpp
 * @brief 回收站页面：已删除任务的记录与成品文件（恢复/彻底删除/清空）
 * @author Falcon Team
 * @date 2026-09-28
 */

#pragma once

#include <services/trash_store.hpp>

#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace falcon::desktop {

/**
 * @brief 回收站页面
 *
 * 纯展示组件：set_entries() 全量替换条目并重建表格；操作经信号交由
 * MainWindow 转发 DownloadService（worker 线程执行，trash_changed
 * 回来后再刷新）。
 */
class TrashPage : public QWidget
{
    Q_OBJECT

public:
    explicit TrashPage(QWidget* parent = nullptr);

    /// 全量替换回收站条目并刷新显示
    void set_entries(const std::vector<TrashEntry>& entries);

signals:
    void restore_requested(std::uint64_t id);
    void purge_requested(std::uint64_t id);
    void clear_requested();

private:
    void setup_ui();
    QWidget* create_page_hero();
    void rebuild_table();
    static QString format_bytes(std::uint64_t bytes);
    static QString format_deleted_at(std::int64_t epoch_seconds);
    static QString status_text(const TrashEntry& entry);

    QTableWidget* table_ = nullptr;
    QLabel* count_label_ = nullptr;
    QWidget* empty_state_ = nullptr;
    QPushButton* clear_button_ = nullptr;

    std::vector<TrashEntry> entries_;
};

} // namespace falcon::desktop
