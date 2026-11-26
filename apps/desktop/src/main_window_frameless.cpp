/**
 * @file main_window_frameless.cpp
 * @brief MainWindow frameless edge resize and window control slots
 */

#include "main_window.hpp"
#include "widgets/top_bar.hpp"

#include <QEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QWidget>

using namespace falcon::desktop;

namespace {
// 无边框窗口边缘缩放带宽度(像素)
constexpr int kResizeEdgeBand = 6;

/** 缩放边组合 → 对应的系统缩放光标形状 */
Qt::CursorShape cursor_shape_for_edges(Qt::Edges edges)
{
    if (edges == (Qt::TopEdge | Qt::LeftEdge)
            || edges == (Qt::BottomEdge | Qt::RightEdge)) {
        return Qt::SizeFDiagCursor;
    }
    if (edges == (Qt::TopEdge | Qt::RightEdge)
            || edges == (Qt::BottomEdge | Qt::LeftEdge)) {
        return Qt::SizeBDiagCursor;
    }
    if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        return Qt::SizeHorCursor;
    }
    if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}
} // namespace

//==============================================================================
// 无边框窗口:边缘缩放 + 最大化状态同步
//==============================================================================

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::WindowStateChange && top_bar_) {
        // 最大化/还原切换顶栏按钮图标(Square ↔ Restore)
        top_bar_->set_maximized(isMaximized());
    }
    QMainWindow::changeEvent(event);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::MouseMove || type == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(obj);
        // 只关心本窗口内的控件;菜单/tooltip/combo 弹窗是独立顶层窗口,
        // window() 比较天然排除。缩放仅在非最大化时生效。
        if (widget && widget->window() == static_cast<QWidget*>(this) && !isMaximized()) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const Qt::Edges edges = resize_edge_for(mouse->globalPosition().toPoint());
            if (edges != 0) {
                if (type == QEvent::MouseButtonPress
                        && mouse->button() == Qt::LeftButton && windowHandle()) {
                    windowHandle()->startSystemResize(edges);
                    return true;
                }
                if (type == QEvent::MouseMove) {
                    setCursor(cursor_shape_for_edges(edges));
                    resize_cursor_active_ = true;
                    return false;
                }
            } else if (type == QEvent::MouseMove && resize_cursor_active_) {
                // 仅在曾设过缩放光标时恢复,避免覆盖子控件自己的光标
                // (如 QLineEdit 的 IBeam)
                setCursor(Qt::ArrowCursor);
                resize_cursor_active_ = false;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

Qt::Edges MainWindow::resize_edge_for(const QPoint& global_pos) const
{
    const QPoint local = mapFromGlobal(global_pos);
    const QRect frame = rect();

    Qt::Edges edges;
    if (local.x() <= kResizeEdgeBand) {
        edges |= Qt::LeftEdge;
    }
    if (local.x() >= frame.width() - kResizeEdgeBand) {
        edges |= Qt::RightEdge;
    }
    if (local.y() <= kResizeEdgeBand) {
        edges |= Qt::TopEdge;
    }
    if (local.y() >= frame.height() - kResizeEdgeBand) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

void MainWindow::on_minimize_requested()
{
    showMinimized();
}

void MainWindow::on_maximize_requested()
{
    if (isMaximized()) {
        showNormal();
    } else {
        showMaximized();
    }
}

void MainWindow::on_close_requested()
{
    // 如果系统托盘可用，最小化到托盘而不是关闭
    if (system_tray_ && system_tray_->isVisible()) {
        hide();
    } else {
        close();
    }
}
