[简体中文](/zh/ui/design-spec) | **English**

# Desktop UI Design Specification

This is the single authoritative UI design for Falcon, replacing all earlier UI explorations (now removed). It merges the existing desktop screens with the planned cross-device, account & sharing, and NAS flows into one interactive prototype at `docs/design/ui-next/`.

- Default accent: **warm orange** (the current brand, unchanged)
- Switchable accents: **cold blue** / **graphite green**
- Modes: **light / dark / follow system**
- Rule: prototype only — production code is never touched

## Reviewing the prototype

Open `index.html` directly in a browser (static, no dependencies).

- The top toolbar switches accent and mode across every screen at once, for direct comparison.
- The left sidebar navigates screen by screen: **24 screens** (7 modal previews + 1 Web view + 1 Windows tray).
- `←` / `→` page through screens; switches, checkboxes, segmented controls, and the matching-code input are interactive.

## Theme system (key decision)

Earlier designs presented three directions as competing alternatives. This version folds them into **switchable accent themes within one product**: identical layout, components, spacing, and interactions; only tokens change.

| Token | warm orange (default) light / dark | cold blue light / dark | graphite green light / dark |
|---|---|---|---|
| window | `#f6f4f1` / `#16120f` | `#f4f6f8` / `#131519` | `#f4f7f5` / `#0f1214` |
| card | `#ffffff` / `#221c16` | `#ffffff` / `#1b1e24` | `#ffffff` / `#161b1e` |
| text | `#292524` / `#f7f2ec` | `#1e252c` / `#f5f7fa` | `#1b231f` / `#eef2f0` |
| accent | `#c2410c` / `#ffa07a` | `#2f6fd0` / `#5b9df5` | `#0b7d59` / `#34d399` |
| text on accent | `#ffffff` / `#27140a` | `#ffffff` / `#0d1a2b` | `#ffffff` / `#052017` |

- All three directions were checked against WCAG AA (24 contrast pairs per direction, all ≥ 4.5:1).
- The salmon `#ffa07a` Falcon logo in the top bar is a brand anchor baked into the SVG and stays constant across themes.
- Derived tokens (`--accent-soft`, `--accent-ring`, `--track`, status colours) follow the accent and switch with it.
- Theme controls live in Preferences → Appearance: accent (warm / cold / graphite) and mode (light / dark / **follow system**). Auto mode follows `prefers-color-scheme` live; the review toolbar is a global reviewer control kept in sync with the settings page.

## Screen inventory (24 screens)

**Existing structure (measured from the local Falcon client + 14 legacy feature areas)**

| # | Screen | Notes |
|---|---|---|
| 1 | Downloading · card view | Home; stats cards, four task states (downloading / paused / failed) |
| 2 | Downloading · table view | Same data as a seven-column table; **default landing view** |
| 3 | Task context menu | 10 items + shortcuts + "Move to" submenu + multi-select state; shows original peer count / remaining time; no membership copy |
| 4 | Resource discovery | Keyword + size filters + relevance sort + one-click download; enabled sites collapsed to icons by default |
| 5 | Cloud space | S3/COS and NAS connection cards, file list |
| 6 | Recycle bin | 7-day retention, restore / permanent delete |
| 7 | Preferences | Appearance / clipboard / download / BitTorrent (incl. Daemon RPC) |
| 8 | Add task (modal) | URL / destination / advanced (connections, seeding, UA, Referer, Cookie) |

**Cross-device & nodes (things) / account & sharing (people)**

| # | Screen | Notes |
|---|---|---|
| 9 | Cross-device | Replaces "My devices"; 5 sync switches + my devices (matching code) + air-transfer shortcut |
| 10 | Show transfer code (modal) | QR + 6-digit code together; one-time, 120 s, copy, refresh; for a device or another person |
| 11 | Matching-code pairing (modal) | Scan / manual input toggle (same code); scanning is faster, typing is the fallback |
| 12 | NAS & router | Feiniu / OpenWrt / generic NAS; capability tags (WebDAV/SMB/SFTP/native package) |
| 13 | Add NAS node (modal) | Presets + protocol parameters (WebDAV/SMB/SFTP/S3) |
| 14 | Account | Later phase · personal centre: profile, stats, security, evolution path |
| 15 | Friends | Later phase · add friend by matching code, requests, list, direct transfer |
| 16 | Air transfer | Standalone entry under Account & Sharing; send/receive share one code; recipients: my devices, friends, one-off sessions |
| 17 | Sharing groups | Later phase · group my devices with friends' devices; members / permissions / key epoch |
| 18 | Share records | Later phase · trace of air transfers and group shares; codes themselves are not retained |
| 19 | Login (modal) | Reuses the existing login dialog; entry may be hidden in phase 1 |
| 20 | Edit profile (modal) | Nickname, avatar, read-only Falcon ID |
| 21 | File details (modal) | From the context menu: size, path, link, original peer count, verification; can jump to cloud |
| 22 | NAS / router Web admin | Minimal set: tasks + storage + pairing, LAN only |
| 23 | Windows system tray | Windows only: persistent icon (idle/download/paused/complete) + context menu + two-state speed window (mini capsule / expanded card); macOS uses a menu-bar equivalent, not drawn here |

