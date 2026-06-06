#!/usr/bin/env bash
# Build Pocket Player into a self-contained, ad-hoc-signed .dmg.
#
#   scripts/package-macos.sh                 # native arm64, official Qt
#   ARCHS="arm64;x86_64" scripts/package-macos.sh   # universal (needs universal TagLib)
#   QT_DIR=~/Qt/6.11.1/macos scripts/package-macos.sh   # explicit Qt
#
# Uses **official Qt** (installed via aqtinstall — see notes/macos.md), NOT Homebrew
# Qt: only the official binaries pair cleanly with macdeployqt and bundle Qt's own
# FFmpeg multimedia backend. Native arm64 only; universal needs universal TagLib.
#
# Steps: configure -> build -> macdeployqt (gather Qt + FFmpeg) -> trim unused
# plugins -> ad-hoc codesign -> .dmg. Ad-hoc signing (codesign -s -) is MANDATORY on
# Apple Silicon: the kernel refuses to run unsigned arm64 code. A frictionless
# download additionally needs a Developer ID signature + notarization (not done here).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Locate official Qt: $QT_DIR, else newest ~/Qt/<ver>/macos. (Homebrew Qt is
# deliberately not used — macdeployqt is unreliable against it.)
QT_DIR="${QT_DIR:-$(ls -d "$HOME"/Qt/*/macos 2>/dev/null | sort -V | tail -1)}"
[[ -n "$QT_DIR" && -x "$QT_DIR/bin/macdeployqt" ]] || {
    echo "error: no official Qt found. Install it, e.g.:" >&2
    echo "  python3 -m venv ~/aqt-venv && ~/aqt-venv/bin/pip install aqtinstall" >&2
    echo "  ~/aqt-venv/bin/aqt install-qt mac desktop 6.11.1 clang_64 -m qtmultimedia qtimageformats --outputdir ~/Qt" >&2
    echo "Then re-run, or set QT_DIR=/path/to/Qt/<ver>/macos" >&2
    exit 1
}
MACDEPLOYQT="$QT_DIR/bin/macdeployqt"
echo "==> Using Qt at $QT_DIR"

ARCHS="${ARCHS:-arm64}"                       # "arm64;x86_64" for universal
UNIVERSAL=0; [[ "$ARCHS" == *";"* ]] && UNIVERSAL=1
BUILD="${BUILD_DIR:-$([[ $UNIVERSAL == 1 ]] && echo build-mac-uni || echo build-mac)}"

# TagLib for find_package. Prefer the from-source **static** TagLib (built with our
# deployment target, contains arm64+x86_64) — Homebrew's TagLib is built for the
# host's *current* macOS, so bundling it would refuse to load on the older macOS we
# claim to support (LSMinimumSystemVersion). Homebrew is only a fallback for a quick
# arm64 build that won't be distributed to older systems.
TAGLIB_STATIC="${TAGLIB_PREFIX:-${DEPS_DIR:-$ROOT/.deps}/taglib-universal}"
if [[ -f "$TAGLIB_STATIC/lib/libtag.a" ]]; then
    PREFIX_PATH="$TAGLIB_STATIC;/opt/homebrew"
    echo "==> TagLib: static $TAGLIB_STATIC ($(lipo -archs "$TAGLIB_STATIC/lib/libtag.a"))"
elif [[ $UNIVERSAL == 1 ]]; then
    echo "error: universal build needs the universal static TagLib at $TAGLIB_STATIC" >&2
    echo "  run: scripts/build-taglib-universal.sh" >&2
    exit 1
else
    PREFIX_PATH="/opt/homebrew"
    echo "WARNING: falling back to Homebrew TagLib — built for this host's macOS, so the" >&2
    echo "         dmg may NOT run on older macOS. Run scripts/build-taglib-universal.sh" >&2
    echo "         for a distributable build." >&2
fi

echo "==> Configuring ($ARCHS)"
"$QT_DIR/bin/qt-cmake" -B "$BUILD" -S . -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_PREFIX_PATH="$PREFIX_PATH"

echo "==> Building"
cmake --build "$BUILD" -j

APP="$BUILD/mp3player.app"
[[ -d "$APP" ]] || { echo "error: $APP not built as a bundle" >&2; exit 1; }

echo "==> macdeployqt (gathering Qt frameworks + FFmpeg backend)"
# Deliberately NOT using macdeployqt's -dmg / -codesign: it signs *before* its own
# later framework surgery, leaving an invalid signature (Apple Silicon then SIGKILLs
# the app). We sign and build the dmg ourselves below, after all edits settle.
# (Stray "no file at .../libpq/libiodbc" errors are harmless — macdeployqt probing
# the ODBC/Postgres SQL drivers we trim next; we only use SQLite.)
"$MACDEPLOYQT" "$APP" -always-overwrite || true

echo "==> Trimming unused Qt plugins"
PLUGINS="$APP/Contents/PlugIns"
# We use only the SQLite driver; drop ODBC/Postgres/MySQL (they pull external libs).
find "$PLUGINS/sqldrivers" -type f ! -name "*sqlite*" -delete 2>/dev/null || true
# No QML/Qt Quick or virtual keyboard in this Widgets app.
rm -rf "$PLUGINS/platforminputcontexts" "$PLUGINS/virtualkeyboard" \
       "$APP/Contents/Resources/qml" 2>/dev/null || true

# Thin to the single target arch for a non-universal build. Official Qt's frameworks
# (and its bundled FFmpeg) are fat, so macdeployqt copies both slices even for an
# arm64-only build — thinning ~halves the bundle. Skipped for universal. (QtGui
# hard-links QtDBus on macOS, so QtDBus stays — just thinned, not deleted.)
if [[ "$ARCHS" != *";"* ]]; then
    echo "==> Thinning bundle to $ARCHS"
    while read -r f; do
        file "$f" | grep -q "Mach-O" || continue
        lipo -archs "$f" 2>/dev/null | grep -qw "$ARCHS" || continue
        [[ $(lipo -archs "$f" 2>/dev/null | wc -w) -gt 1 ]] || continue   # already thin
        lipo "$f" -thin "$ARCHS" -output "$f.thin" 2>/dev/null && mv "$f.thin" "$f"
    done < <(find "$APP" -type f \( -name "*.dylib" -o -perm +111 \))
fi

echo "==> Ad-hoc signing (deep, after all modifications)"
# Mandatory on Apple Silicon. For a frictionless download, replace '-' with your
# Developer ID and add notarization (see notes/macos.md).
codesign --remove-signature "$APP" 2>/dev/null || true
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP" && echo "    signature valid"

echo "==> Building .dmg"
DMG="$BUILD/mp3player.dmg"
rm -f "$DMG"
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"   # drag-to-install affordance
# ULFO (LZFSE) compresses better than UDZO/zlib; readable since macOS 10.11.
hdiutil create -volname "Pocket Player" -srcfolder "$STAGE" -ov -format ULFO "$DMG" >/dev/null
rm -rf "$STAGE"

echo "==> Done: $DMG"
echo "    First launch on another Mac: right-click -> Open, or"
echo "    xattr -dr com.apple.quarantine '/Applications/Pocket Player.app'"
