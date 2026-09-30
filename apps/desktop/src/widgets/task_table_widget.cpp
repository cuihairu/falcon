/**
 * @file task_table_widget.cpp
 * @brief 支持行拖拽排序的任务表格实现
 * @author Falcon Team
 * @date 2026-09-30
 */

#include "task_table_widget.hpp"

#include <QDropEvent>

namespace falcon::desktop {

TaskTableWidget::TaskTableWidget(QWidget* parent)
    : QTableWidget(parent)
{
    // 行拖拽排序配置（选中行整体拖动，显示落点指示线）
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDragDropOverwriteMode(false);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(true);
}

void TaskTableWidget::startDrag(Qt::DropActions actions)
{
    // QDrag::exec() 同步阻塞到拖放结束——以此括起整个会话，
    // 落定(dropEvent)/取消/拖出窗外全部收敛到这里的 false
    emit dragSessionChanged(true);
    QTableWidget::startDrag(actions);
    emit dragSessionChanged(false);
}

void TaskTableWidget::dropEvent(QDropEvent* event)
{
    if (event->source() != this) {
        QTableWidget::dropEvent(event);  // 外部来源（目前不存在）走基类
        return;
    }

    const int from_row = currentRow();
    int to_row = rowAt(event->position().y());
    if (to_row < 0) {
        // 落在末行下方的空白区 → 移到末尾
        to_row = rowCount() - 1;
    }
    to_row = qBound(0, to_row, rowCount() - 1);

    event->setDropAction(Qt::MoveAction);
    event->accept();
    // 不调基类：QTableWidget 的 InternalMove drop 是单元格粘贴语义，
    // 整行换位由 DownloadPage 依 reorderRequested 完成
    if (from_row >= 0 && from_row != to_row && rowCount() > 0) {
        emit reorderRequested(from_row, to_row);
    }
}

} // namespace falcon::desktop
