**中文** | [English](README.md)

# 方向 C · graphite emerald

自由方向，不延续任何既有家族：近黑石墨中性底 + 祖母绿强调。既不是 Motrix 衍生也不是 Fluent 克隆——
色板自成一体。

原型渲染：**28 张图**（14 个功能面 × 亮/暗双主题），全部由 ui_sandbox 离屏管线产出（真实 Qt 组件树 +
演示任务数据，零引擎/网络副作用）。

## Token 表（生产暖色 → 变体）

**暗色**（`kGraphiteDark`）：

| 角色 | 生产 | 变体 |
|---|---|---|
| window | `#1b1714` | `#0f1214` |
| card | `#262019` | `#161b1e` |
| divider | `#37302a` | `#262d31` |
| text | `#f2ede8` | `#eef2f0` |
| text_secondary | `#b5aca3` | `#9ba5a2` |
| text_disabled | `#6e665e` | `#5d6663` |
| 控件底（按钮/输入） | `#312a24` | `#1d2326` |
| 控件描边 | `#4a423a` | `#333c40` |
| 分段钮 checked 底 | `#3a322b` | `#222a2d` |
| 卡片 hover 描边 | `#554b41` | `#3b4449` |
| accent | `#ffa07a` | `#34d399` |
| accent hover | `#ffb28c` | `#5fe0ae` |
| accent pressed | `#e0875e` | `#1fb07e` |
| accent 上文字 | `#27140a` | `#052017` |

**亮色**（`kGraphiteLight`）——冷灰绿白面；`#ffffff` 双角色（card + accent_text）不动：

| 角色 | 生产 | 变体 |
|---|---|---|
| window | `#f6f4f1` | `#f4f7f5` |
| text | `#292524` | `#1b231f` |
| text_secondary | `#57534e` | `#55605b` |
| text_disabled | `#a8a29e` | `#a1aaa6` |
| checkbox 描边环 | `#a29a92` | `#a3aca8` |
| accent | `#c2410c` | `#0b7d59` |
| accent hover | `#9a3412` | `#096649` |
| accent pressed | `#7c2d12` | `#064f39` |
| divider | `#e7e5e4` | `#e0e6e3` |
| 边框/分隔强 | `#dcd7d1` | `#d3ddd8` |
| hover 底 | `#f5f2ee` | `#eef2f0` |
| 控件底 | `#edeae5` | `#e2e9e5` |
| primaryButton disabled 底 | `#e8e3dd` | `#dcebe3` |
| 卡片 hover 描边 | `#cac2b9` | `#c2cdc7` |

## 布局 / 组件差异

**无。** 纯 token 换肤：组件树、布局参数、间距、交互与生产完全一致，只有两份 QSS 内的 hex 值（连同重建
的 QPalette）不同。checkbox SVG 按变体色重新生成，托盘菜单（第 14 个功能面）自动继承方向色板。

## 取舍

- **Pro**：唯一真正建立新桌面身份的方向——近黑底 + 饱和祖母绿强调，读作「产品」而非「媒体播放器」。
  三者中最深的窗底（`#0f1214`）+ 最高的 accent 于窗底对比度（**9.78:1**）。
- **Con**：与 falcon 品牌的风格断裂最大。logo 保持鲑橙（品牌锚点，烧在 SVG 里）而全部强调变祖母绿，
  两种颜色必须在顶栏共存——这是最可能因品牌原因被否掉的方向，也是最能改变产品气质的方向。
- 近黑底上的祖母绿是终端/开发者工具的成熟组合，因此有读作「泛用 dev tool」而非独立产品身份的风险。

## 对比度（WCAG AA，Python 实算）

暗色：accent 于窗底 **9.78**；全部 24 对检查 ≥ 4.5:1。亮色 accent `#0b7d59` 于 `#ffffff` 达 AA。

## 已知边界（如实记录）

- 顶栏 falcon logo 的鲑橙 `#ffa07a` 烧在品牌 SVG 资产里、不经 token 体系——每个方向的 logo 都保持品牌
  色。有意保留的品牌锚点，不是漏改。
- 图标着色经 `icon_utils` 的 `tokens_for()` 取生产值；Text 角色图标色在 16px 下差异不可辨，Accent 角色
  图标（云盘连接钮）不在主视图。修复属生产代码改动，超出「只做原型」任务范围。
- `rgba(...)` 叠加 token（hover/pressed 色调）不映射，沿用生产值。
- 亮色 checkbox 环映射 `#a29a92 → #a3aca8`；生产环色细节（`#a29a92` 与 token `#a8a29e` 是两个值）由
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
