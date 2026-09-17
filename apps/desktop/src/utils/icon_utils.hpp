/**
 * @file icon_utils.hpp
 * @brief 内嵌 SVG 图标工具(Lucide 风格,主题感知着色)
 *
 * 图标资源位于 resources/icons/*.svg(stroke="currentColor"),渲染时按
 * 当前主题 token 着色。themed() 返回的 QIcon 绑定语义色角色,主题切换后
 * 已设置的按钮图标无需重新赋值即可换色。
 *
 * 图标来源:Lucide v0.294.0(ISC License),restore.svg 基于 Lucide "copy"
 * 形状手写;许可证见 resources/icons/ 各文件头部注释。
 *
 * @author Falcon Team
 * @date 2026-09-17
 */

#pragma once

#include "theme_manager.hpp"

#include <QColor>
#include <QIcon>
#include <QSize>

namespace falcon::desktop::icons {

/** 图标标识(qrc 文件见 resources/icons/) */
enum class Id {
    Minus,
    Square,
    Restore,
    X,
    Pause,
    Play,
    Trash,
    Folder,
    FolderOpen,
    FolderPlus,
    Download,
    Upload,
    Cloud,
    Settings,
    Search,
    LayoutGrid,
    List,
    Plus,
    Refresh,
    Link,
    MoreHorizontal,
    ArrowUp,
    ArrowDown,
    CheckCircle,
    AlertCircle,
    Clock,
    File,
};

/** 语义着色角色:取值来自当前主题 ThemeTokens */
enum class ColorRole {
    Text,          // 主文字色
    TextSecondary, // 次要文字色
    Accent,        // 品牌强调色
    AccentText,    // accent 底上的文字色
    Danger,        // 危险操作色
};

/**
 * @brief 同步当前主题(icon_utils 无 QObject 依赖,由 ThemeManager 通知)
 *
 * ThemeManager::apply_stylesheet() 时调用,使已存在的 themed 图标换色。
 */
void set_current_theme(ThemeType theme);

/**
 * @brief 以指定颜色渲染图标(一次性着色,不随主题变化)
 */
QIcon make(Id id, const QColor& color, QSize size = QSize(16, 16));

/**
 * @brief 主题感知图标:每次绘制时按当前主题 token 取色
 *
 * 绘制尺寸由使用方控件请求决定,无需在此指定。
 */
QIcon themed(Id id, ColorRole role = ColorRole::Text);

} // namespace falcon::desktop::icons
