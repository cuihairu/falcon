/**
 * @file task_table_widget.hpp
 * @brief 支持行拖拽排序的任务表格
 *
 * QTableWidget 与 QListWidget 不同：DragDropMode=InternalMove 下基类
 * dropEvent 走模型粘贴（把单元格文本粘到目标格），不会移动整行——必须
 * override dropEvent 自行换行并 accept（不调基类），行移动本身由
 * reorderRequested 信号交 DownloadPage 完成（含全局 order 重建与持久化）。
 *
 * 拖拽会话的挂起刷新：QDrag::exec() 在 startDrag 内同步阻塞直到拖放结束，
 * 以 startDrag 为同步括号发 dragSessionChanged(true/false)——DownloadPage
 * 在会话期间挂起 500ms 快照刷新（拖到一半行被重建会使落点行号失真），
 * 会话结束后补刷挂起的快照。
 *
 * @author Falcon Team
 * @date 2026-09-30
 */

#pragma once

#include <QTableWidget>

namespace falcon::desktop {

class TaskTableWidget : public QTableWidget
{
    Q_OBJECT

public:
    explicit TaskTableWidget(QWidget* parent = nullptr);

signals:
    /// 内部拖拽落定：from 行拖到 to 行（均已钳制在有效行区间）
    void reorderRequested(int from_row, int to_row);
    /// 拖拽会话开始(true)/结束(false)——startDrag 同步括号，覆盖
    /// 落定/取消/拖出窗外的全部结束路径
    void dragSessionChanged(bool active);

protected:
    void startDrag(Qt::DropActions actions) override;
    void dropEvent(QDropEvent* event) override;
};

} // namespace falcon::desktop
