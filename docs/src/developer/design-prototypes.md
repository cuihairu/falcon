# UI Design Prototypes

[简体中文](/zh/developer/design-prototypes) | **English**

The current desktop design is `docs/design/ui-next/`: a static, dependency-free
interactive prototype. The two screenshots below are exported from that
prototype in the default **Downloading · table** view — the first screen the
app lands on — under the warm and cold accent themes. Layout, components, and
interactions are identical; only the accent tokens change.

![Warm orange theme (default)](/design/ui-next/screens/warm-dark/023-download-table.png)
![Cold blue theme](/design/ui-next/screens/cold-dark/067-download-table.png)

<div class="prototype-caption">Left: warm orange (default)｜Right: cold blue (switchable theme)</div>

## Theme tokens

| | Warm orange (default) | Cold blue |
|---|---|---|
| Direction | Warm neutral base, salmon-orange accent; the current brand | Cool utility base, restrained blue accent |
| window | `#16120f` | `#131519` |
| card | `#221c16` | `#1b1e24` |
| text | `#f7f2ec` | `#f5f7fa` |
| accent | `#ffa07a` (text on accent `#27140a`) | `#5b9df5` (text on accent `#0d1a2b`) |

Contrast is verified against WCAG AA for all three themes in both light and
dark modes. The salmon `#ffa07a` falcon logo in the top bar is a deliberate
brand anchor: it stays the same in every theme.

## What the prototype covers

The prototype has 22 screens: the existing download / discovery / cloud /
recycle / settings flows, plus the planned cross-device, account & sharing,
NAS, and Windows tray flows. Key decisions documented there:

- **Cross-device ↔ account & sharing**: devices are "things", accounts are
  "people"; sharing groups connect the two.
- **Air transfer** uses the same syncbond 6-digit matching code as
  cross-device pairing; scanning a QR code is just a faster way to enter the
  same code.
- **LocalSend** can be supported as a LAN discovery channel; AirDrop stays a
  system share entry (AWDL is proprietary); DHT is a peer-discovery layer for
  download results, not a resource site.
- **Windows tray**: persistent icon, context menu, and floating speed window;
  macOS uses a separate menu-bar implementation.

See the full Chinese design brief in the repository:
`docs/design/ui-next/README.zh.md`.

## Exporting screenshots

Screenshots live under `src/public/design/ui-next/screens/<theme>-<mode>/`.
The export mode is driven by URL parameters, for example:

```text
index.html?clean=1&theme=warm&mode=dark&screen=download-table
```

`clean=1` hides the review toolbar and disables entry animations so the image
captures a stable frame.

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
