#!/usr/bin/env bash
# Build Pocket Player into a Linux AppImage (dynamic Qt bundled).
#
#   scripts/package-appimage.sh
#   QT_DIR=/path/to/Qt/6.11.1/gcc_64 scripts/package-appimage.sh   # override Qt
#
# Built against the *official* Qt (aqtinstall, under ~/Qt — same as the macOS
# build), NOT the host's system Qt. Official Qt ships a maintainer-tested, self-
# contained FFmpeg backend that uses dlopen *stub* libs (libQt6FFmpegStub-*) for
# hw-accel/TLS, so its libav* drag in nothing but z/bz2 — it bundles cleanly. The
# host's desktop FFmpeg instead hard-links a whole encoder tree (dav1d/rav1e/
# SvtAv1/va/glib/…) that doesn't bundle reliably against mismatched host libs.
# The AppImage needs a glibc >= the build host's — fine on this machine;
# the Woodpecker/Debian-13 pipeline (.woodpecker.yml) builds it in a controlled,
# older-glibc base for a portable artifact. yt-dlp is NOT bundled; the app
# downloads/manages it at runtime.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD="${BUILD_DIR:-build-appimage}"
APPDIR="$ROOT/AppDir"
TOOLS="$ROOT/scripts/.tools"
JOBS="$(nproc)"

# Official Qt selection: auto-detect the newest ~/Qt/<ver>/gcc_64 (mirrors the
# macOS QT_DIR pattern in the Makefile), override via the QT_DIR env var. We build
# AND deploy against this Qt so linuxdeploy bundles its clean FFmpeg, not the host's.
QT_DIR="${QT_DIR:-$(ls -d "$HOME"/Qt/*/gcc_64 2>/dev/null | sort -V | tail -1)}"
[[ -n "$QT_DIR" && -x "$QT_DIR/bin/qt-cmake" ]] || {
    echo "error: no official Qt found under ~/Qt; install it with"
    echo "       aqt install-qt linux desktop 6.11.1 linux_gcc_64 -m qtmultimedia -O ~/Qt"
    echo "       or set QT_DIR=/path/to/Qt/<ver>/gcc_64 (see notes/linux.md)"
    exit 1
}
echo "==> Using Qt: $QT_DIR"

echo "==> Configuring + building ($BUILD)"
"$QT_DIR/bin/qt-cmake" -B "$BUILD" -S . -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$JOBS"

echo "==> Installing into AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr

echo "==> Fetching linuxdeploy + qt plugin"
mkdir -p "$TOOLS"
fetch() {  # url filename
    local dest="$TOOLS/$2"
    [[ -x "$dest" ]] || { echo "    $2"; curl -fL "$1" -o "$dest"; chmod +x "$dest"; }
}
fetch https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage linuxdeploy
fetch https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage linuxdeploy-plugin-qt
export PATH="$TOOLS:$PATH"

# --- Wayland (the prior pain point) ---------------------------------------------
# The Wayland platform plugin and its nested integration plugins are loaded by NAME
# at runtime, not as NEEDED libraries, so linuxdeploy-plugin-qt does NOT pull them in
# automatically (it only bundles xcb by default). Force them:
#   * EXTRA_PLATFORM_PLUGINS  -> the wayland platform plugin .so under platforms/
#   * EXTRA_QT_PLUGINS        -> the wayland-*-integration plugin *categories*
# libwayland-client is deliberately left UNbundled (it must match the host
# compositor's protocol set); linuxdeploy's excludelist keeps host glibc/libwayland.
# Point linuxdeploy-plugin-qt at the official Qt (NOT a system qmake), so it
# deploys those libs/plugins + the bundled clean FFmpeg.
export QMAKE="$QT_DIR/bin/qmake6"
[[ -x "$QMAKE" ]] || QMAKE="$QT_DIR/bin/qmake"
[[ -x "$QMAKE" ]] || { echo "error: no qmake under $QT_DIR/bin"; exit 1; }

# The wayland platform plugin's filename varies by Qt build (libqwayland.so on Arch,
# libqwayland-generic.so elsewhere), so detect it instead of hardcoding.
QT_PLUGINS_DIR="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
WL_PLATFORM="$(cd "$QT_PLUGINS_DIR/platforms" 2>/dev/null && ls libqwayland*.so 2>/dev/null | tr '\n' ';' || true)"
if [[ -z "$WL_PLATFORM" ]]; then
    echo "WARNING: no wayland platform plugin found under $QT_PLUGINS_DIR/platforms"
    echo "         install qt6-wayland (Arch) / qtwayland (others) for Wayland support."
fi
export EXTRA_PLATFORM_PLUGINS="$WL_PLATFORM"
# EXTRA_QT_PLUGINS was renamed to EXTRA_QT_MODULES in newer linuxdeploy-plugin-qt;
# set both so the wayland integration plugins get bundled on old and new tool builds.
export EXTRA_QT_MODULES="wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client"
export EXTRA_QT_PLUGINS="$EXTRA_QT_MODULES"
echo "    wayland platform plugin: ${WL_PLATFORM:-<none>}"

