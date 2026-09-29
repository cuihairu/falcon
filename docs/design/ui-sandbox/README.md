# ui_sandbox 正式截图矩阵（52 张）

ui_sandbox 离屏渲染的正式设计验收资产：13 视图 × 亮/暗两主题 ×
1200/960 双尺寸。组件、布局参数、样式表与生产完全一致（不构造
DownloadService，引擎/网络零副作用）。

## 矩阵构成

- `download_table_*` / `download_grid_*`：下载中视图（表格/网格）
- `download_completed_*` / `download_completed_grid_*`：已完成视图
  （做种任务的做种列/做种卡片摘要在位）
- `cloud_*`：云盘空间；`discovery_*`：资源发现；`trash_*`：回收站
- `settings_*`：设置页整窗 + 四组特写（`_seeding`/`_trash`/`_search`/
  `_about`——滚动区折叠线以下，整窗截图看不到）
- `add_dialog_*`：添加下载对话框（主窗之外最高频交互面）

尺寸后缀 `_1200` = 生产默认 1200×800，`_960` = 最小窗 960×640
（挤压场景——重叠类布局缺陷只在窄窗下暴露）。

## 重新生成

```bash
cmake --build build-desktop --target falcon-ui-sandbox
build-desktop/bin/falcon-ui-sandbox docs/design/ui-sandbox
```

截图脚本与 demo 数据见 `apps/desktop/dev/ui_sandbox.cpp`。
