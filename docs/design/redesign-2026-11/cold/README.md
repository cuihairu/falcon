[中文](README.zh.md) | **English**

# Direction B · cold utility II

The continuation of the 2026-09-29 "cold utility" prototype (Motrix-benchmarked): a deeper neutral base,
higher-contrast text, and a restrained blue accent. The dark palette is **verbatim** `kColdHexMap` from the
earlier prototype (window `#131519`, accent `#5b9df5`); the light palette is a new cold face in the same family.

Prototype renders: **28 images** (14 functional faces × light/dark themes), all produced by the ui_sandbox
offscreen pipeline (real Qt widget tree + demo task data, zero engine/network side effects).

## Token table (production warm → variant)

**Dark** (`kColdIiDark`) — matches `kColdHexMap` from the 2026-09-29 prototype, including the accent family:

| Role | Production | Variant |
|---|---|---|
| window | `#1b1714` | `#131519` |
| card | `#262019` | `#1b1e24` |
| divider | `#37302a` | `#2b3038` |
| text | `#f2ede8` | `#f5f7fa` |
| text_secondary | `#b5aca3` | `#98a0ab` |
| text_disabled | `#6e665e` | `#5a6169` |
| 控件底 (button/input) | `#312a24` | `#22262d` |
| 控件描边 (control border) | `#4a423a` | `#363c45` |
| 分段钮 checked 底 (segmented checked) | `#3a322b` | `#262b33` |
| 卡片 hover 描边 (card hover border) | `#554b41` | `#3d444e` |
| accent | `#ffa07a` | `#5b9df5` |
| accent hover | `#ffb28c` | `#7ab1f8` |
| accent pressed | `#e0875e` | `#4585e0` |
| accent 上文字 (accent text) | `#27140a` | `#0d1a2b` |

**Light** (`kColdIiLight`) — new cold face; `#ffffff` dual-role untouched:

| Role | Production | Variant |
|---|---|---|
| window | `#f6f4f1` | `#f4f6f8` |
| text | `#292524` | `#1e252c` |
| text_secondary | `#57534e` | `#5c6672` |
| text_disabled | `#a8a29e` | `#a2abb5` |
| checkbox 描边环 (checkbox ring) | `#a29a92` | `#a3acb6` |
| accent | `#c2410c` | `#2f6fd0` |
| accent hover | `#9a3412` | `#275cb3` |
| accent pressed | `#7c2d12` | `#1f4c96` |
| divider | `#e7e5e4` | `#dee3e8` |
| 边框/分隔强 (strong border) | `#dcd7d1` | `#d2d9e0` |
| hover 底 (hover base) | `#f5f2ee` | `#eef1f4` |
| 控件底 (control base) | `#edeae5` | `#e3e8ed` |
| primaryButton disabled 底 | `#e8e3dd` | `#e1e8f0` |
| 卡片 hover 描边 | `#cac2b9` | `#c5ccd4` |

## Layout / component differences

**None.** Token-only re-skin: the component tree, layout parameters, spacing, and interactions are identical to
production. Only the hex values inside the two QSS files (and the rebuilt QPalette) differ. The checkbox SVGs
are regenerated in the variant colors, and the tray menu (14th face) inherits the direction palette.

## 取舍 (tradeoffs)

- **Pro**: the highest-contrast text of the three (dark **17.03:1**), the most restrained accent (a calm blue
  that reads "utility" rather than "media"), and the darkest neutral base — closest to a "power tool" feel.
- **Con**: the furthest from the falcon brand orange; the logo stays salmon (brand anchor) while everything
  else turns blue, which is the largest stylistic tension of the three.
- The light face is brand new (the 2026-09-29 prototype shipped dark only) — both themes are now coherent in
  the cold family.

## Contrast (WCAG AA, Python-computed)

Dark: text 17.03 / accent 6.60 / accent-text 6.32 / secondary 6.32. Light: text 14.29 / accent 4.50 /
white-on-accent 4.88 / secondary 5.84. All 24 pair checks ≥ 4.5:1.

## Known boundaries (recorded as-is)

- The salmon `#ffa07a` of the top bar falcon logo is baked into the brand SVG asset and does not go through the
  token system — every direction keeps the logo in the brand color. Intentional brand anchor.
- Icon coloring takes production values via `icon_utils`'s `tokens_for()`; Text-role icon colors differ
  imperceptibly at 16px, and the Accent-role icon (cloud drive connect button) is not in the main view.
  Fixing this would be a production change, out of scope for a prototype-only task.
- `rgba(...)` overlay tokens (hover/pressed tints) are not mapped; they carry over from production.
- Light checkbox ring maps `#a29a92 → #a3acb6`; the production ring nuance is preserved by dedicated map rows.
- `#ffffff` is dual-role (card + accent_text) in light; safe because every light direction keeps
  accent_text `#ffffff`.

## Face naming note

`download_table_*` captures actually show the **grid-card** view and `download_grid_*` the **table**, because
`DownloadPage`'s production default is `TaskDisplayStyle::Grid` (`download_page.cpp:82`) and the sandbox
captures before/after `toggle_display_style()`. This naming convention is inherited from the already-shipped
`docs/design/ui-sandbox` matrix — kept consistent here rather than re-rendered.

## Regeneration

```bash
cmake --build build-ui-audit --target falcon-ui-sandbox
build-ui-audit/bin/falcon-ui-sandbox --redesign docs/design/redesign-2026-11
```

(Needs a build tree configured with `FALCON_BUILD_UI_SANDBOX=ON` and system Qt6; offscreen rendering, no
display environment required. The `--redesign` mode is new in `apps/desktop/dev/ui_sandbox.cpp` and touches
only the sandbox session — production `theme_tokens.hpp` / `fluent_*.qss` are untouched.)
