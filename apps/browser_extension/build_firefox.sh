#!/usr/bin/env bash
# Build the Firefox variant of the extension package from the shared source
# tree. The Chromium package is zipped straight from this directory; Firefox
# needs a patched manifest (event-page background, gecko id, strict minimum
# version) and must not ship this build script itself.
#
# Usage: build_firefox.sh <output.zip>   (jq and zip are required)
set -euo pipefail

src="$(cd "$(dirname "$0")" && pwd)"
out_zip="${1:?usage: build_firefox.sh <output.zip>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir "$work/pkg"
cp -r "$src/." "$work/pkg/"
rm -f "$work/pkg/build_firefox.sh"

# Manifest variant: Chromium service worker -> Firefox event page scripts
# (api.js first, so the shim binds before the worker body runs). Firefox
# never supported background service workers in stable release; event pages
# are the supported background for MV3 there.
#   - gecko.id: AMO requires a stable add-on id for unlisted/listed builds.
#   - strict_min_version 115: storage.session (used with a local fallback),
#     MV3 event pages and the scripting API are all available from 115.
jq '.background = {"scripts": ["api.js", "service_worker.js"]}
    | .browser_specific_settings = {"gecko": {"id": "falcon@cuihairu.github.io", "strict_min_version": "115.0", "data_collection_permissions": {"required": ["none"]}}}
    | del(.minimum_chrome_version)' "$work/pkg/manifest.json" > "$work/pkg/manifest.json.new"
mv "$work/pkg/manifest.json.new" "$work/pkg/manifest.json"

# Zip with manifest.json at the archive root (load-time requirement for
# temporary add-ons), same layout contract as the Chromium package.
( cd "$work/pkg" && zip -qr "$out_zip" . )

# Structural self-check: same mandatory-file set as the Chromium verify
# step, plus the Firefox-specific manifest shape.
rm -rf "$work/verify" && mkdir "$work/verify"
unzip -q "$out_zip" -d "$work/verify"
for f in manifest.json api.js service_worker.js \
         _locales/en/messages.json _locales/zh_CN/messages.json \
         batch/batch.html options/options.html popup/popup.html; do
  test -f "$work/verify/$f" || { echo "MISSING $f"; exit 1; }
done

jq -e '
  .background.scripts == ["api.js", "service_worker.js"]
  and (.background.service_worker | not)
  and .browser_specific_settings.gecko.id == "falcon@cuihairu.github.io"
  and (.browser_specific_settings.gecko.strict_min_version | strings | startswith("115"))
  and .browser_specific_settings.gecko.data_collection_permissions.required == ["none"]
  and (.minimum_chrome_version | not)
' "$work/verify/manifest.json" > /dev/null \
  || { echo "firefox manifest shape wrong"; cat "$work/verify/manifest.json"; exit 1; }

test -f "$work/pkg/build_firefox.sh" && { echo "build script leaked into package"; exit 1; } || true

# Optional: keep the patched tree for consumers like `web-ext lint`, which
# must lint the Firefox manifest variant, not the Chromium base tree.
if [[ -n "${KEEP_TREE:-}" ]]; then
  mkdir -p "$KEEP_TREE"
  cp -r "$work/pkg/." "$KEEP_TREE/"
fi

echo "firefox package OK: $out_zip"
