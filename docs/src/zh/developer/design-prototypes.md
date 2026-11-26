# 界面设计原型

[English](/developer/design-prototypes) | **简体中文**

当前桌面端设计是 `docs/design/ui-next/`：一个纯静态、无依赖的可交互原型。
下面两图从该原型导出，均为默认进入的「下载中 · 列表视图」，分别使用暖橙与
冷蓝主题；布局、组件、交互完全一致，仅 accent token 不同。

![暖橙主题（默认）](/design/ui-next/screens/warm-dark/023-download-table.png)
![冷蓝主题（可切换）](/design/ui-next/screens/cold-dark/067-download-table.png)

<div class="prototype-caption">左：暖橙（默认）｜右：冷蓝（可切换主题）</div>

## 主题 token

| | 暖橙（默认） | 冷蓝 |
|---|---|---|
| 方向 | 暖色中性底 + 鲑橙强调，延续现行品牌 | 冷峻工具感底 + 克制蓝色强调 |
| window | `#16120f` | `#131519` |
| card | `#221c16` | `#1b1e24` |
| text | `#f7f2ec` | `#f5f7fa` |
| accent | `#ffa07a`（accent 上文字 `#27140a`） | `#5b9df5`（accent 上文字 `#0d1a2b`） |

三主题 × 亮暗两模式均按 WCAG AA 校验过对比度。顶栏 falcon logo 的鲑橙
`#ffa07a` 是刻意保留的品牌锚点，不随主题变化。

## 原型覆盖范围

原型共 24 屏：现有下载 / 资源发现 / 云盘 / 回收站 / 偏好设置，以及规划中的
跨设备、账号与分享、NAS 与 Windows 托盘。关键结论：

- **跨设备 ↔ 账号与分享**：设备属「物」，账号属「人」，分享组连接两者。
- **隔空传送**与跨设备共用同一套 syncbond 6 位匹配码；扫码只是同一串码的
  更快输入方式。
- **LocalSend** 可作为局域网发现通道兼容；AirDrop 只走系统分享入口（AWDL
  为私有协议）；DHT 是下载结果的 peer 发现层，不是资源站。
- **Windows 托盘**为常驻图标 + 右键菜单 + 悬浮速度窗（默认迷你胶囊、点击展开详细卡片）；macOS 是另一套菜单栏实现。
- **任务右键菜单**沿用迅雷 10 项形态（暂停 / 文件夹 / 收藏 / 详情 / 云盘 / 删除 / 彻底删除 / 移动 / 复制链接 / 取消全选），Falcon 化去掉会员文案，并补「原始节点数 / 剩余时间」；「文件详情」「添加到云盘」可跳转。

完整设计说明见仓库中的 `docs/design/ui-next/README.zh.md`。

## 设计图导出

设计图位于 `src/public/design/ui-next/screens/<主题>-<模式>/`。导出模式由 URL
参数控制，例如：

```text
index.html?clean=1&theme=warm&mode=dark&screen=download-table
```

`clean=1` 会隐藏评审工具条并关闭入场动画，保证截图是稳定帧。

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
