[中文](README.zh.md) | **English**

# Direction A · warm console II

The iterative continuation of the current production "warm console" direction: the same warm-black neutral base
and the single falcon salmon-orange accent, deepened and re-tuned. This is the **smallest delta** of the three
proposals — it stays on the brand family shared with the docs site, and just tightens the palette.

Prototype renders: **28 images** (14 functional faces × light/dark themes), all produced by the ui_sandbox
offscreen pipeline (real Qt widget tree + demo task data, zero engine/network side effects).

## Token table (production warm → variant)

**Dark** (`kWarmIiDark`):

| Role | Production | Variant |
|---|---|---|
| window | `#1b1714` | `#16120f` |
| card | `#262019` | `#221c16` |
| divider | `#37302a` | `#3a322a` |
| text | `#f2ede8` | `#f7f2ec` |
| text_secondary | `#b5aca3` | `#bcb2a6` |
| text_disabled | `#6e665e` | `#756c62` |
| 控件底 (button/input) | `#312a24` | `#2d2620` |
| 控件描边 (control border) | `#4a423a` | `#4e453c` |
| 分段钮 checked 底 (segmented checked) | `#3a322b` | `#362e26` |
| 卡片 hover 描边 (card hover border) | `#554b41` | `#5a4f44` |

**Light** (`kWarmIiLight`) — accent family unchanged (`#c2410c` / `#9a3412` / `#7c2d12`), `#ffffff` dual-role
(card + accent_text) untouched:

| Role | Production | Variant |
|---|---|---|
| window | `#f6f4f1` | `#f7f5f2` |
| text | `#292524` | `#262220` |
| text_secondary | `#57534e` | `#524d47` |
| text_disabled | `#a8a29e` | `#aaa39c` |
| checkbox 描边环 (checkbox ring) | `#a29a92` | `#aba49b` |
| divider | `#e7e5e4` | `#e4e1dc` |
| 边框/分隔强 (strong border) | `#dcd7d1` | `#d8d3cb` |
| hover 底 (hover base) | `#f5f2ee` | `#f4f1ec` |
| 控件底 (control base) | `#edeae5` | `#eae6df` |
| primaryButton disabled 底 | `#e8e3dd` | `#e7e1d9` |
| 卡片 hover 描边 | `#cac2b9` | `#c5bdb2` |

## Layout / component differences

**None.** This is a token-only re-skin: the component tree, layout parameters, spacing, and interactions are
identical to production. Only the hex values inside the two QSS files (and the rebuilt QPalette) differ. The
checkbox SVGs are regenerated in the variant colors (same 16×16 `rx=3` geometry), and the tray menu (the 14th
face, new to this matrix) inherits the direction palette automatically.

## 取舍 (tradeoffs)

- **Pro**: the safest proposal — near-identical to what ships today, so the review risk is minimal and the
  delta is easy to audit; keeps full brand continuity with the docs site.
- **Con**: the least transformative; if the goal is a visibly different desktop identity, this direction
  delivers the least change per unit of effort.
- The dark window deepens to `#16120f` (a touch closer to true black) and the text brightens to `#f7f2ec`,
  raising text-on-window contrast to **16.73:1** (from 15.3:1).

## Contrast (WCAG AA, Python-computed)

Dark: text 16.73 / accent 9.37 / accent-text 8.87 / secondary 8.07. Light: text 14.49 / accent 4.76 /
white-on-accent 5.18 / secondary 8.36. All 24 pair checks ≥ 4.5:1.

## Known boundaries (recorded as-is)

- The salmon `#ffa07a` of the top bar falcon logo (`:/icons/falcon.svg`) is baked into the brand SVG asset and
  does not go through the token system — every direction keeps the logo in the brand color. Intentional brand
  anchor, not a missed replacement.
- Icon coloring takes production values via `icon_utils`'s `tokens_for()`; Text-role icon colors differ
  imperceptibly at 16px, and the Accent-role icon (cloud drive connect button) is not in the main view.
  Fixing this would be a production change, out of scope for a prototype-only task.
- `rgba(...)` overlay tokens (hover/pressed tints) are not mapped; they carry over from production.
- Light checkbox ring maps `#a29a92 → #aba49b`; the production ring nuance (`#a29a92` vs token `#a8a29e`) is
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
