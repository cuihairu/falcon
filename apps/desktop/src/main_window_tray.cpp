/**
 * @file main_window_tray.cpp
 * @brief MainWindow tray icon, clipboard monitor, float widget and theme slots
 */

#include "main_window.hpp"
#include "pages/settings_page.hpp"
#include "services/download_service.hpp"
#include "utils/clipboard_monitor.hpp"
#include "utils/icon_utils.hpp"
#include "utils/theme_manager.hpp"
#include "widgets/speed_float_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QScreen>
#include <QSettings>
#include <QSystemTrayIcon>

using namespace falcon::desktop;

void MainWindow::setup_clipboard_monitor()
{
    QClipboard* clipboard = QApplication::clipboard();
    clipboard_monitor_ = new ClipboardMonitor(clipboard, this);

    // 连接URL检测信号
    connect(clipboard_monitor_, &ClipboardMonitor::url_detected, this, &MainWindow::on_url_detected);

    // 默认不启动，由设置页面控制
    // clipboard_monitor_->start();
}

void MainWindow::setup_speed_float()
{
    speed_float_ = new SpeedFloatWidget(nullptr); // 独立顶层悬浮窗
    speed_float_->apply_theme(theme_manager_->current_theme());

    // 拖动结束即持久化位置（不等设置页"应用"——位置是即时的空间状态）
    connect(speed_float_, &SpeedFloatWidget::position_changed, this,
            [this](const QPoint& pos) {
                QSettings settings;
                settings.beginGroup("desktop");
                settings.setValue("float_pos_x", pos.x());
                settings.setValue("float_pos_y", pos.y());
                settings.endGroup();
            });

    connect(theme_manager_, &ThemeManager::theme_changed, speed_float_,
            &SpeedFloatWidget::apply_theme);

    // 家族切换与主题切换对称:家族仅改色相不动主题类型,浮窗经
    // apply_family 重取家族×主题 token 重渲(theme_changed 不发射)
    connect(theme_manager_, &ThemeManager::family_changed, speed_float_,
            &SpeedFloatWidget::apply_family);

    // 浮窗按钮/右键菜单的暂停/继续请求 → 下载服务（与下载页行内按钮同路径）
    connect(speed_float_, &SpeedFloatWidget::pause_resume_requested, this,
            [this](std::uint64_t task_id, bool pause) {
                if (!download_service_) {
                    return;
                }
                const falcon::TaskId id = static_cast<falcon::TaskId>(task_id);
                if (pause) {
                    download_service_->pause_task(id);
                } else {
                    download_service_->resume_task(id);
                }
            });

    // B21③: 右键「关闭浮窗」= 关闭并持久化隐藏。回写设置页复选框
    // （setChecked 触发 toggled → float_widget_toggled 处理器统一收口
    // 显隐 + 落盘，无第二路径）
    connect(speed_float_, &SpeedFloatWidget::close_requested, this, [this]() {
        if (settings_page_) {
            settings_page_->set_float_widget_enabled(false);
        } else if (speed_float_) {
            speed_float_->setVisible(false);
        }
    });

    // 恢复上次位置；无记录默认停靠主屏右下角
    QSettings settings;
    settings.beginGroup("desktop");
    const int x = settings.value("float_pos_x", -1).toInt();
    const int y = settings.value("float_pos_y", -1).toInt();
    settings.endGroup();
    if (x >= 0 && y >= 0) {
        speed_float_->move(x, y);
    } else if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        speed_float_->move(available.right() - speed_float_->width() - 24,
                           available.bottom() - speed_float_->height() - 48);
    }
}

