# Falcon Browser Extension (Chrome / Edge)

This MV3 extension intercepts browser downloads and forwards the download URL to Falcon Desktop over localhost HTTP.

## Features

- Intercept downloads and send to Falcon.
- "Skip next download (this tab)".
- Disable interception on specific sites (by hostname).
- Media sniffer: detects video/audio/HLS/DASH requests and lets you choose what to download.
- Chrome + Edge (Chromium) compatible.

## Install (Dev)

1. Open `chrome://extensions` (or `edge://extensions`).
2. Enable **Developer mode**.
3. Click **Load unpacked** and select `apps/browser_extension/`.

## Desktop Side (Falcon)

- Falcon Desktop listens on `http://127.0.0.1:51337/v1/add` and will show the “Add Download” dialog when a URL is received.
- If you need to change the port, update the extension option `Falcon API endpoint` and rebuild Falcon with a matching port.
- If Falcon is not running, the extension can optionally try `falcon://add?url=...` (requires registering the `falcon://` protocol handler on your OS).

## Notes (Sniffer)

- The sniffer uses `webRequest` with `<all_urls>` host permissions to detect media requests; you can disable it in Options.
- For authenticated/DRM-protected streams, Falcon may need cookies/headers; currently the extension only sends the URL.
