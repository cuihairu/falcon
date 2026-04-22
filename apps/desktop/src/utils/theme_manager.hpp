/**
 * @file theme_manager.hpp
 * @brief 主题管理器
 * @author Falcon Team
 * @date 2026-04-22
 */

#pragma once

#include <QObject>
#include <QString>
#include <QPalette>

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
     * @brief 获取当前主题的样式表
     */
    QString get_stylesheet() const;

    /**
     * @brief 应用样式表到应用
     */
    void apply_stylesheet();

    /**
     * @brief 获取主题名称
     */
    static QString theme_name(ThemeType theme);

signals:
    /**
     * @brief 主题改变信号
     */
    void theme_changed(ThemeType theme);

private:
    QString load_light_theme() const;
    QString load_dark_theme() const;

    ThemeType current_theme_;
};

} // namespace falcon::desktop
