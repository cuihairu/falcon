# Desktop Redesign Brief

This document defines the interface redesign direction for Falcon Desktop. The goal is to establish a reviewable visual standard before implementation in Qt, rather than settling for a low-quality recoloring pass.

## Design goals

Falcon Desktop should feel like a high-performance download console, not a generic administration backend. The interface must foreground task status, speed, queues, and resource entry points so users understand whether the download system is healthy within three seconds of opening the app.

Core goals:

- Strengthen product identity: establish Falcon's own visual language instead of stacking default Qt controls.
- Improve task readability: download status, speed, progress, and actions must stand out more than decorative elements.
- Reduce cognitive load: preserve the current feature structure while reorganizing the information hierarchy.
- Control implementation risk: change styles, layout, and component appearance first; do not touch the download engine, storage, search, or IPC logic.

## Visual direction

The recommended direction is `Deep Signal`: a dark console base, teal signal light, warm-orange primary actions, and glass-like task panels.

Why this direction suits a download tool:

- A dark base supports long-running sessions and reduces visual fatigue.
- Teal and blue communicate networking, speed, connectivity, and stability.
- Warm orange is reserved for high-priority actions such as "New Download" and "Start Now", avoiding saturation across the entire interface.
- Cards and tables use low-contrast borders and layered shadows so the task list reads like a dashboard, not a form.

Alternative directions:

- `Graphite Pro`: a light graphite-gray professional tool style, suited to office use and cross-platform consistency.
- `Kinetic Glass`: mixed light/dark glassmorphism with stronger visual impact, but significantly higher Qt QSS implementation complexity.
- `Neo Classic`: macOS-style sidebar and content cards; safe, but lacking differentiation.

## Information architecture

### Top bar

The top bar is the global task entry point, not a conventional title bar.

Suggested structure:

- Left: Falcon mark and a small version/runtime-status badge.
- Center: a command bar combining link input and resource search.
- Right: refresh, view, more, and window controls.

Suggested copy:

- Search placeholder: `Paste a link, magnet, ed2k, or search resources`
- Status badge: `Signal Ready` or `Engine Online`
- Primary action: `New Download`

### Sidebar

The sidebar should evolve from a "feature menu" into a "task workspace".

Suggested groups:

- `Transfer Queue`: Downloading, Completed, Cloud Add.
- `Resource Network`: Resource Discovery, Cloud Storage.
- `System`: Preferences.

Remove or demote the currently low-value "Private Space" entry unless real private-download functionality arrives later.

### Download page

The download page is the primary experience and should be rebuilt as three layers:

- Hero status panel: current mode, total speed, active tasks, primary action.
- Summary cards: active tasks, completed tasks, current speed, failed/waiting tasks.
- Task surface: table and card views with one consistent visual style.

The task list must emphasize:

- Filename and source.
- Progress bar.
- Real-time speed.
- Status badge.
- Inline actions such as pause, resume, and delete.

### Status bar

The status bar should read like system telemetry, not a pile of buttons.

Suggested content:

- Left: schedule, remote, plugins.
- Center: real-time speed, task count, connection status.
- Right: download detection and notification badges.

## Design tokens

### Colors

Suggested `Deep Signal` tokens:

| Token | Purpose | Value |
| --- | --- | --- |
| `bg.app` | Application background | `#08111F` |
| `bg.sidebar` | Sidebar | `#0B1726` |
| `bg.surface` | Content panel | `#101D2E` |
| `bg.surfaceElevated` | Floating cards | `#15263A` |
| `border.subtle` | Subtle border | `#22364D` |
| `text.primary` | Primary text | `#EAF2FF` |
| `text.secondary` | Secondary text | `#8EA3BA` |
| `brand.signal` | Brand signal color | `#36E3B4` |
| `brand.action` | Primary action color | `#FFB454` |
| `state.error` | Error | `#FF5C7A` |
| `state.warning` | Warning | `#FFD166` |
| `state.success` | Success | `#35D07F` |

### Typography

The cross-platform Qt implementation should prefer an available system font stack:

```text
"Segoe UI Variable", "SF Pro Display", "Microsoft YaHei UI", "Noto Sans CJK SC", sans-serif
```

Suggested type scale:

| Level | Size | Weight |
| --- | --- | --- |
| Hero title | 24px | 700 |
| Section title | 16px | 700 |
| Body | 13px | 500 |
| Metadata | 12px | 500 |
| Caption | 11px | 600 |

### Spacing and radii

Use a 4px base grid:

- Page margin: 24px.
- Card padding: 16px or 20px.
- Control spacing: 8px, 12px, 16px.
- Standard corner radius: 12px.
- Card corner radius: 18px.
- Command bar corner radius: 20px.

## Qt implementation strategy

### Phase 1: low-risk visual refresh

Change only the UI layer:

- `apps/desktop/src/utils/theme_manager.cpp`
- `apps/desktop/src/widgets/top_bar.cpp`
- `apps/desktop/src/navigation/sidebar.cpp`
- `apps/desktop/src/widgets/status_bar.cpp`
- Minor object-name, spacing, and copy adjustments in `download_page.cpp` where necessary.

Do not change:

- `DownloadEngine`
- `TaskManager`
- Protocol handlers
- Cloud-storage and resource-search business logic
- IPC and system-tray logic

### Phase 2: component consolidation

If Phase 1 is approved, abstract:

- `MetricCard`
- `CommandBar`
- `StatusBadge`
- `TaskActionButton`
- `NavItem`

These components should keep a single responsibility; do not mix business state and styling in one component.

### Phase 3: motion and empty states

Qt Widgets motion must remain restrained:

- Use a subtle opacity/position transition when a new task appears.
- Use a gradient chunk for progress bars.
- Add graphical signal ripples to empty states without introducing a complex animation system.

## Acceptance criteria

Implementation must satisfy:

- The main window, top bar, sidebar, download page, settings page, and resource-discovery page share one visual language.
- Table and card views remain readable in the dark theme without relying on pure-black/pure-white contrast.
- The primary action is the only globally prominent action; secondary buttons do not compete for focus.
- Task status and progress are more prominent than decoration.
- Desktop builds complete on Linux, macOS, and Windows CI.
- If Qt is unavailable locally, do not claim completion until GitHub Actions has verified the build.

## Current constraints

This session has no available Figma MCP tool, so it cannot generate Figma boards directly. After Figma-related skills are installed, and once a Figma MCP connection exists, follow this flow:

1. Use `figma-use` to inspect the target Figma file.
2. Use `figma-generate-design` to generate the main-window board from this brief.
3. Verify screenshots before starting code implementation.
4. Use `figma-implement-design` to turn the approved design into Qt implementation constraints.

Without Figma MCP, this brief is the implementation source of truth.