> 7 modal previews: #8 add task, #10 show code, #11 pairing, #13 add NAS node, #19 login, #20 edit profile, #21 file details.
>
> **Pairing mechanism**: scanning and manual entry are two forms of the same 6-digit code and run the same flow (syncbond / SPAKE2, end-to-end encrypted; the relay never sees the code or key). Scanning is faster, typing is the fallback.

## Confirmed design decisions

### Account & Sharing (naming and semantics)

- The navigation group is **Account & Sharing**, not "Account & Relationships". Cross-device & nodes governs **things**; Account & Sharing governs **people**; sharing groups connect the two (a group holds both my devices and friends' devices).
- Air transfer belongs to Account & Sharing: it is a **sharing action** that reuses the same syncbond matching code as cross-device pairing — not a second mechanism. The cross-device page keeps a device-oriented shortcut.

### Transfer and discovery protocols

| Capability | Decision | Treatment in resource discovery |
|---|---|---|
| LocalSend | Public protocol v2/v2.2 (UDP multicast `224.0.0.167:53317` discovery + HTTP(S) REST `53317` transport, certificate fingerprint / PIN). Falcon can implement as a LocalSend peer, complementary to matching codes. | Never mixed into the site list; nearby devices live in air transfer / channel settings, or feed the global search "Other devices" category |
| AirDrop | AWDL is Apple-proprietary; third-party apps cannot implement the protocol. Only a "open system share sheet" entry; Android interoperates via Quick Share. | At most a system-share button in discovery; no device list |
| DHT | Fits BitTorrent peer discovery underneath results (better peer counts and credibility), not keyword resource search; do not build Falcon-node discovery on DHT in phase 1. | Show "DHT peers / sources: DHT + Tracker + PEX" in results; no new page |

- Resource discovery stays **pure content search**. Nearby devices enter, if at all, via the global search "Other devices" category or a lightweight "Nearby devices" card that jumps to air transfer — never as a pseudo resource site.
- Library assessment: implement LocalSend natively with Qt (`QUdpSocket` + `QNetworkAccessManager` + `QSslCertificate::digest()`), no third-party dependency; DHT via `mainline` (Rust, MIT) or `libtorrent-rasterbar` (C++/BSD); no AWDL reverse-engineering library.

### Platform differences

- Windows: persistent tray icon + context menu + floating speed window, shaped after Xunlei (#23). The speed window has two states: default mini capsule (hexagonal icon + single speed `3.7 MB/s`), expanding on click into a detail card (up/down speed + progress + task count); always-on-top, draggable, toggled from the tray menu.
- macOS: menu-bar icon with a drop-down menu following macOS status-bar conventions; a separate implementation from the Windows tray, marked only (no mockup in this version).

### Task context menu (#3)

- The 10-item Xunlei-shaped menu is Falcon-branded: Pause `Ctrl P`, Open folder `Ctrl E`, Add to favourites, File details `Ctrl I`, Add to cloud `Ctrl Y`, Delete `Del`, Permanent delete `Shift Del`, Move to (submenu), Copy download link `Ctrl C`, Deselect all (multi-select state).
- Membership-trial copy is removed; task rows add "original peer count / remaining time" to judge source quality and completion time.
- "File details" and "Add to cloud" are navigable actions (#21 and #5); the menu and the list share the same task object.

## Exported design images

Static exports live at `/design/ui-next/screens/`:

- 3 accents × light/dark × 24 screens = **144 PNGs** in directories such as `warm-light` and `cold-dark`. The two newest screens continue the numbering at the end of each directory (e.g. `warm-light/023-task-context-menu.png`, `024-task-detail.png`).
- File numbers are export-batch order; the screen name is the suffix. Docs and READMEs reference these images.

## Open items for review

1. Confirm warm orange as the default accent (implemented as such).
2. Keep both cold blue and graphite green as built-in themes, or only one?
3. Are the two sidebar groups (Cross-device & nodes / Account & Sharing) in the right place?
4. Hide account entries in phase 1 (#14/15/17/18 are design placeholders)?
5. Does LocalSend compatibility enter phase 1; show DHT peer counts in phase-1 results?
6. Are the Windows tray menu items and the two-state speed window as expected?
7. Is the NAS Web admin "tasks + storage + pairing" minimum set enough?
