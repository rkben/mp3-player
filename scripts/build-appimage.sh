#!/usr/bin/env bash
# Build a self-contained AppImage of Pocket Player.
#
# Bundles Qt (incl. the svg image plugin and the multimedia/ffmpeg backend) via
# linuxdeploy + its Qt plugin. Run from anywhere; needs network access the first
# time to fetch the linuxdeploy tools. Requires qmake6 on PATH (qt6-base).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD=build-appimage
APPDIR="$ROOT/AppDir"
TOOLS="$ROOT/tools"

# 1. Build and install into AppDir.
cmake -B "$BUILD" -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD" -j"$(nproc)"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD"

# 2. Fetch linuxdeploy + the Qt plugin (cached in tools/).
mkdir -p "$TOOLS"
fetch() {
    local out="$TOOLS/$1"
    [ -x "$out" ] || { wget -q -O "$out" "$2"; chmod +x "$out"; }
}
BASE=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous
QBASE=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous
fetch linuxdeploy           "$BASE/linuxdeploy-x86_64.AppImage"
fetch linuxdeploy-plugin-qt "$QBASE/linuxdeploy-plugin-qt-x86_64.AppImage"

# 3. Produce the AppImage. EXTRA_QT_PLUGINS pulls in the plugins linuxdeploy
#    can't infer from symbols (svg icons, the ffmpeg media backend).
#    APPIMAGE_EXTRACT_AND_RUN lets the tool AppImages (and appimagetool) run
#    without FUSE — otherwise the deploy succeeds but no .AppImage is emitted.
#
#
# 3. Produce the AppImage.
export QMAKE="${QMAKE:-qmake6}"
export EXTRA_QT_MODULES="svg;multimedia;waylandcompositor"
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
export OUTPUT="Pocket_Player-x86_64.AppImage"
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP=1

# --- CREATE SQL STUBS ---
# Prevent linuxdeploy from aborting on missing database client libs.
STUBDIR="$(mktemp -d)"
trap 'rm -rf "$STUBDIR"' EXIT
for lib in libfbclient.so.2 libmariadb.so.3 libpq.so.5 libodbc.so.2 libmimerapi.so; do
    cc -shared -x c -o "$STUBDIR/$lib" /dev/null 2>/dev/null || true
done
export LD_LIBRARY_PATH="$STUBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# --- BUNDLE KDE PLASMA INTEGRATION & BREEZE ---
# Manually copy the host's Plasma theme and Breeze style plugins into the AppDir.
# linuxdeploy will automatically trace these and bundle required KDE Frameworks.
mkdir -p "$APPDIR/usr/plugins/platformthemes" "$APPDIR/usr/plugins/styles"
cp /usr/lib/qt6/plugins/platformthemes/KDEPlasmaPlatformTheme.so "$APPDIR/usr/plugins/platformthemes/" || true
cp /usr/lib/qt6/plugins/styles/breeze*.so "$APPDIR/usr/plugins/styles/" || true

# --- DEPLOYMENT PHASE 1: GATHER ---
# Run linuxdeploy to gather dependencies and plugins, but DO NOT build the AppImage yet.
"$TOOLS/linuxdeploy" \
    --appdir "$APPDIR" \
    --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/pocketplayer.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/pocketplayer.svg"

for prefix in \
    libwayland- \
    libxkbcommon \
    libdrm \
    libgbm \
    libEGL \
    libGL \
    libGLESv2 \
    libglapi \
    libffi \
    libva \
    libvdpau \
    libsystemd \
    libudev
do
    rm -f "$APPDIR/usr/lib/${prefix}"*.so*
done

# --- DEPLOYMENT PHASE 3: PACKAGE ---
# Package the cleaned AppDir into the final AppImage.
"$TOOLS/linuxdeploy" \
    --appdir "$APPDIR" \
    --output appimage

echo "Built $ROOT/$OUTPUT"
