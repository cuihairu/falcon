# UI Design Prototypes

[简体中文](/zh/developer/design-prototypes) | **English**

Two candidate designs for the desktop main view — prototypes are reviewed
first, production code changes second. Both shots are offscreen ui_sandbox
renders (real Qt widget tree + demo task data: 2 downloading / 1 paused /
1 completed / 1 failed / 2 seeding), the "Downloading · table" view (the
first screen the app lands on), with every element — top bar / sidebar /
hero summary card / seven-column task table / status bar — dark theme
1200×800.

![Variant A · warm console (current production theme)](/prototypes/warm-console-dark.png)
![Variant B · cold utility (variant candidate)](/prototypes/cold-utility-dark.png)

<div class="prototype-caption">Left: variant A (warm console, current production theme)｜Right: variant B (cold utility, candidate)</div>

## Side-by-side comparison

| | Variant A · warm console | Variant B · cold utility |
|---|---|---|
| Direction | Current production theme: warm charcoal-neutral base + a single falcon salmon-orange accent, same family as this docs site | Cool utility variant (benchmarked against Motrix): deeper neutral base, higher-contrast text, a more restrained blue accent |
| window | `#1b1714` | `#131519` |
| card | `#262019` | `#1b1e24` |
| text | `#f2ede8` | `#f5f7fa` |
| accent | `#ffa07a` (text on accent `#27140a`) | `#5b9df5` (text on accent `#0d1a2b`) |

Contrast (WCAG AA, computed in Python):

- Variant A: text on window 15.3:1, accent on window 9.0:1, text on accent 9.1:1.
- Variant B: text on window 17.0:1 (the "high-contrast text" goal met), accent
  on window 6.6:1, text on accent 6.3:1, secondary 6.9:1.

## Known boundaries

- The salmon `#ffa07a` in the top-bar falcon logo is baked into the brand SVG
  asset and does not go through the token system — in the cold variant the logo
  keeps its brand color (152 px). That is a deliberate brand anchor, not a
  missed replacement.
- Icon tinting takes production values through `icon_utils`' `tokens_for()`;
  the Text-role icons' warm white vs cool white are indistinguishable at 16px,
  and Accent-role icons appear in exactly one place (the cloud-drive connect
  button, not on the main view) — so contrast is unaffected.
- Variant B's accent hue is a restrained blue (Motrix direction); for a
  "cool neutral base + falcon orange" combination, change the three accent
  rows of `kColdHexMap` and re-render.

## Prototype assets and regeneration

Source files are checked in under `docs/design/prototypes/` (two shots + a
README covering variant B's implementation boundaries and the full token
mapping). To regenerate:

```bash
cmake --build build-desktop --target falcon-ui-sandbox
build-desktop/bin/falcon-ui-sandbox --prototype docs/design/prototypes
```

(Requires a build-desktop tree with `FALCON_BUILD_UI_SANDBOX=ON`; offscreen
rendering, no display server needed.)

See also the [Desktop Redesign](/developer/desktop-redesign) brief for the
overall redesign scope.

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
