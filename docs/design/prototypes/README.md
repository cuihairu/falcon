# 主视图设计原型（暗色 1200×800）

桌面端界面再设计的两版候选——先出原型过目，再动生产代码。两图均为
ui_sandbox 离屏渲染（真实组件 + demo 任务数据：2 下载中 / 1 暂停 / 1 完成 /
1 失败 / 2 做种），「下载中·表格」视图（应用落地第一屏），顶栏/侧栏/
hero 摘要卡/七列任务表/状态栏全要素。

| 文件 | 方向 | 关键 token |
|---|---|---|
| `warm-console-dark.png` | A：现生产 warm console 主题现状（暖黑中性底 + falcon 鲑橙单强调，与文档站同源） | window `#1b1714` / card `#262019` / text `#f2ede8` / accent `#ffa07a`（accent 上文字 `#27140a`） |
| `cold-utility-dark.png` | B：冷峻工具感变体（对标 Motrix：更深的中性底、高对比文字、更克制的蓝色 accent） | window `#131519` / card `#1b1e24` / text `#f5f7fa` / accent `#5b9df5`（accent 上文字 `#0d1a2b`） |

## 变体 B 的实现边界（重要）

变体 B 的 token 替换**只发生在 sandbox 会话内**——对暗色 QSS 文本做
hex 热→冷映射 + 整串替换 checkbox 内嵌 base64 SVG + 按冷 token 重建
QPalette，生产 `theme_tokens.hpp` / `fluent_dark.qss` **零改动**。
落地时只需把 `ui_sandbox.cpp` 中 `kColdHexMap` 的映射值写入生产
token/QSS（若选 B）。

对比度（WCAG AA，Python 实算）：cold accent 于 window 6.6:1、accent
上文字 6.3:1、text 17.0:1（warm 为 15.3:1，"高对比文字"目标达成）、
secondary 6.9:1。

已知边界（如实记录）：

- 顶栏 falcon logo（`:/icons/falcon.svg`）的鲑橙 `#ffa07a` 烧在品牌
  SVG 资产里、不走 token 系统——冷版里 logo 保留品牌色（152 px），
  属有意保留的品牌锚点，非替换遗漏。
- 图标着色经 `icon_utils` 的 `tokens_for()` 取生产值；Text 角色图标
  暖白 vs 冷白在 16px 下不可辨，Accent 角色图标仅云盘连接钮一处
  （主视图不出现）——对比不受影响。
- accent 色相选了克制的蓝（Motrix 方向）；若想「冷中性底 + falcon
  橙」组合，改 `kColdHexMap` 的 accent 三行即可重出图。

## 重新生成

```bash
cmake --build build-desktop --target falcon-ui-sandbox
build-desktop/bin/falcon-ui-sandbox --prototype docs/design/prototypes
```

（需要 FALCON_BUILD_UI_SANDBOX=ON 的 build-desktop 树；离屏渲染，
无需显示环境。）
