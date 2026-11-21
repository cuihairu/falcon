/**
 * @file theme_tokens.hpp
 * @brief 语义色彩 token(主题家族 Warm/Cold/Graphite × 亮/暗)
 *
 * 「warm console」为默认家族:暖石色中性底 + falcon 橙单强调色,与
 * 文档站(docs/)同源。Cold/Graphite 两家族是冷中性底变体(取自
 * ui_sandbox --redesign 模式的色板考古)。QSS 资源文件(fluent_*.qss)
 * 已模板化为 {{token}} 占位符,由 ThemeManager::stylesheet() 按
 * tokens_for(family, theme) 逐 token 填充;图标着色共用同一组 token。
 * 调色只改这里,禁止在页面代码里出现硬编码颜色。
 *
 * 对比度基线(WCAG AA,warm 家族,按变体表色值 Python 实算):亮 text
 * 14.5:1、accent 4.8:1、白字于 accent 上 5.2:1、secondary 8.4:1;暗
 * text 16.7:1、accent 9.4:1、accent 上深字 8.9:1、secondary 8.1:1
 * ——三家族全部 24 个前景/背景配对 ≥4.5:1。
 *
 * @author Falcon Team
 * @date 2026-09-17
 */

#pragma once

#include "theme_manager.hpp"

#include <QColor>
#include <QString>

