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

class QEvent;

namespace falcon::desktop {

/**
 * @brief 主题类型
 */
enum class ThemeType {
    Light,  // 亮色主题
    Dark    // 暗色主题
};

/**
 * @brief 主题家族(整体色相方向):warm 现行暖石底,cold/graphite
 *        为冷中性底变体(强调色分别为克制的蓝与绿)
 */
enum class ThemeFamily {
    Warm,
    Cold,
    Graphite,
};

/**
 * @brief 主题模式(设置页「主题」三级切换):显式浅色/深色,或跟随系统
 *        (桌面亮暗由平台主题决定,系统切换时动态跟随)
 */
enum class ThemeMode {
    Light,
    Dark,
    System,
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
     * @brief 获取当前主题家族
     */
    ThemeFamily family() const { return current_family_; }

    /**
     * @brief 获取当前主题模式
     */
    ThemeMode mode() const { return current_mode_; }

    /**
     * @brief 设置主题模式:浅色/深色直接生效,跟随系统按当前系统亮暗
     *        解析出主题并生效;系统主题变化时经 ThemeChange 事件重解析
     */
    void set_theme_mode(ThemeMode mode);

    /**
     * @brief 探测系统亮暗(Qt >= 6.5 用平台 colorScheme,更早回落窗口底色)
     */
    static ThemeType detect_system_theme();

    /**
     * @brief 设置主题家族(清样式表缓存并重新应用;家族实际变化时发射
     *        family_changed——悬浮速度窗等局部 QSS 消费方据此跟随)
     */
    void set_family(ThemeFamily family);

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

    /**
     * @brief 获取主题家族名称(设置页下拉文案)
     */
    static QString family_name(ThemeFamily family);

    /**
     * @brief 获取主题模式名称(设置页下拉文案)
     */
    static QString mode_name(ThemeMode mode);

protected:
    /**
     * @brief 平台主题变化转发(跟随系统模式下重解析亮暗)
     */
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    /**
     * @brief 主题改变信号(仅主题实际变化时发射)
     */
    void theme_changed(ThemeType theme);

    /**
     * @brief 主题家族改变信号(仅家族实际变化时发射)
     *
     * 独立于 theme_changed:家族切换不改变主题类型,应用整体经
     * apply_stylesheet() 重渲,悬浮速度窗等局部 QSS 消费方经本信号跟随。
     */
    void family_changed(ThemeFamily family);

private:
    ThemeType current_theme_;
    ThemeFamily current_family_;
    ThemeMode current_mode_;
    mutable QString light_cache_;
    mutable QString dark_cache_;
};

} // namespace falcon::desktop
