/**
 * @file icon_utils.cpp
 * @brief 内嵌 SVG 图标工具实现
 * @author Falcon Team
 * @date 2026-09-17
 */

#include "icon_utils.hpp"

#include "theme_tokens.hpp"

#include <QApplication>
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>
#include <atomic>

namespace falcon::desktop::icons {

namespace {

std::atomic<ThemeType> g_theme{ThemeType::Light};

/** Id → qrc 图标路径(:/icons/<file>) */
QString icon_path(Id id)
{
    switch (id) {
        case Id::Minus:         return QStringLiteral(":/icons/minus.svg");
        case Id::Square:        return QStringLiteral(":/icons/square.svg");
        case Id::Restore:       return QStringLiteral(":/icons/restore.svg");
        case Id::X:             return QStringLiteral(":/icons/x.svg");
        case Id::Pause:         return QStringLiteral(":/icons/pause.svg");
        case Id::Play:          return QStringLiteral(":/icons/play.svg");
        case Id::Trash:         return QStringLiteral(":/icons/trash-2.svg");
        case Id::Folder:        return QStringLiteral(":/icons/folder.svg");
        case Id::FolderOpen:    return QStringLiteral(":/icons/folder-open.svg");
        case Id::FolderPlus:    return QStringLiteral(":/icons/folder-plus.svg");
        case Id::Download:      return QStringLiteral(":/icons/download.svg");
        case Id::Upload:        return QStringLiteral(":/icons/upload.svg");
        case Id::Cloud:         return QStringLiteral(":/icons/cloud.svg");
        case Id::Settings:      return QStringLiteral(":/icons/settings.svg");
        case Id::Search:        return QStringLiteral(":/icons/search.svg");
        case Id::LayoutGrid:    return QStringLiteral(":/icons/layout-grid.svg");
        case Id::List:          return QStringLiteral(":/icons/list.svg");
        case Id::Plus:          return QStringLiteral(":/icons/plus.svg");
        case Id::Refresh:       return QStringLiteral(":/icons/refresh-cw.svg");
        case Id::Link:          return QStringLiteral(":/icons/link.svg");
        case Id::MoreHorizontal: return QStringLiteral(":/icons/more-horizontal.svg");
        case Id::ArrowUp:       return QStringLiteral(":/icons/arrow-up.svg");
        case Id::ArrowDown:     return QStringLiteral(":/icons/arrow-down.svg");
        case Id::CheckCircle:   return QStringLiteral(":/icons/check-circle-2.svg");
        case Id::AlertCircle:   return QStringLiteral(":/icons/alert-circle.svg");
        case Id::Clock:         return QStringLiteral(":/icons/clock.svg");
        case Id::File:          return QStringLiteral(":/icons/file.svg");
    }
    return QString();
}

QColor role_color(ColorRole role)
{
    const ThemeTokens tokens = tokens_for(g_theme.load());
    switch (role) {
        case ColorRole::Text:          return tokens.text;
        case ColorRole::TextSecondary: return tokens.text_secondary;
        case ColorRole::Accent:        return tokens.accent;
        case ColorRole::AccentText:    return tokens.accent_text;
        case ColorRole::Danger:        return tokens.danger;
    }
    return tokens.text;
}

/** 渲染 SVG 为透明底 pixmap;着色 = 文本替换 stroke 的 currentColor */
QPixmap render_icon(Id id, const QColor& color, const QSize& size, qreal dpr)
{
    QFile file(icon_path(id));
    if (!file.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QString source = QString::fromUtf8(file.readAll());
    source.replace(QStringLiteral("currentColor"), color.name());

    QSvgRenderer renderer(source.toUtf8());
    if (!renderer.isValid()) {
        return QPixmap();
    }

    QPixmap pm(size * dpr);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);
    QPainter painter(&pm);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
    return pm;
}

/** 全局 pixmap 缓存:key 含 id/色值/尺寸/DPR,QPixmapCache 自行管理内存 */
QPixmap cached_render(Id id, const QColor& color, const QSize& size, qreal dpr)
{
    const QString key = QStringLiteral("falcon-icon/%1/%2/%3x%4/%5")
                            .arg(static_cast<int>(id))
                            .arg(color.name(QColor::HexArgb))
                            .arg(size.width())
                            .arg(size.height())
                            .arg(dpr);
    QPixmap pm;
    if (!QPixmapCache::find(key, &pm)) {
        pm = render_icon(id, color, size, dpr);
        QPixmapCache::insert(key, pm);
    }
    return pm;
}

QColor role_color_adjusted(ColorRole role, QIcon::Mode mode)
{
    QColor color = role_color(role);
    if (mode == QIcon::Disabled) {
        color.setAlphaF(color.alphaF() * 0.4);
    }
    return color;
}

/**
 * @brief 主题感知的 QIcon 引擎:绘制时实时解析当前主题 token 色
 */
class TokenIconEngine final : public QIconEngine
{
public:
    TokenIconEngine(Id id, ColorRole role)
        : id_(id)
        , role_(role)
    {
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State) override
    {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const QPixmap pm = cached_render(id_, role_color_adjusted(role_, mode),
                                         rect.size(), dpr);
        painter->drawPixmap(rect.topLeft(), pm);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State) override
    {
        const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
        return cached_render(id_, role_color_adjusted(role_, mode), size, dpr);
    }

    QIconEngine* clone() const override
    {
        return new TokenIconEngine(id_, role_);
    }

private:
    Id id_;
    ColorRole role_;
};

} // namespace

void set_current_theme(ThemeType theme)
{
    g_theme.store(theme);
}

QIcon make(Id id, const QColor& color, QSize size)
{
    QIcon icon;
    icon.addPixmap(render_icon(id, color, size, 1.0));
    icon.addPixmap(render_icon(id, color, size, 2.0));
    return icon;
}

QIcon themed(Id id, ColorRole role)
{
    // 尺寸由使用方控件的绘制请求决定,engine 按请求渲染
    return QIcon(new TokenIconEngine(id, role));
}

} // namespace falcon::desktop::icons