# linuxdeploy-plugin-qt insists on deploying EVERY Qt SQL driver, but official Qt
# ships Firebird/Mimer/MySQL/Oracle/ODBC/PostgreSQL drivers whose client libs
# aren't installed, which hard-fails the dependency scan. We only use SQLite.
# Provide empty stubs so the scan passes; the unused drivers + stubs are stripped
# back out after deploy (below).
mkdir -p "$TOOLS/stubs"
make_stub() {  # soname
    local out="$TOOLS/stubs/$1"
    [[ -f "$out" ]] || echo "void stub(){}" | \
        gcc -x c -shared -Wl,-soname,"$1" -o "$out" - 2>/dev/null || true
}
# One per non-sqlite driver's NEEDED client lib (see plugins/sqldrivers/*.so).
make_stub libfbclient.so.2      # libqsqlibase  (Firebird/InterBase)
make_stub libmimerapi.so        # libqsqlmimer
make_stub libmysqlclient.so.21  # libqsqlmysql
make_stub libclntsh.so.23.1     # libqsqloci    (Oracle)
make_stub libodbc.so.2          # libqsqlodbc
make_stub libpq.so.5            # libqsqlpsql
export LD_LIBRARY_PATH="$TOOLS/stubs:${LD_LIBRARY_PATH:-}"

# Official Qt's FFmpeg is self-contained (its libav* drag in only z/bz2 + the
# Qt FFmpeg dlopen stubs), so the old encoder-tree excludes are gone. The GLib
# stack still needs excluding: libQt6Core NEEDs it, and a bundled GLib that
# mismatches the host's makes ld.so crash relocating against the host copy. Keep
# the host's (matching) GLib; the AppRun hook also sets QT_NO_GLIB so Qt never
# spins up the GLib event loop.
EXCLUDES=(
    "libglib-2.0*" "libgobject-2.0*" "libgio-2.0*" "libgmodule-2.0*"
)
EXCL_ARGS=(); for p in "${EXCLUDES[@]}"; do EXCL_ARGS+=(--exclude-library "$p"); done

# linuxdeploy's OWN bundled `strip` is too old to handle modern ELFs with a
# .relr.dyn (packed relative relocations) section — it aborts on every Arch/Debian-13
# lib ("unknown type [0x13] section .relr.dyn"). So we always pass NO_STRIP=1 to
# linuxdeploy and instead strip ourselves below with the host's (newer) strip.
echo "==> Deploying Qt (QMAKE=$QMAKE)"
NO_STRIP=1 linuxdeploy \
    --appdir "$APPDIR" \
    --plugin qt \
    "${EXCL_ARGS[@]}" \
    --desktop-file "$APPDIR/usr/share/applications/pocketplayer.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/pocketplayer.svg"

# Keep only the SQLite driver; drop the other drivers and the stub client libs that
# only existed to satisfy linuxdeploy's dependency scan.
echo "==> Trimming SQL drivers to SQLite only"
find "$APPDIR/usr/plugins/sqldrivers" -name 'libqsql*.so' ! -name 'libqsqlite.so' -delete 2>/dev/null || true
rm -f "$APPDIR"/usr/lib/libfbclient.so.2 "$APPDIR"/usr/lib/libmimerapi.so \
      "$APPDIR"/usr/lib/libmysqlclient.so.21 "$APPDIR"/usr/lib/libclntsh.so.23.1 \
      "$APPDIR"/usr/lib/libodbc.so.2 "$APPDIR"/usr/lib/libpq.so.5 2>/dev/null || true

# Belt-and-suspenders: if a bundled GLib slipped past the excludes, drop it so the
# host's (matching) copy is used — a bundled-vs-host GLib mismatch crashes ld.so.
rm -f "$APPDIR"/usr/lib/lib{glib-2.0,gobject-2.0,gio-2.0,gmodule-2.0}.so* 2>/dev/null || true

echo "==> Verifying bundled plugins"
ok=1
chk() { if ls "$APPDIR"/usr/plugins/$1 >/dev/null 2>&1; then echo "    ok   $1";
        else echo "    MISS $1"; ok=0; fi; }
chk "platforms/libqxcb.so"
chk "platforms/libqwayland*.so"
chk "wayland-shell-integration"
chk "sqldrivers/libqsqlite.so"
# xdg-desktop-portal theme: conveys the system dark/light preference (and accent)
# over portals on KDE/GNOME/any — our portable alternative to bundling Breeze/KF6.
# The base widget style stays Fusion (compiled into Qt Widgets, no plugin needed).
chk "platformthemes/libqxdgdesktopportal.so"
chk "tls"
chk "multimedia"
ls "$APPDIR"/usr/plugins/multimedia/*ffmpeg* >/dev/null 2>&1 \
    && echo "    ok   multimedia/ffmpeg" || { echo "    MISS multimedia/ffmpeg"; ok=0; }
[[ "$ok" == 1 ]] || echo "WARNING: some plugins missing — the AppImage may fail (esp. Wayland/streaming)."

# AppRun hooks: pin the bundled FFmpeg backend, and try Wayland with an xcb
# (XWayland) fallback so the app still launches if the wayland plugin can't load.
echo "==> Writing AppRun hooks (media backend + wayland->xcb fallback)"
mkdir -p "$APPDIR/apprun-hooks"
cat > "$APPDIR/apprun-hooks/01-pocketplayer.sh" <<'HOOK'
export QT_MEDIA_BACKEND="${QT_MEDIA_BACKEND:-ffmpeg}"
# Use Qt's own event dispatcher, not the bundled GLib one: a bundled-vs-host GLib
# double-init segfaults during GUI startup.
export QT_NO_GLIB="${QT_NO_GLIB:-1}"
# Prefer Wayland on a Wayland session, fall back to xcb (XWayland) if its plugin
# can't load. Honour an explicit user override.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland;xcb}"
HOOK

echo "==> Building AppImage"
NO_STRIP=1 linuxdeploy --appdir "$APPDIR" "${EXCL_ARGS[@]}" --output appimage

echo "==> Done:"
ls -1 "$ROOT"/Pocket*.AppImage 2>/dev/null || echo "    (AppImage in repo root)"