void MainWindow::setup_system_tray()
{
    // 检查系统是否支持托盘
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    // 创建托盘图标
    system_tray_ = new QSystemTrayIcon(this);

    // 设置托盘图标：应用 logo（qrc 内联 SVG，主题引擎按需缩放）
    system_tray_->setIcon(QIcon(":/icons/falcon.svg"));

    // 创建托盘菜单
    tray_menu_ = new QMenu(this);

    auto* show_action = tray_menu_->addAction(tr("显示主窗口"));
    show_action->setIcon(QIcon(":/icons/falcon.svg"));
    connect(show_action, &QAction::triggered, this, &MainWindow::on_tray_show_clicked);

    tray_menu_->addSeparator();

    auto* quit_action = tray_menu_->addAction(tr("退出"));
    quit_action->setIcon(icons::themed(icons::Id::X));
    connect(quit_action, &QAction::triggered, this, &MainWindow::on_tray_quit_clicked);

    system_tray_->setContextMenu(tray_menu_);

    // 连接托盘激活信号
    connect(system_tray_, &QSystemTrayIcon::activated, this, &MainWindow::on_tray_activated);

    // 显示托盘图标
    system_tray_->show();

    // 设置托盘提示
    system_tray_->setToolTip(tr("Falcon 下载器"));
}

void MainWindow::on_tray_activated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
        case QSystemTrayIcon::Trigger:
        case QSystemTrayIcon::DoubleClick:
            // 单击或双击托盘图标，切换窗口可见性
            if (isVisible()) {
                hide();
            } else {
                showNormal();
                activateWindow();
                raise();
            }
            break;
        case QSystemTrayIcon::MiddleClick:
            // 中键点击显示主窗口
            showNormal();
            activateWindow();
            raise();
            break;
        default:
            break;
    }
}

void MainWindow::on_tray_show_clicked()
{
    showNormal();
    activateWindow();
    raise();
}

void MainWindow::on_tray_quit_clicked()
{
    // 保存设置后退出
    save_settings();
    // B21③: 托盘退出显式收窗——悬浮窗是独立顶层窗口，显式 close 保证
    // 与主窗同步消失不残留（QApplication::quit 只是退出事件循环）
    if (speed_float_) {
        speed_float_->close();
    }
    QApplication::quit();
}

void MainWindow::on_theme_mode_changed(int mode)
{
    if (!theme_manager_) {
        return;
    }
    // 下拉索引与 ThemeMode 枚举对齐(0=浅色 1=深色 2=跟随系统);越界防御
    if (mode < static_cast<int>(ThemeMode::Light) ||
        mode > static_cast<int>(ThemeMode::System)) {
        return;
    }
    theme_manager_->set_theme_mode(static_cast<ThemeMode>(mode));

    // 持久化模式键(构造器同款字符串口径);解析出的主题同步写入 theme
    // 键,保持旧读取路径(theme 作 theme_mode 缺省)与顶栏切换路径一致
    QSettings settings;
    settings.beginGroup("desktop");
    const auto m = static_cast<ThemeMode>(mode);
    settings.setValue("theme_mode",
                      m == ThemeMode::System ? "system"
                      : (m == ThemeMode::Dark ? "dark" : "light"));
    settings.setValue("theme",
                      theme_manager_->current_theme() == ThemeType::Dark ? "dark" : "light");
    settings.endGroup();
    settings.sync();
}

void MainWindow::on_theme_family_changed(int family)
{
    if (!theme_manager_) {
        return;
    }
    // 下拉索引与 ThemeFamily 枚举对齐(0=Warm 1=Cold 2=Graphite);越界防御
    if (family < static_cast<int>(ThemeFamily::Warm) ||
        family > static_cast<int>(ThemeFamily::Graphite)) {
        return;
    }
    theme_manager_->set_family(static_cast<ThemeFamily>(family));

    QSettings settings;
    settings.beginGroup("desktop");
    const auto f = static_cast<ThemeFamily>(family);
    settings.setValue("theme_family",
                      f == ThemeFamily::Cold ? "cold"
                      : (f == ThemeFamily::Graphite ? "graphite" : "warm"));
    settings.endGroup();
    settings.sync();
}
