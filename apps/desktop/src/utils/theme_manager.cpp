/**
 * @file theme_manager.cpp
 * @brief 主题管理器实现
 * @author Falcon Team
 * @date 2026-09-17
 */

#include "theme_manager.hpp"

#include "theme_tokens.hpp"
#include "icon_utils.hpp"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace falcon::desktop {

namespace {

QString load_qss(const QString& qrc_path)
{
    QFile file(qrc_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("ThemeManager: 无法读取样式表 %s", qPrintable(qrc_path));
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

/**
 * fluent_*.qss 是 {{token}} 占位符模板,按 token 集逐项填充为可用的
 * 样式表。占位符名与 ThemeTokens 字段的对应关系见 theme_tokens.hpp
 * 字段注释(注意:{{surface}} 是卡片底 t.card,{{card}} 是卡片描边
 * t.card_border——占位符命名沿模板改造前的任务书)。
 */
QString render_qss(QString qss, const ThemeTokens& t)
{
    auto sub = [&qss](const char* placeholder, const QColor& color) {
        qss.replace(QLatin1String(placeholder), color.name(QColor::HexRgb));
    };
    sub("{{window}}", t.window);
    sub("{{surface}}", t.card);
    sub("{{card}}", t.card_border);
    sub("{{text}}", t.text);
    sub("{{text_secondary}}", t.text_secondary);
    sub("{{text_disabled}}", t.text_disabled);
    sub("{{input_bg}}", t.input_bg);
    sub("{{divider}}", t.control_border);
    sub("{{divider_weak}}", t.card_border_hover);
    sub("{{surface_deep}}", t.surface_deep);
    sub("{{accent}}", t.accent);
    sub("{{accent_hover}}", t.accent_hover);
    sub("{{accent_pressed}}", t.accent_pressed);
    sub("{{accent_text}}", t.accent_text);
    sub("{{danger}}", t.danger);
    sub("{{danger_pressed}}", t.danger_pressed);
    sub("{{hover_bg}}", t.hover_bg);
    sub("{{primary_disabled_bg}}", t.primary_disabled_bg);
    return qss;
}

/**
 * 未被 QSS 覆盖的原生部件(原生菜单箭头、对话框按钮、滚动细节)
 * 融合同一套 token,避免与 QSS 混绘两套配色。
 */
QPalette palette_for(const ThemeTokens& t)
{
    QPalette pal;
    pal.setColor(QPalette::Window, t.window);
    pal.setColor(QPalette::WindowText, t.text);
    pal.setColor(QPalette::Base, t.card);
    pal.setColor(QPalette::AlternateBase, t.window);
    pal.setColor(QPalette::Text, t.text);
    pal.setColor(QPalette::PlaceholderText, t.text_disabled);
    pal.setColor(QPalette::Button, t.card);
    pal.setColor(QPalette::ButtonText, t.text);
    pal.setColor(QPalette::BrightText, t.text);
    pal.setColor(QPalette::Highlight, t.accent);
    pal.setColor(QPalette::HighlightedText, t.accent_text);
    pal.setColor(QPalette::Link, t.accent);
    pal.setColor(QPalette::ToolTipBase, t.card);
    pal.setColor(QPalette::ToolTipText, t.text);
    pal.setColor(QPalette::Light, t.card);
    pal.setColor(QPalette::Midlight, t.divider);
    pal.setColor(QPalette::Mid, t.divider);
    pal.setColor(QPalette::Dark, t.divider);
    pal.setColor(QPalette::Disabled, QPalette::Window, t.window);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, t.text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::Text, t.text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, t.text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::Base, t.window);
    return pal;
}

} // namespace

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent)
    , current_theme_(ThemeType::Dark)
    , current_family_(ThemeFamily::Warm)
{
}

ThemeManager::~ThemeManager() = default;

void ThemeManager::set_theme(ThemeType theme)
{
    const bool changed = current_theme_ != theme;
    current_theme_ = theme;
    apply_stylesheet();
    if (changed) {
        emit theme_changed(theme);
    }
}

void ThemeManager::toggle_theme()
{
    set_theme(current_theme_ == ThemeType::Light ? ThemeType::Dark : ThemeType::Light);
}

void ThemeManager::set_family(ThemeFamily family)
{
    current_family_ = family;
    // 缓存按主题各存一份,家族切换后必须作废重渲
    light_cache_.clear();
    dark_cache_.clear();
    apply_stylesheet();
}

QString ThemeManager::stylesheet(ThemeType theme) const
{
    // 缓存按主题各存一份;set_family 家族切换时已作废,故缓存恒对应
    // current_family_ 当前值。
    const char* qrc_path = theme == ThemeType::Light
                               ? ":/styles/fluent_light.qss"
                               : ":/styles/fluent_dark.qss";
    QString& cache = theme == ThemeType::Light ? light_cache_ : dark_cache_;
    if (cache.isEmpty()) {
        cache = render_qss(load_qss(QLatin1String(qrc_path)), tokens_for(current_family_, theme));
    }
    return cache;
}

void ThemeManager::apply_stylesheet()
{
    // Fusion 基座:跨平台一致的控件绘制,未覆盖细节吃 QPalette(token 融合)
    if (qApp->style()->objectName().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0) {
        qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    const ThemeTokens tokens = tokens_for(current_theme_);
    icons::set_current_theme(current_theme_);
    qApp->setPalette(palette_for(tokens));
    qApp->setStyleSheet(stylesheet(current_theme_));
}

QString ThemeManager::theme_name(ThemeType theme)
{
    switch (theme) {
        case ThemeType::Light:
            return tr("亮色");
        case ThemeType::Dark:
            return tr("暗色");
    }
    return QString();
}

} // namespace falcon::desktop
