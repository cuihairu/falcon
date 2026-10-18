[中文](README.zh.md) | **English**

# Main View Design Prototypes (Dark, 1200×800)

Two candidate versions of the desktop UI redesign — prototype first for review, then touch production code. Both images are
ui_sandbox offscreen renders (real components + demo task data: 2 downloading / 1 paused / 1 completed /
1 failed / 2 seeding), in the "Downloading · Table" view (the first screen once the app lands), with all elements present:
top bar / sidebar / hero summary card / seven-column task table / status bar.

| File | Direction | Key tokens |
|---|---|---|
| `warm-console-dark.png` | A: the current production warm console theme as-is (warm black neutral base + single falcon salmon-orange accent, same source as the docs site) | window `#1b1714` / card `#262019` / text `#f2ede8` / accent `#ffa07a` (text on accent `#27140a`) |
| `cold-utility-dark.png` | B: a colder utility-style variant (benchmarked against Motrix: deeper neutral base, higher-contrast text, more restrained blue accent) | window `#131519` / card `#1b1e24` / text `#f5f7fa` / accent `#5b9df5` (text on accent `#0d1a2b`) |

## Variant B's implementation boundary (important)

Variant B's token replacement **happens only inside the sandbox session** — hot→cold hex mapping applied to the dark QSS text
plus whole-string replacement of the checkbox embedded base64 SVG plus rebuilding the QPalette from cold tokens; production
`theme_tokens.hpp` / `fluent_dark.qss` remain **untouched**. To adopt it, just write the mapping values of `kColdHexMap`
from `ui_sandbox.cpp` into the production tokens/QSS (if B is chosen).

Contrast (WCAG AA, computed in Python): cold accent on window 6.6:1, text on accent 6.3:1, text 17.0:1 (warm is 15.3:1 —
the "high-contrast text" goal is achieved), secondary 6.9:1.

Known boundaries (recorded as-is):

- The salmon `#ffa07a` of the top bar falcon logo (`:/icons/falcon.svg`) is baked into the brand
  SVG asset and does not go through the token system — the cold version keeps the logo in the brand color (152 px).
  This is an intentional brand anchor, not a missed replacement.
- Icon coloring takes production values via `icon_utils`'s `tokens_for()`; for Text-role icons,
  warm white vs cold white is indistinguishable at 16px, and the Accent-role icon appears in only one place,
  the cloud drive connect button (not present in the main view) — contrast is unaffected.
- The accent hue chosen is a restrained blue (Motrix direction); for a "cold neutral base + falcon
  orange" combination, change the three accent rows of `kColdHexMap` and re-render.

## Regeneration

```bash
cmake --build build-desktop --target falcon-ui-sandbox
build-desktop/bin/falcon-ui-sandbox --prototype docs/design/prototypes
```

(Requires a build-desktop tree with FALCON_BUILD_UI_SANDBOX=ON; offscreen rendering,
no display environment needed.)
