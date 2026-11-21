# UI-RD-002 C 证据：三家族 × 亮暗主题截图

本目录为 todo `UI-RD-002`（桌面 UI 精修 + 三主题落地）子任务 C 的验收证据：
真实 `falcon-desktop` 二进制在三家族（Warm Console II / Cold Utility II /
Graphite Emerald）× 亮暗两模式下启动的窗口截图，共 6 张。

> **视觉类条目待用户亲验**：本目录只提供截图与像素自检记录，不做任何
> 「视觉通过」结论；`todo.md` 中 C 项的勾选由用户完成。

## 截图清单

| 文件 | 家族 | 模式 |
|------|------|------|
| `theme_warm_light.png` | Warm Console II | 亮色 |
| `theme_warm_dark.png` | Warm Console II | 深色 |
| `theme_cold_light.png` | Cold Utility II | 亮色 |
| `theme_cold_dark.png` | Cold Utility II | 深色 |
| `theme_graphite_light.png` | Graphite Emerald | 亮色 |
| `theme_graphite_dark.png` | Graphite Emerald | 深色 |

全部 1200×800（主窗口 `resize(1200, 800)`，见 `main_window.cpp`）。

## 方法

### 环境

- Xvfb `:95`，屏幕 1400×900×24（无窗口管理器）
- 真实二进制 `build-desktop/bin/falcon-desktop`（重建于 B5 修复之后）
- `xdotool` 定位窗口 + ImageMagick `import` 截图

### 启动态注入

主题由 QSettings 读取（`main_window.cpp` 构造器）：org `FalconTeam`，
app `Falcon`，`desktop` 组键 `theme_family`（`warm|cold|graphite`）与
`theme_mode`（`light|dark`）。为忠实还原每种组合的启动态，每次启动前用
隔离的临时 `XDG_CONFIG_HOME` 预置 `FalconTeam/Falcon.conf`：

```ini
[desktop]
theme_family=<warm|cold|graphite>
theme_mode=<light|dark>
```

### 截图脚本要点

```bash
CONF=$(mktemp -d); mkdir -p "$CONF/FalconTeam"
printf '[desktop]\ntheme_family=%s\ntheme_mode=%s\n' "$family" "$mode" \
    > "$CONF/FalconTeam/Falcon.conf"
XDG_CONFIG_HOME="$CONF" ./build-desktop/bin/falcon-desktop &
APP=$!
# 等待主窗口：取该进程下 WIDTH>=1000 的可见窗口（排除 260x59 的托盘窗口）
for id in $(xdotool search --onlyvisible --pid "$APP"); do
    W=$(xdotool getwindowgeometry --shell "$id" | sed -n 's/^WIDTH=//p')
    [ "$W" -ge 1000 ] && MAIN=$id && break
done
sleep 2
import -window "$MAIN" "theme_${family}_${mode}.png"
```

关键点：

- **必须按窗口几何挑主窗口**。一次启动会创建两个顶层窗口：260×59 的
  托盘窗口与 1200×800 的主窗口。`xdotool search --onlyvisible --pid | head -1`
  取到的是**托盘窗口**（首轮 6 张全为 260×59，像素自检 0 通过即此因）。
  改为按 `WIDTH >= 1000` 过滤后全部正确。
- **窗口 ID 可重复**：同一 Xvfb 上每次新建 X 客户端连接从同一 ID 基数
  起分配，不同启动合法地报告相同数值 ID（如 2097163 / 4194315）。
  这非陈旧截图——像素自检按家族×模式逐一核对，才是有效判据。
- 每次启动用全新 `mktemp -d` 的 `XDG_CONFIG_HOME`，避免跨组合串扰。

## 像素自检

期望值取自 `apps/desktop/src/utils/theme_tokens.hpp` 变体表（单一事实源，
优先于任何历史记录；Graphite 深色以代码 `0x0f,0x12,0x14` 为准）。

### 主背景（窗口底色，主导像素色）

| 家族 | 模式 | 期望 | 实测主导色 | 占比 |
|------|------|------|-----------|------|
| Warm | 亮 | `#f7f5f2` | `#f7f5f2` | 63.7% |
| Warm | 深 | `#16120f` | `#16120f` | 63.7% |
| Cold | 亮 | `#f4f6f8` | `#f4f6f8` | 63.7% |
| Cold | 深 | `#131519` | `#131519` | 63.7% |
| Graphite | 亮 | `#f4f7f5` | `#f4f7f5` | 63.7% |
| Graphite | 深 | `#0f1214` | `#0f1214` | 63.7% |

6/6 与期望逐字节一致。

### 家族强调色（accent，证明家族生效而非仅亮暗）

| 家族 | 模式 | 期望 | 精确像素数 |
|------|------|------|-----------|
| Warm | 亮 | `#c2410c` | 2388 |
| Warm | 深 | `#ffa07a` | 2389 |
| Cold | 亮 | `#2f6fd0` | 2392 |
| Cold | 深 | `#5b9df5` | 2385 |
| Graphite | 亮 | `#0b7d59` | 2383 |
| Graphite | 深 | `#34d399` | 2388 |

6/6 均在图中出现（约 2.4k 像素，侧栏选中态/主按钮/进度条等 accent 元素）。

## 结论

6 张截图的主背景与家族×模式 token 全一致，且各自家族的 accent 色均
出现——证明**家族**与**模式**两个维度都真实生效（若仅亮暗生效而家族
未接线，冷/石墨系将沿用 Warm 的 accent 或背景，自检即失败）。

截图与自检只证「主题系统接线正确」，**不代用户做视觉判定**（设计观感、
对比度舒适度等由用户亲验）。
