**中文** | [English](README.md)

# 方向 A · warm console II

现行生产「warm console」方向的迭代延续：同一暖黑中性底 + falcon 鲑橙单强调，加深并精修。这是三个提案中
**改动最小**的一个——留在与文档站共享的品牌家族内，只收紧色板。

原型渲染：**28 张图**（14 个功能面 × 亮/暗双主题），全部由 ui_sandbox 离屏管线产出（真实 Qt 组件树 +
演示任务数据，零引擎/网络副作用）。

## Token 表（生产暖色 → 变体）

**暗色**（`kWarmIiDark`）：

| 角色 | 生产 | 变体 |
|---|---|---|
| window | `#1b1714` | `#16120f` |
| card | `#262019` | `#221c16` |
| divider | `#37302a` | `#3a322a` |
| text | `#f2ede8` | `#f7f2ec` |
| text_secondary | `#b5aca3` | `#bcb2a6` |
| text_disabled | `#6e665e` | `#756c62` |
| 控件底（按钮/输入） | `#312a24` | `#2d2620` |
| 控件描边 | `#4a423a` | `#4e453c` |
| 分段钮 checked 底 | `#3a322b` | `#362e26` |
| 卡片 hover 描边 | `#554b41` | `#5a4f44` |

**亮色**（`kWarmIiLight`）——accent 家族不变（`#c2410c` / `#9a3412` / `#7c2d12`），`#ffffff` 双角色
（card + accent_text）不动：

| 角色 | 生产 | 变体 |
|---|---|---|
| window | `#f6f4f1` | `#f7f5f2` |
| text | `#292524` | `#262220` |
| text_secondary | `#57534e` | `#524d47` |
| text_disabled | `#a8a29e` | `#aaa39c` |
| checkbox 描边环 | `#a29a92` | `#aba49b` |
| divider | `#e7e5e4` | `#e4e1dc` |
| 边框/分隔强 | `#dcd7d1` | `#d8d3cb` |
| hover 底 | `#f5f2ee` | `#f4f1ec` |
| 控件底 | `#edeae5` | `#eae6df` |
| primaryButton disabled 底 | `#e8e3dd` | `#e7e1d9` |
| 卡片 hover 描边 | `#cac2b9` | `#c5bdb2` |

## 布局 / 组件差异

**无。** 纯 token 换肤：组件树、布局参数、间距、交互与生产完全一致，只有两份 QSS 内的 hex 值（连同重建
的 QPalette）不同。checkbox SVG 按变体色重新生成（几何同生产 16×16 `rx=3`），托盘菜单（第 14 个功能面，
本矩阵新增）自动继承方向色板。

## 取舍

- **Pro**：最安全的提案——与今日线上形态近乎一致，评审风险最小、增量易于审计；与文档站保持完整的品牌
  连续性。
- **Con**：变革性最低；若目标是让桌面端呈现可感知的不同身份，这个方向单位投入产出的变化量最少。
- 暗色窗底加深到 `#16120f`（更贴近纯黑一档），文字提亮到 `#f7f2ec`，文字于窗底的对比度抬升到
  **16.73:1**（自 15.3:1）。

## 对比度（WCAG AA，Python 实算）

暗色：text 16.73 / accent 9.37 / accent-text 8.87 / secondary 8.07。亮色：text 14.49 / accent 4.76 /
白字于 accent 5.18 / secondary 8.36。全部 24 对检查 ≥ 4.5:1。

## 已知边界（如实记录）

- 顶栏 falcon logo（`:/icons/falcon.svg`）的鲑橙 `#ffa07a` 烧在品牌 SVG 资产里、不经 token 体系——每个
  方向的 logo 都保持品牌色。有意保留的品牌锚点，不是漏改。
- 图标着色经 `icon_utils` 的 `tokens_for()` 取生产值；Text 角色图标色在 16px 下差异不可辨，Accent 角色
  图标（云盘连接钮）不在主视图。修复属生产代码改动，超出「只做原型」任务范围。
- `rgba(...)` 叠加 token（hover/pressed 色调）不映射，沿用生产值。
- 亮色 checkbox 环映射 `#a29a92 → #aba49b`；生产环色细节（`#a29a92` 与 token `#a8a29e` 是两个值）由
  专门映射行保留。
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
