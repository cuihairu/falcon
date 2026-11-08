**中文** | [English](README.md)

# 方向 B · cold utility II

2026-09-29「cold utility」原型（对标 Motrix）的延续：更深的冷色中性底、更高对比度的文字、克制的蓝色
强调。暗色色板**逐字沿用**早期原型的 `kColdHexMap`（窗底 `#131519`，强调 `#5b9df5`）；亮色色板是同家族
新增的冷色亮面。

原型渲染：**28 张图**（14 个功能面 × 亮/暗双主题），全部由 ui_sandbox 离屏管线产出（真实 Qt 组件树 +
演示任务数据，零引擎/网络副作用）。

## Token 表（生产暖色 → 变体）

**暗色**（`kColdIiDark`）——与 2026-09-29 原型的 `kColdHexMap` 一致，含 accent 家族：

| 角色 | 生产 | 变体 |
|---|---|---|
| window | `#1b1714` | `#131519` |
| card | `#262019` | `#1b1e24` |
| divider | `#37302a` | `#2b3038` |
| text | `#f2ede8` | `#f5f7fa` |
| text_secondary | `#b5aca3` | `#98a0ab` |
| text_disabled | `#6e665e` | `#5a6169` |
| 控件底（按钮/输入） | `#312a24` | `#22262d` |
| 控件描边 | `#4a423a` | `#363c45` |
| 分段钮 checked 底 | `#3a322b` | `#262b33` |
| 卡片 hover 描边 | `#554b41` | `#3d444e` |
| accent | `#ffa07a` | `#5b9df5` |
| accent hover | `#ffb28c` | `#7ab1f8` |
| accent pressed | `#e0875e` | `#4585e0` |
| accent 上文字 | `#27140a` | `#0d1a2b` |

**亮色**（`kColdIiLight`）——全新冷色亮面；`#ffffff` 双角色（card + accent_text）不动：

| 角色 | 生产 | 变体 |
|---|---|---|
| window | `#f6f4f1` | `#f4f6f8` |
| text | `#292524` | `#1e252c` |
| text_secondary | `#57534e` | `#5c6672` |
| text_disabled | `#a8a29e` | `#a2abb5` |
| checkbox 描边环 | `#a29a92` | `#a3acb6` |
| accent | `#c2410c` | `#2f6fd0` |
| accent hover | `#9a3412` | `#275cb3` |
| accent pressed | `#7c2d12` | `#1f4c96` |
| divider | `#e7e5e4` | `#dee3e8` |
| 边框/分隔强 | `#dcd7d1` | `#d2d9e0` |
| hover 底 | `#f5f2ee` | `#eef1f4` |
| 控件底 | `#edeae5` | `#e3e8ed` |
| primaryButton disabled 底 | `#e8e3dd` | `#e1e8f0` |
| 卡片 hover 描边 | `#cac2b9` | `#c5ccd4` |

## 布局 / 组件差异

**无。** 纯 token 换肤：组件树、布局参数、间距、交互与生产完全一致，只有两份 QSS 内的 hex 值（连同重建
的 QPalette）不同。checkbox SVG 按变体色重新生成，托盘菜单（第 14 个功能面）自动继承方向色板。

## 取舍

- **Pro**：三者中文字对比度最高（暗色 **17.03:1**）、强调最克制（沉静的蓝读作「工具」而非「媒体」）、
  中性底最深——最接近「power tool」的气质。
- **Con**：与 falcon 品牌橙距离最远；logo 保持鲑橙（品牌锚点）而其余全部转蓝，是三者中风格张力最大
  的一个。
- 亮色面是全新的（2026-09-29 原型只出了暗色）——现在亮暗两主题在冷色家族内自洽。

## 对比度（WCAG AA，Python 实算）

暗色：text 17.03 / accent 6.60 / accent-text 6.32 / secondary 6.32。亮色：text 14.29 / accent 4.50 /
白字于 accent 4.88 / secondary 5.84。全部 24 对检查 ≥ 4.5:1。

## 已知边界（如实记录）

- 顶栏 falcon logo 的鲑橙 `#ffa07a` 烧在品牌 SVG 资产里、不经 token 体系——每个方向的 logo 都保持品牌
  色。有意保留的品牌锚点。
- 图标着色经 `icon_utils` 的 `tokens_for()` 取生产值；Text 角色图标色在 16px 下差异不可辨，Accent 角色
  图标（云盘连接钮）不在主视图。修复属生产代码改动，超出「只做原型」任务范围。
- `rgba(...)` 叠加 token（hover/pressed 色调）不映射，沿用生产值。
- 亮色 checkbox 环映射 `#a29a92 → #a3acb6`；生产环色细节由专门映射行保留。
- `#ffffff` 在亮色下是双角色（card + accent_text）；安全，因为每个亮色方向的 accent_text 都保持
  `#ffffff`。

## 功能面命名说明

`download_table_*` 截图实际显示的是**网格卡片**视图，`download_grid_*` 显示的是**表格**——因为
`DownloadPage` 生产默认是 `TaskDisplayStyle::Grid`（`download_page.cpp:82`），沙盒在
`toggle_display_style()` 前后各截一张。该命名惯例沿用已入库的 `docs/design/ui-sandbox` 矩阵，保持一致
而不重新渲染。

## 重新生成

```bash
cmake --build build-ui-audit --target falcon-ui-sandbox
build-ui-audit/bin/falcon-ui-sandbox --redesign docs/design/redesign-2026-11
```

（需要以 `FALCON_BUILD_UI_SANDBOX=ON` 配置的构建树 + 系统 Qt6；离屏渲染，无需显示环境。`--redesign`
模式新增于 `apps/desktop/dev/ui_sandbox.cpp`，只触碰沙盒会话——生产 `theme_tokens.hpp` /
`fluent_*.qss` 零改动。）
