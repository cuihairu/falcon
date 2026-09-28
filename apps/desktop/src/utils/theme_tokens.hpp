/**
 * @file theme_tokens.hpp
 * @brief 「warm console」语义色彩 token(亮/暗两套)
 *
 * 设计语言与文档站(docs/)同源:暖石色中性底 + falcon 橙单强调色,
 * 全线产品共享一套品牌配色。QSS 资源文件(fluent_light.qss /
 * fluent_dark.qss)与图标着色共用同一组 token 值;调色只改这里与两份
 * QSS,禁止在页面代码里出现硬编码颜色。
 *
 * 对比度基线(WCAG AA):亮 accent #c2410c 于亮底 4.8:1、白字于其上
 * 5.0:1;暗 accent #ffa07a 于暗底 9.0:1、深咖啡字 #27140a 于其上
 * 9.1:1(与文档站同值同验)。
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
    QColor window;            // 窗体底(暖石/暖黑)
    QColor card;              // 卡片/表面
    QColor text;              // 主文字
    QColor text_secondary;    // 次要文字
    QColor text_disabled;     // 禁用文字
    QColor accent;            // 品牌强调色(交互色)
    QColor accent_hover;
    QColor accent_pressed;
    QColor accent_text;       // accent 底上的文字(dark 下 accent 变亮,文字转深)
    QColor divider;           // 分隔线
    QColor hover_overlay;     // 行/项悬停覆盖层(低对比,保持中性)
    QColor danger;            // 危险操作(close hover / 删除)
};

/**
 * @brief 按主题返回 token 集(与 resources/styles/fluent_*.qss 数值保持一致)
 */
inline ThemeTokens tokens_for(ThemeType theme)
{
    ThemeTokens t;
    if (theme == ThemeType::Dark) {
        t.window          = QColor(0x1b, 0x17, 0x14);
        t.card            = QColor(0x26, 0x20, 0x19);
        t.text            = QColor(0xf2, 0xed, 0xe8);
        t.text_secondary  = QColor(0xb5, 0xac, 0xa3);
        t.text_disabled   = QColor(0x6e, 0x66, 0x5e);
        t.accent          = QColor(0xff, 0xa0, 0x7a);
        t.accent_hover    = QColor(0xff, 0xb2, 0x8c);
        t.accent_pressed  = QColor(0xe0, 0x87, 0x5e);
        t.accent_text     = QColor(0x27, 0x14, 0x0a);
        t.divider         = QColor(0x37, 0x30, 0x2a);
        t.hover_overlay   = QColor(255, 255, 255, 15);   // rgba(255,255,255,0.06)
        t.danger          = QColor(0xc4, 0x2b, 0x1c);
    } else {
        t.window          = QColor(0xf6, 0xf4, 0xf1);
        t.card            = QColor(0xff, 0xff, 0xff);
        t.text            = QColor(0x29, 0x25, 0x24);
        t.text_secondary  = QColor(0x57, 0x53, 0x4e);
        t.text_disabled   = QColor(0xa8, 0xa2, 0x9e);
        t.accent          = QColor(0xc2, 0x41, 0x0c);
        t.accent_hover    = QColor(0x9a, 0x34, 0x12);
        t.accent_pressed  = QColor(0x7c, 0x2d, 0x12);
        t.accent_text     = QColor(0xff, 0xff, 0xff);
        t.divider         = QColor(0xe7, 0xe5, 0xe4);
        t.hover_overlay   = QColor(0, 0, 0, 10);         // rgba(0,0,0,0.04)
        t.danger          = QColor(0xc4, 0x2b, 0x1c);
    }
    return t;
}

} // namespace falcon::desktop