namespace falcon::desktop {

/**
 * @brief 控件形状参数(圆角,单位 px)
 *
 * 三家族初始同值——变体表只定义色板,形状差异待 Phase B
 * 对照原型裁决后按需分档。经 ThemeManager::render_qss 的 subn
 * 数值替换写入 QSS 模板占位符({{radius_card}}/{{radius_control}}/
 * {{radius_small}}),渲染产物与旧字面值逐字节一致。
 */
struct ThemeShape {
    int radius_card    = 12; // 卡片/面板/菜单
    int radius_control = 8;  // 按钮/输入/下拉/spinbox
    int radius_small   = 4;  // toolButton/行内钮/菜单项/tooltip/navTab/分段容器
};

/**
 * @brief 语义角色色彩集合(角色命名,非外观命名)
 */
struct ThemeTokens {
    QColor window;            // 窗体底(暖石/暖黑)
    QColor card;              // 卡片/表面底(QSS 占位符 {{surface}})
    QColor text;              // 主文字
    QColor text_secondary;    // 次要文字
    QColor text_disabled;     // 禁用文字
    QColor accent;            // 品牌强调色(交互色)
    QColor accent_hover;
    QColor accent_pressed;
    QColor accent_text;       // accent 底上的文字(dark 下 accent 变亮,文字转深)
    QColor divider;           // 分隔线(palette 融合用,与卡片描边同源)
    QColor hover_overlay;     // 行/项悬停覆盖层(低对比,保持中性)
    QColor danger;            // 危险操作(close hover / 删除)
    // ---- 以下字段服务 QSS 模板占位符(见 ThemeManager::stylesheet) ----
    QColor input_bg;          // {{input_bg}} 输入控件底
    QColor card_border;       // {{card}} 卡片/面板描边(占位符名沿任务书)
    QColor control_border;    // {{divider}} 控件描边/强分隔
    QColor card_border_hover; // {{divider_weak}} 卡片悬停描边
    QColor surface_deep;      // {{surface_deep}} 分段钮选中底(dark)
    QColor danger_pressed;    // {{danger_pressed}} 危险钮按压底
    QColor hover_bg;          // {{hover_bg}} 实底悬停(亮色表;dark 走 hover_overlay)
    QColor primary_disabled_bg; // {{primary_disabled_bg}} 主按钮禁用底(亮色表)
    QString checkbox_checked;   // {{checkbox_checked}} 勾选态图标 qrc 路径(家族×主题变体)
    QString checkbox_unchecked; // {{checkbox_unchecked}} 未勾选态图标 qrc 路径
    ThemeShape shape;         // 控件圆角三档(卡片 12/控件 8/小件 4)
};

namespace detail {

/**
 * @brief 单个家族×主题的 token 全量构造(与 QSS 模板原 hex 一一对应)
 *
 * divider 与 card_border 同源(卡片描边色兼任 palette 分隔线);
 * hover_bg/primary_disabled_bg 仅在亮色模板出现,dark 取邻近控件底
 * 值占位以保证字段恒有效;hover_overlay 各家族共用(rgba 不随家族变化)。
 */
inline ThemeTokens make_tokens(ThemeFamily family, ThemeType theme)
{
    ThemeTokens t;
    if (theme == ThemeType::Dark) {
        t.hover_overlay  = QColor(255, 255, 255, 15); // rgba(255,255,255,0.06)
        t.danger         = QColor(0xc4, 0x2b, 0x1c);
        t.danger_pressed = QColor(0xa5, 0x26, 0x18);
        switch (family) {
            case ThemeFamily::Warm:
                t.window          = QColor(0x16, 0x12, 0x0f);
                t.card            = QColor(0x22, 0x1c, 0x16);
                t.text            = QColor(0xf7, 0xf2, 0xec);
                t.text_secondary  = QColor(0xbc, 0xb2, 0xa6);
                t.text_disabled   = QColor(0x75, 0x6c, 0x62);
                t.accent          = QColor(0xff, 0xa0, 0x7a);
                t.accent_hover    = QColor(0xff, 0xb2, 0x8c);
                t.accent_pressed  = QColor(0xe0, 0x87, 0x5e);
                t.accent_text     = QColor(0x27, 0x14, 0x0a);
                t.divider         = QColor(0x3a, 0x32, 0x2a);
                t.input_bg        = QColor(0x2d, 0x26, 0x20);
                t.card_border     = t.divider;
                t.control_border  = QColor(0x4e, 0x45, 0x3c);
                t.card_border_hover = QColor(0x5a, 0x4f, 0x44);
                t.surface_deep    = QColor(0x36, 0x2e, 0x26);
                t.hover_bg        = t.input_bg;
                t.primary_disabled_bg = t.input_bg;
                break;
            case ThemeFamily::Cold:
                t.window          = QColor(0x13, 0x15, 0x19);
                t.card            = QColor(0x1b, 0x1e, 0x24);
                t.text            = QColor(0xf5, 0xf7, 0xfa);
                t.text_secondary  = QColor(0x98, 0xa0, 0xab);
                t.text_disabled   = QColor(0x5a, 0x61, 0x69);
                t.accent          = QColor(0x5b, 0x9d, 0xf5);
                t.accent_hover    = QColor(0x7a, 0xb1, 0xf8);
                t.accent_pressed  = QColor(0x45, 0x85, 0xe0);
                t.accent_text     = QColor(0x0d, 0x1a, 0x2b);
                t.divider         = QColor(0x2b, 0x30, 0x38);
                t.input_bg        = QColor(0x22, 0x26, 0x2d);
                t.card_border     = t.divider;
                t.control_border  = QColor(0x36, 0x3c, 0x45);
                t.card_border_hover = QColor(0x3d, 0x44, 0x4e);
                t.surface_deep    = QColor(0x26, 0x2b, 0x33);
                t.hover_bg        = t.input_bg;
                t.primary_disabled_bg = t.input_bg;
                break;
            case ThemeFamily::Graphite:
                t.window          = QColor(0x0f, 0x12, 0x14);
                t.card            = QColor(0x16, 0x1b, 0x1e);
                t.text            = QColor(0xee, 0xf2, 0xf0);
                t.text_secondary  = QColor(0x9b, 0xa5, 0xa2);
                t.text_disabled   = QColor(0x5d, 0x66, 0x63);
                t.accent          = QColor(0x34, 0xd3, 0x99);
                t.accent_hover    = QColor(0x5f, 0xe0, 0xae);
                t.accent_pressed  = QColor(0x1f, 0xb0, 0x7e);
                t.accent_text     = QColor(0x05, 0x20, 0x17);
                t.divider         = QColor(0x26, 0x2d, 0x31);
                t.input_bg        = QColor(0x1d, 0x23, 0x26);
                t.card_border     = t.divider;
                t.control_border  = QColor(0x33, 0x3c, 0x40);
                t.card_border_hover = QColor(0x3b, 0x44, 0x49);
                t.surface_deep    = QColor(0x22, 0x2a, 0x2d);
                t.hover_bg        = t.input_bg;
                t.primary_disabled_bg = t.input_bg;
                break;
        }
    } else {
        t.hover_overlay  = QColor(0, 0, 0, 10); // rgba(0,0,0,0.04)
        t.danger         = QColor(0xc4, 0x2b, 0x1c);
        t.danger_pressed = QColor(0xb0, 0x2c, 0x1c);
        switch (family) {
            case ThemeFamily::Warm:
                t.window          = QColor(0xf7, 0xf5, 0xf2);
                t.card            = QColor(0xff, 0xff, 0xff);
                t.text            = QColor(0x26, 0x22, 0x20);
                t.text_secondary  = QColor(0x52, 0x4d, 0x47);
                t.text_disabled   = QColor(0xaa, 0xa3, 0x9c);
                t.accent          = QColor(0xc2, 0x41, 0x0c);
                t.accent_hover    = QColor(0x9a, 0x34, 0x12);
                t.accent_pressed  = QColor(0x7c, 0x2d, 0x12);
                t.accent_text     = QColor(0xff, 0xff, 0xff);
                t.divider         = QColor(0xe4, 0xe1, 0xdc);
                t.input_bg        = QColor(0xea, 0xe6, 0xdf);
                t.card_border     = t.divider;
                t.control_border  = QColor(0xd8, 0xd3, 0xcb);
                t.card_border_hover = QColor(0xc5, 0xbd, 0xb2);
                t.surface_deep    = t.input_bg;
                t.hover_bg        = QColor(0xf4, 0xf1, 0xec);
                t.primary_disabled_bg = QColor(0xe7, 0xe1, 0xd9);
                break;
            case ThemeFamily::Cold:
                t.window          = QColor(0xf4, 0xf6, 0xf8);
                t.card            = QColor(0xff, 0xff, 0xff);
                t.text            = QColor(0x1e, 0x25, 0x2c);
                t.text_secondary  = QColor(0x5c, 0x66, 0x72);
                t.text_disabled   = QColor(0xa2, 0xab, 0xb5);
                t.accent          = QColor(0x2f, 0x6f, 0xd0);
                t.accent_hover    = QColor(0x27, 0x5c, 0xb3);
                t.accent_pressed  = QColor(0x1f, 0x4c, 0x96);
                t.accent_text     = QColor(0xff, 0xff, 0xff);
                t.divider         = QColor(0xde, 0xe3, 0xe8);
                t.input_bg        = QColor(0xe3, 0xe8, 0xed);
                t.card_border     = t.divider;
                t.control_border  = QColor(0xd2, 0xd9, 0xe0);
                t.card_border_hover = QColor(0xc5, 0xcc, 0xd4);
                t.surface_deep    = t.input_bg;
                t.hover_bg        = QColor(0xee, 0xf1, 0xf4);
                t.primary_disabled_bg = QColor(0xe1, 0xe8, 0xf0);
                break;
            case ThemeFamily::Graphite:
                t.window          = QColor(0xf4, 0xf7, 0xf5);
                t.card            = QColor(0xff, 0xff, 0xff);
                t.text            = QColor(0x1b, 0x23, 0x1f);
                t.text_secondary  = QColor(0x55, 0x60, 0x5b);
                t.text_disabled   = QColor(0xa1, 0xaa, 0xa6);
                t.accent          = QColor(0x0b, 0x7d, 0x59);
                t.accent_hover    = QColor(0x09, 0x66, 0x49);
                t.accent_pressed  = QColor(0x06, 0x4f, 0x39);
                t.accent_text     = QColor(0xff, 0xff, 0xff);
                t.divider         = QColor(0xe0, 0xe6, 0xe3);
                t.input_bg        = QColor(0xe2, 0xe9, 0xe5);
                t.card_border     = t.divider;
                t.control_border  = QColor(0xd3, 0xdd, 0xd8);
                t.card_border_hover = QColor(0xc2, 0xcd, 0xc7);
                t.surface_deep    = t.input_bg;
                t.hover_bg        = QColor(0xee, 0xf2, 0xf0);
                t.primary_disabled_bg = QColor(0xdc, 0xeb, 0xe3);
                break;
        }
    }
    // checkbox 图标走家族×主题 12 枚 qrc 预生成 SVG 变体
    // (resources/icons/checkbox-*-<family>-<theme>.svg):几何同形,
    // 着色取本家族色板(checked 底=accent/勾=accent_text,unchecked
    // 底=card/描边环=亮暗各自的中性环色),由 render_qss 经
    // {{checkbox_*}} 占位符写入 QSS 模板的 url() 引用。
    const char* family_key = family == ThemeFamily::Warm   ? "warm"
                             : family == ThemeFamily::Cold ? "cold"
                                                           : "graphite";
    const char* theme_key  = theme == ThemeType::Light ? "light" : "dark";
    t.checkbox_checked     = QStringLiteral(":/icons/checkbox-checked-%1-%2.svg")
                                 .arg(QLatin1String(family_key),
                                      QLatin1String(theme_key));
    t.checkbox_unchecked   = QStringLiteral(":/icons/checkbox-unchecked-%1-%2.svg")
                                 .arg(QLatin1String(family_key),
                                      QLatin1String(theme_key));
    return t;
}

} // namespace detail

/**
 * @brief 按家族+主题返回 token 集(与 resources/styles/fluent_*.qss 模板对应)
 */
inline ThemeTokens tokens_for(ThemeFamily family, ThemeType theme)
{
    return detail::make_tokens(family, theme);
}

/**
 * @brief 默认家族(Warm)便捷重载——数值即 warm 家族变体表列(生产默认)
 */
inline ThemeTokens tokens_for(ThemeType theme)
{
    return tokens_for(ThemeFamily::Warm, theme);
}

} // namespace falcon::desktop
