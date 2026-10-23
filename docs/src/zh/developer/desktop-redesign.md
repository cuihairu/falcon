# Desktop Redesign Brief

本文定义 Falcon Desktop 的界面重设计方向。目标是在进入 Qt 代码实现前，先建立可评审的视觉标准，避免只做低质量换色。

## 设计目标

Falcon Desktop 应该像一个高性能下载控制台，而不是通用后台管理系统。界面需要突出任务状态、速度、队列和资源入口，让用户在打开应用后的 3 秒内理解当前下载系统是否健康。

核心目标：

- 强化产品识别度：形成 Falcon 自己的视觉语言，而不是默认 Qt 控件堆叠。
- 提升任务可读性：下载状态、速度、进度、操作入口必须比装饰元素更醒目。
- 降低认知负担：保留当前功能结构，但重新组织信息层级。
- 控制实现风险：优先改样式、布局和组件外观，不改下载引擎、存储、搜索和 IPC 逻辑。

## 视觉方向

推荐采用 `Deep Signal` 方向：深色控制台基底、青绿色信号光、暖橙关键操作、玻璃感任务面板。

这个方向适合下载工具的原因：

- 深色基底能承载长时间运行场景，减少视觉疲劳。
- 青绿色和蓝色可表达网络、速度、连接、稳定。
- 暖橙只用于“新建下载”“立即开始”等高优先级动作，避免全界面高饱和。
- 卡片和表格使用低对比边框与层级阴影，让任务列表像仪表板而不是表单。

备选方向：

- `Graphite Pro`：浅色石墨灰专业工具风，适合偏办公和跨平台一致性。
- `Kinetic Glass`：浅深混合玻璃拟态，视觉更强，但 Qt QSS 实现复杂度更高。
- `Neo Classic`：macOS 风格侧栏与内容卡片，稳妥但辨识度不足。

## 信息架构

### 顶部栏

顶部栏承担全局任务入口，而不是普通标题栏。

建议结构：

- 左侧：Falcon 标识、版本/运行状态小标签。
- 中间：链接输入与资源搜索合一的 command bar。
- 右侧：刷新、视图、更多、窗口控制。

文案建议：

- 搜索占位：`粘贴链接、磁力、电驴或搜索资源`
- 状态标签：`Signal Ready` 或 `Engine Online`
- 主操作按钮：`新建下载`

### 侧边栏

侧边栏应从“功能菜单”升级为“任务空间”。

建议分组：

- `传输队列`：下载中、已完成、云添加。
- `资源网络`：资源发现、云盘空间。
- `系统`：偏好设置。

删除或后置目前弱价值的“私人空间”入口，除非后续有真实隐私下载功能。

### 下载页

下载页是主体验，应重做为三层结构：

- Hero 状态面板：当前模式、总速度、活跃任务、主操作。
- Summary cards：活跃任务、已完成、当前速度、失败/等待。
- Task surface：表格/卡片两种视图，但视觉风格统一。

任务列表需要突出：

- 文件名和来源。
- 进度条。
- 实时速度。
- 状态 badge。
- 暂停、继续、删除等行内操作。

### 状态栏

状态栏应像系统 telemetry，不应像按钮堆。

建议内容：

- 左侧：计划、远程、插件。
- 中间：实时速度、任务数量、连接状态。
- 右侧：下载检测与通知 badge。

## 设计 Token

### 颜色

`Deep Signal` 建议 token：

| Token | 用途 | 值 |
| --- | --- | --- |
| `bg.app` | 应用底色 | `#08111F` |
| `bg.sidebar` | 侧边栏 | `#0B1726` |
| `bg.surface` | 内容面板 | `#101D2E` |
| `bg.surfaceElevated` | 悬浮卡片 | `#15263A` |
| `border.subtle` | 弱边框 | `#22364D` |
| `text.primary` | 主文本 | `#EAF2FF` |
| `text.secondary` | 次级文本 | `#8EA3BA` |
| `brand.signal` | 品牌信号色 | `#36E3B4` |
| `brand.action` | 主操作色 | `#FFB454` |
| `state.error` | 错误 | `#FF5C7A` |
| `state.warning` | 警告 | `#FFD166` |
| `state.success` | 成功 | `#35D07F` |

### 字体

Qt 跨平台实现优先使用系统可用字体链：

```text
"Segoe UI Variable", "SF Pro Display", "Microsoft YaHei UI", "Noto Sans CJK SC", sans-serif
```

字号建议：

| 层级 | 字号 | 字重 |
| --- | --- | --- |
| Hero title | 24px | 700 |
| Section title | 16px | 700 |
| Body | 13px | 500 |
| Metadata | 12px | 500 |
| Caption | 11px | 600 |

### 间距与圆角

使用 4px 基础网格：

- 页面边距：24px。
- 卡片内边距：16px 或 20px。
- 控件间距：8px、12px、16px。
- 普通圆角：12px。
- 卡片圆角：18px。
- Command bar 圆角：20px。

## Qt 落地策略

### 第一阶段：低风险视觉换代

只修改 UI 层：

- `apps/desktop/src/utils/theme_manager.cpp`
- `apps/desktop/src/widgets/top_bar.cpp`
- `apps/desktop/src/navigation/sidebar.cpp`
- `apps/desktop/src/widgets/status_bar.cpp`
- 必要时小幅调整 `download_page.cpp` 的对象名、间距和文案。

不修改：

- `DownloadEngine`
- `TaskManager`
- 协议处理器
- 云盘和资源搜索业务逻辑
- IPC 和系统托盘逻辑

### 第二阶段：组件化整理

如果第一阶段方向通过，再抽象：

- `MetricCard`
- `CommandBar`
- `StatusBadge`
- `TaskActionButton`
- `NavItem`

这些组件应保持单一职责，避免把业务状态和样式揉在一起。

### 第三阶段：动效与空状态

Qt Widgets 动效需要克制：

- 新任务出现时做轻微 opacity/position transition。
- 进度条使用渐变 chunk。
- 空状态增加图形化信号波纹，但不要引入复杂动画系统。

## 验收标准

实现阶段必须满足：

- 主窗口、顶部栏、侧边栏、下载页、设置页、资源发现页风格一致。
- 表格和卡片视图在深色主题下可读，不依赖纯黑/纯白强对比。
- 主操作按钮全局唯一醒目，次要按钮不抢视觉焦点。
- 任务状态和进度比装饰更醒目。
- Linux/macOS/Windows CI 至少能完成 desktop build。
- 如果本地缺少 Qt，必须通过 GitHub Actions 完整验证后才能声称完成。

## 当前约束

当前会话没有可用的 Figma MCP 工具，无法直接生成 Figma 画板。已安装 Figma 相关 skills 后，若后续接入 Figma MCP，应按以下流程执行：

1. 使用 `figma-use` 检查目标 Figma 文件。
2. 使用 `figma-generate-design` 基于本 brief 生成主窗口画板。
3. 先验证截图，再进入代码实现。
4. 使用 `figma-implement-design` 将确认后的设计转换为 Qt 实现约束。

在没有 Figma MCP 的情况下，本 brief 作为实现依据。
