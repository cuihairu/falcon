[中文](README.zh.md) | **English**

# Direction C · graphite emerald

A free direction, not a continuation of an existing family: a near-black graphite neutral base with an emerald
accent. Neither a Motrix derivative nor a Fluent clone — the palette stands on its own.

Prototype renders: **28 images** (14 functional faces × light/dark themes), all produced by the ui_sandbox
offscreen pipeline (real Qt widget tree + demo task data, zero engine/network side effects).

## Token table (production warm → variant)

**Dark** (`kGraphiteDark`):

| Role | Production | Variant |
|---|---|---|
| window | `#1b1714` | `#0f1214` |
| card | `#262019` | `#161b1e` |
| divider | `#37302a` | `#262d31` |
| text | `#f2ede8` | `#eef2f0` |
| text_secondary | `#b5aca3` | `#9ba5a2` |
| text_disabled | `#6e665e` | `#5d6663` |
| 控件底 (button/input) | `#312a24` | `#1d2326` |
| 控件描边 (control border) | `#4a423a` | `#333c40` |
| 分段钮 checked 底 (segmented checked) | `#3a322b` | `#222a2d` |
| 卡片 hover 描边 (card hover border) | `#554b41` | `#3b4449` |
| accent | `#ffa07a` | `#34d399` |
| accent hover | `#ffb28c` | `#5fe0ae` |
| accent pressed | `#e0875e` | `#1fb07e` |
| accent 上文字 (accent text) | `#27140a` | `#052017` |

**Light** (`kGraphiteLight`) — cool grey-green white face; `#ffffff` dual-role untouched:

| Role | Production | Variant |
|---|---|---|
| window | `#f6f4f1` | `#f4f7f5` |
| text | `#292524` | `#1b231f` |
| text_secondary | `#57534e` | `#55605b` |
| text_disabled | `#a8a29e` | `#a1aaa6` |
| checkbox 描边环 (checkbox ring) | `#a29a92` | `#a3aca8` |
| accent | `#c2410c` | `#0b7d59` |
| accent hover | `#9a3412` | `#096649` |
| accent pressed | `#7c2d12` | `#064f39` |
| divider | `#e7e5e4` | `#e0e6e3` |
| 边框/分隔强 (strong border) | `#dcd7d1` | `#d3ddd8` |
| hover 底 (hover base) | `#f5f2ee` | `#eef2f0` |
| 控件底 (control base) | `#edeae5` | `#e2e9e5` |
| primaryButton disabled 底 | `#e8e3dd` | `#dcebe3` |
| 卡片 hover 描边 | `#cac2b9` | `#c2cdc7` |

## Layout / component differences

**None.** Token-only re-skin: the component tree, layout parameters, spacing, and interactions are identical to
production. Only the hex values inside the two QSS files (and the rebuilt QPalette) differ. The checkbox SVGs
are regenerated in the variant colors, and the tray menu (14th face) inherits the direction palette.

## 取舍 (tradeoffs)

- **Pro**: the only direction that establishes a genuinely new desktop identity — a near-black base with a
  saturated emerald accent reads distinctly "product" rather than "media player". The darkest window of the
  three (`#0f1214`) with the highest accent-on-window contrast (**9.78:1**).
- **Con**: the largest stylistic break from the falcon brand. The logo stays salmon (brand anchor, baked into
  the SVG) while every accent becomes emerald, so the two colours must coexist in the top bar. This is the
  direction most likely to be rejected on brand grounds, and also the one that most changes the product feel.
- Emerald on a near-black base is a well-trodden combination (terminal/developer tooling), so it risks reading
  as "generic dev tool" rather than as a distinct product identity.

## Contrast (WCAG AA, Python-computed)

Dark: accent-on-window **9.78**; all 24 pair checks ≥ 4.5:1. Light accent `#0b7d59` on `#ffffff` meets AA.

## Known boundaries (recorded as-is)

- The salmon `#ffa07a` of the top bar falcon logo is baked into the brand SVG asset and does not go through the
  token system — every direction keeps the logo in the brand color. Intentional brand anchor, not a missed
  replacement.
- Icon coloring takes production values via `icon_utils`'s `tokens_for()`; Text-role icon colors differ
  imperceptibly at 16px, and the Accent-role icon (cloud drive connect button) is not in the main view.
  Fixing this would be a production change, out of scope for a prototype-only task.
- `rgba(...)` overlay tokens (hover/pressed tints) are not mapped; they carry over from production.
- Light checkbox ring maps `#a29a92 → #a3aca8`; the production ring nuance (`#a29a92` vs token `#a8a29e`) is
  preserved by dedicated map rows.
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
