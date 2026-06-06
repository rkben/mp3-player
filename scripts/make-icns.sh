#!/usr/bin/env bash
# Generate resources/macos/AppIcon.icns from a square source PNG (≥1024×1024).
#
#   scripts/make-icns.sh path/to/logo.png
#
# Uses only Apple-native tools (sips + iconutil). No SVG support — rasterize the
# SVG to a 1024px PNG first (any tool), then pass it here. Re-run cmake configure
# afterwards so the bundle picks up the new icon.
set -euo pipefail

SRC="${1:-}"
if [[ -z "$SRC" || ! -f "$SRC" ]]; then
    echo "usage: $0 <source-png>  (square, ideally 1024x1024)" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/resources/macos/AppIcon.icns"
WORK="$(mktemp -d)/AppIcon.iconset"
mkdir -p "$WORK"

# macOS iconset: each size at 1x and 2x.
for size in 16 32 128 256 512; do
    sips -z "$size" "$size"        "$SRC" --out "$WORK/icon_${size}x${size}.png"   >/dev/null
    sips -z $((size*2)) $((size*2)) "$SRC" --out "$WORK/icon_${size}x${size}@2x.png" >/dev/null
done

iconutil -c icns "$WORK" -o "$OUT"
rm -rf "$(dirname "$WORK")"
echo "wrote $OUT — re-run 'cmake --preset macos' (or your configure) to embed it."
