# 界面设计原型

桌面端主视图的两版设计候选——先出原型过目，再动生产代码。两图均为
ui_sandbox 离屏渲染（真实 Qt 组件树 + demo 任务数据：2 下载中 / 1 暂停 /
1 完成 / 1 失败 / 2 做种），「下载中·表格」视图（应用落地第一屏），顶栏 /
侧栏 / hero 摘要卡 / 七列任务表 / 状态栏全要素，暗色主题 1200×800。

![变体 A · warm console（现行生产主题）](../../design/prototypes/warm-console-dark.png)
![变体 B · cold utility（变体候选）](../../design/prototypes/cold-utility-dark.png)

<div class="prototype-caption">左：变体 A（warm console，现行生产主题）｜右：变体 B（cold utility，变体候选）</div>

## 两版对比

| | 变体 A · warm console | 变体 B · cold utility |
|---|---|---|
| 方向 | 现行生产主题现状：暖黑中性底 + falcon 鲑橙单强调，与本文档站同源 | 冷峻工具感变体（对标 Motrix）：更深的中性底、高对比文字、更克制的蓝色 accent |
| window | `#1b1714` | `#131519` |
| card | `#262019` | `#1b1e24` |
| text | `#f2ede8` | `#f5f7fa` |
| accent | `#ffa07a`（accent 上文字 `#27140a`） | `#5b9df5`（accent 上文字 `#0d1a2b`） |

对比度（WCAG AA，Python 实算）：

- 变体 A：text 于 window 15.3:1，accent 于 window 9.0:1，accent 上文字 9.1:1。
- 变体 B：text 于 window 17.0:1（「高对比文字」目标达成），accent 于
  window 6.6:1，accent 上文字 6.3:1，secondary 6.9:1。

## 已知边界

- 顶栏 falcon logo 的鲑橙 `#ffa07a` 烧在品牌 SVG 资产里、不走 token 系统——
  冷版里 logo 保留品牌色（152 px），属有意保留的品牌锚点，非替换遗漏。
- 图标着色经 `icon_utils` 的 `tokens_for()` 取生产值；Text 角色图标暖白 vs
  冷白在 16px 下不可辨，Accent 角色图标仅云盘连接钮一处（主视图不出现）——
  对比不受影响。
- 变体 B 的 accent 色相选了克制的蓝（Motrix 方向）；若想「冷中性底 +
  falcon 橙」组合，改 `kColdHexMap` 的 accent 三行即可重出图。

## 原型资产与再生成

源文件入库于 `docs/design/prototypes/`（两图 + README，含变体 B 的实现
边界与 token 映射全集）。重新生成：

```bash
cmake --build build-desktop --target falcon-ui-sandbox
build-desktop/bin/falcon-ui-sandbox --prototype docs/design/prototypes
```

（需要 `FALCON_BUILD_UI_SANDBOX=ON` 的 build-desktop 树；离屏渲染，
无需显示环境。）

另见 [桌面端重设计](/developer/desktop-redesign) 的整体重设计 brief。

<style scoped>
img {
  display: inline-block;
  width: 49.4%;
  vertical-align: top;
  border-radius: 8px;
  border: 1px solid var(--vp-c-divider);
}

.prototype-caption {
  text-align: center;
  color: var(--vp-c-text-2);
  font-size: 14px;
  margin-top: 4px;
}

@media (max-width: 768px) {
  img {
    width: 100%;
  }
}
</style>
