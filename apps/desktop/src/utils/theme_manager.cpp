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

QString ThemeManager::stylesheet(ThemeType theme) const
{
    if (theme == ThemeType::Light) {
        if (light_cache_.isEmpty()) {
            light_cache_ = load_qss(QStringLiteral(":/styles/fluent_light.qss"));
        }
        return light_cache_;
    }
    if (dark_cache_.isEmpty()) {
        dark_cache_ = load_qss(QStringLiteral(":/styles/fluent_dark.qss"));
    }
    return dark_cache_;
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
