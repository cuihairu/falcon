/**
 * @file theme_manager.hpp
 * @brief 主题管理器
 *
 * 样式表来自 resources/styles/fluent_light.qss / fluent_dark.qss(qrc),
 * 应用时同步设置 QPalette(未覆盖控件融合)与 icon_utils 当前主题
 * (themed 图标换色)。
 *
 * @author Falcon Team
 * @date 2026-09-17
 */

#pragma once

#include <QObject>
#include <QString>

namespace falcon::desktop {

/**
 * @brief 主题类型
 */
enum class ThemeType {
    Light,  // 亮色主题
    Dark    // 暗色主题
};

/**
 * @brief 主题管理器
 *
 * 管理应用主题切换和样式表加载
 */
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    explicit ThemeManager(QObject* parent = nullptr);
    ~ThemeManager() override;

    /**
     * @brief 获取当前主题
     */
    ThemeType current_theme() const { return current_theme_; }

    /**
     * @brief 设置主题
     */
    void set_theme(ThemeType theme);

    /**
     * @brief 切换主题
     */
    void toggle_theme();

    /**
     * @brief 获取指定主题的样式表(qrc 加载,结果缓存)
     */
    QString stylesheet(ThemeType theme) const;

    /**
     * @brief 应用样式表到应用(QPalette + QSS + 图标主题同步)
     */
    void apply_stylesheet();

    /**
     * @brief 获取主题名称
     */
    static QString theme_name(ThemeType theme);

signals:
    /**
     * @brief 主题改变信号(仅主题实际变化时发射)
     */
    void theme_changed(ThemeType theme);

private:
    ThemeType current_theme_;
    mutable QString light_cache_;
    mutable QString dark_cache_;
};

} // namespace falcon::desktop
