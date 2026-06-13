# Linux packaging

## AppImage (current)

Single portable file with Qt bundled dynamically. Built against the **official Qt**
(from aqtinstall, under `~/Qt` — same Qt the macOS build uses), **not** the host's
system Qt. The resulting binary needs a glibc **>= the build host's**: building on
this rolling-Arch box gives an artifact for this machine + very recent distros;
the **CI** path (below) builds it in a Debian 13 container (glibc 2.41)
for a portable artifact.

### Why official Qt (the FFmpeg backend)
The earlier builds linked the **system** Qt against Arch's **desktop FFmpeg**, whose
`libavcodec` hard-links a whole encoder/hw-accel tree (`dav1d`, `rav1e`, `SvtAv1Enc`,
`libva`, `glib`, …) — that doesn't bundle reliably against mismatched host libs.
Official Qt instead ships a maintainer-tested, self-contained FFmpeg 7.1
(`libavcodec.so.61`) that uses **dlopen stub libs** (`libQt6FFmpegStub-*`) for
hw-accel/TLS — so its `libav*` drag in nothing but `z`/`bz2` and bundle cleanly. The
old per-encoder `--exclude-library` / GLib-strip hacks are gone as a result.

### Prerequisite: install official Qt
One-time, mirrors `notes/macos.md`:
```sh
python3 -m venv ~/.aqt-venv && ~/.aqt-venv/bin/pip install aqtinstall
~/.aqt-venv/bin/aqt install-qt linux desktop 6.11.1 linux_gcc_64 -m qtmultimedia -O ~/Qt
```
`qtwayland` ships in the `linux_gcc_64` base, so the Wayland platform plugin is
included. (`qtmultimedia` brings the FFmpeg backend + its bundled libs.)

```sh
scripts/package-appimage.sh        # -> Pocket_Player-x86_64.AppImage in repo root
QT_DIR=/path/to/Qt/6.11.1/gcc_64 scripts/package-appimage.sh   # override autodetect
```

The script: auto-detect `~/Qt/<ver>/gcc_64` (override via `QT_DIR`) → `qt-cmake`
Release build → `cmake --install` into `AppDir/` (install rules in `CMakeLists.txt`,
the `if(UNIX AND NOT APPLE)` block) → `linuxdeploy --plugin qt` (with `QMAKE` pointed
at that Qt) → package. Tools (`linuxdeploy`, `linuxdeploy-plugin-qt`) are cached in
`scripts/.tools/`. Needs FUSE to *run* the resulting AppImage
(`./Pocket_Player-x86_64.AppImage`, or `--appimage-extract-and-run` /
`APPIMAGE_EXTRACT_AND_RUN=1` without FUSE).

### What's bundled
Qt6 (Core/Gui/Widgets/Multimedia/Sql/Concurrent/Network/DBus/Svg) + plugins:
platforms (xcb **and wayland**), `sqldrivers/libqsqlite` (the other drivers Qt ships
— mysql/psql/oci/odbc/mimer/ibase — are stubbed past the dep scan then stripped),
`platformthemes/libqxdgdesktopportal` (system **dark/light** preference over XDG
portals — see Styling), `tls` (OpenSSL backend, for HTTPS streaming + cover/GitHub
downloads), `imageformats`, `iconengines`, and **`multimedia/` (the FFmpeg backend)**
with Qt's clean FFmpeg 7.1 libs + stubs. `yt-dlp` is **not** bundled — the in-app
managed-download feature fetches it at runtime.

### Styling (KDE / dark-light)
The AppImage uses **Fusion** (compiled into Qt Widgets) + the app's own Dark theme
(`Theme.*`), and bundles the **xdg-desktop-portal** platform theme so it follows the
system dark/light preference on KDE/GNOME/any desktop. It deliberately does **not**
bundle KDE **Breeze** / the Plasma platform theme: those pull in the whole KF6 stack
(KConfig/KIO/KIconThemes/KColorScheme/XmlGui/… ~30-40 libs) and are tightly coupled
to the KF6 + Qt versions — impractical and fragile to ship. (This is a bundling
choice, *not* an AppImage permissions/sandbox limitation — AppImages aren't
sandboxed; see linuxdeploy-plugin-qt issue #5 "Dealing with platform themes".)

## CI (forgejo-runner)
CI runs under **forgejo-runner**. The portable build is produced in a **Debian 13
(trixie)** container — glibc 2.41, current stable — for an artifact that runs on
Debian 13+/Ubuntu 24.04+/recent distros. The job `apt-get`s the toolchain +
`libtag1-dev` (trixie's TagLib 2.0.2 has the CMake config, so no from-source build)
+ the X11/xcb/wayland runtime libs linuxdeploy resolves, installs official Qt via
aqtinstall into `/opt/Qt`, then runs `scripts/package-appimage.sh` with `QT_DIR`
set and `APPIMAGE_EXTRACT_AND_RUN=1` (the CI container has no FUSE). Release publish
(attach the AppImage to a Forgejo release on tag builds) is still TODO.

### Wayland (was the prior failure)
linuxdeploy-plugin-qt only bundles the **xcb** platform plugin by default: the Wayland
platform plugin and its nested integration plugins are loaded by *name*, not as
`NEEDED` libraries, so they're missed unless forced. The script forces them:

```sh
EXTRA_PLATFORM_PLUGINS="libqwayland.so"   # filename auto-detected (official Qt: libqwayland.so)
EXTRA_QT_MODULES="wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client"
# (also exported as the deprecated EXTRA_QT_PLUGINS for older linuxdeploy-plugin-qt)
```

and adds an AppRun hook setting `QT_QPA_PLATFORM=wayland;xcb` so it prefers Wayland but
**falls back to xcb (XWayland)** if the wayland plugin can't load — the app launches
either way. `libwayland-client` is deliberately **not** bundled (it must match the
host compositor's protocols; linuxdeploy's excludelist keeps the host copy).

Verify after a build:
```sh
./Pocket_Player-x86_64.AppImage --appimage-extract
ls squashfs-root/usr/plugins/platforms/        # libqxcb.so AND libqwayland.so
ls squashfs-root/usr/plugins/wayland-shell-integration/
ls squashfs-root/usr/plugins/multimedia/        # *ffmpeg*
# FFmpeg must be clean: only libav* (+z/bz2/stubs), no dav1d/rav1e/SvtAv1/glib/va
objdump -p squashfs-root/usr/plugins/multimedia/libffmpegmediaplugin.so | grep NEEDED
WAYLAND_DISPLAY=$WAYLAND_DISPLAY QT_QPA_PLATFORM=wayland ./Pocket_Player-x86_64.AppImage  # forces wayland
```

## Static build (deferred)

A fully static single ELF is **not** done — it's heavy and poorly supported for Qt
Multimedia:
- Requires Qt built from source with `-static` (aqtinstall has no static Qt), plus
  **static FFmpeg** and **static TagLib**.
- Needs `qt_import_plugins(...)` for the static SQLite / TLS / platform / multimedia
  plugins — same idea as the existing Windows-static block in `CMakeLists.txt`.
- Qt Multimedia + FFmpeg static linking is the main blocker.
- LGPLv3 static linking carries relink obligations (ship relinkable objects, or rely
  on the open-source exception).

Revisit as its own task if a single static binary is ever required.
