/**
 * @file theme_tokens.hpp
 * @brief Fluent 风格语义色彩 token(亮/暗两套)
 *
 * QSS 资源文件(fluent_light.qss / fluent_dark.qss)与图标着色共用同一组
 * token 值;调色只改这里与两份 QSS,禁止在页面代码里出现硬编码颜色。
 *
 * @author Falcon Team
 * @date 2026-09-17
 */

#pragma once

#include "theme_manager.hpp"

#include <QColor>

namespace falcon::desktop {

/**
 * @brief 语义角色色彩集合(角色命名,非外观命名)
 */
struct ThemeTokens {
    QColor window;            // 窗体底(Mica 灰)
    QColor card;              // 卡片/表面
    QColor text;              // 主文字
    QColor text_secondary;    // 次要文字
    QColor text_disabled;     // 禁用文字
    QColor accent;            // 品牌强调色(交互色)
    QColor accent_hover;
    QColor accent_pressed;
    QColor accent_text;       // accent 底上的文字(dark 下 accent 变亮,文字转深)
    QColor divider;           // 分隔线
    QColor hover_overlay;     // 行/项悬停覆盖层(低对比)
    QColor danger;            // 危险操作(close hover / 删除)
};

/**
 * @brief 按主题返回 token 集(与 resources/styles/fluent_*.qss 数值保持一致)
 */
inline ThemeTokens tokens_for(ThemeType theme)
{
    ThemeTokens t;
    if (theme == ThemeType::Dark) {
        t.window          = QColor(0x20, 0x20, 0x20);
        t.card            = QColor(0x2b, 0x2b, 0x2b);
        t.text            = QColor(0xff, 0xff, 0xff);
        t.text_secondary  = QColor(0xc8, 0xc8, 0xc8);
        t.text_disabled   = QColor(0x6e, 0x6e, 0x6e);
        t.accent          = QColor(0x4c, 0xc2, 0xff);
        t.accent_hover    = QColor(0x47, 0xb1, 0xe8);
        t.accent_pressed  = QColor(0x00, 0x3e, 0x6b);
        t.accent_text     = QColor(0x1b, 0x1b, 0x1b);
        t.divider         = QColor(0x38, 0x38, 0x38);
        t.hover_overlay   = QColor(255, 255, 255, 15);   // rgba(255,255,255,0.06)
        t.danger          = QColor(0xc4, 0x2b, 0x1c);
    } else {
        t.window          = QColor(0xf3, 0xf3, 0xf3);
        t.card            = QColor(0xff, 0xff, 0xff);
        t.text            = QColor(0x1b, 0x1b, 0x1b);
        t.text_secondary  = QColor(0x61, 0x61, 0x61);
        t.text_disabled   = QColor(0x9d, 0x9d, 0x9d);
        t.accent          = QColor(0x00, 0x5f, 0xb8);
        t.accent_hover    = QColor(0x19, 0x75, 0xc5);
        t.accent_pressed  = QColor(0x00, 0x45, 0x78);
        t.accent_text     = QColor(0xff, 0xff, 0xff);
        t.divider         = QColor(0xe5, 0xe5, 0xe5);
        t.hover_overlay   = QColor(0, 0, 0, 10);         // rgba(0,0,0,0.04)
        t.danger          = QColor(0xc4, 0x2b, 0x1c);
    }
    return t;
}

} // namespace falcon::desktop
