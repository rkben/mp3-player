# Windows build & port notes

Status: builds are wired for it (`CMakePresets.json` `windows` preset, `ENABLE_MPRIS`
auto-off, FFmpeg backend pinned in `main.cpp`, console-suppression via
`ProcUtil.h`), but the native media session is **not yet implemented** and it has
not been built/tested on Windows. See `notes/cross-platform.md` for the big picture.

## Toolchain & dependencies

- **MSVC** (VS 2022 Build Tools) + **Ninja**.
- **Qt 6.5+** (FFmpeg multimedia backend is the default and we rely on it) via the
  official installer or `aqtinstall`. Components: Core, Gui, Widgets, Multimedia,
  Sql, Concurrent, Network, Svg. (No DBus — MPRIS is off here.)
- **TagLib** via **vcpkg** (`vcpkg install taglib`). FFmpeg ships with Qt's
  multimedia package; otherwise vcpkg `ffmpeg`.
- **yt-dlp** — `yt-dlp.exe` (standalone, no Python needed). `findExecutable`
  resolves `.exe`; the Settings field covers non-PATH installs.

## Configure & build

```pwsh
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset windows
cmake --build --preset windows
```

The `windows` preset sets the vcpkg toolchain file and uses Ninja.

## What needs doing for a good Windows citizen

1. **Media session (M3).** Implement `SmtcSession` using
   `Windows.Media.SystemMediaTransportControls` (C++/WinRT) — media keys,
   lock-screen/now-playing, transport callbacks. Needs the top-level `HWND`
   (`ISystemMediaTransportControlsInterop`). Link `windowsapp`. Compile the file
   only under `WIN32` and return it from `MediaSession::create()` under `Q_OS_WIN`.
2. **Notifications.** `Notifier` already falls back to `QSystemTrayIcon::showMessage`
   (works unpackaged). Optional: native WinRT toasts need an AppUserModelID.
3. **Console flash.** Handled — `suppressConsoleWindow()` sets `CREATE_NO_WINDOW`
   on the yt-dlp `QProcess` (`RemoteResolver`/`Importer`/`ImportDialog`).
4. **Icons.** `QIcon::fromTheme` (volume icons) returns null on Windows; we fall
   back to `style()->standardIcon`. Consider bundling our own SVGs for parity.
5. **Filesystem identity.** NTFS is case-insensitive but `Track::key()` is a
   case-sensitive string → the same file in two casings = duplicate rows + scan
   churn. Canonicalise local paths before forming the key (`QFileInfo::canonicalFilePath`).
   Drive letters / UNC / long (>260) paths go through `QUrl::fromLocalFile`.

## Packaging

- `windeployqt build\mp3player.exe` to gather Qt DLLs + the FFmpeg media backend.
- Bundle TagLib DLL and the MSVC runtime (or static CRT).
- Ship a portable zip and/or a WiX/NSIS installer.
- Optional: Authenticode signing to reduce SmartScreen friction.

## Static build (single self-contained `.exe`) — preferred for Windows

Static linking is the recommended Windows distribution: one `.exe`, no `windeployqt`
DLL-gathering, no "missing DLL" reports, and (with a static CRT) no VC++ redist
dependency. It's well-supported on Windows/MSVC — unlike macOS (frameworks) and
Linux (distro Qt), which stay **dynamic**. So linkage is per-platform: static on
Windows only.

### Licensing

Qt is LGPLv3, TagLib LGPLv2.1. Static-linking LGPL code is permitted but carries the
**relink obligation** — users must be able to swap in their own Qt. This project ships
its source + a buildable CMake setup (and is public-domain / 0BSD), which satisfies it.
Keep the Qt/TagLib LGPL attribution in the About dialog / README. The obligation rides
on Qt, not on our code, so our code's own license/copyright status is irrelevant here.

### CRT decision drives everything

For a *true* zero-dependency exe, build Qt with **`-static -static-runtime`** (static
CRT, `/MT`). Then **every** other static lib must also be `/MT`, i.e. vcpkg triplet
**`x64-windows-static`** for TagLib + FFmpeg. (Skipping `-static-runtime` gives `/MD`,
which needs the VC redist shipped and the `x64-windows-static-md` triplet — not worth
it; go full static.)

### 1. Build static FFmpeg, minimal (also our binary-size lever)

The Homebrew/stock FFmpeg is the "everything" build; a music player needs a handful of
audio decoders. A minimal static build keeps the exe small (libavcodec ~9 MB → ~2-3 MB
and lets us drop avfilter/avdevice/swscale if Qt's plugin doesn't hard-link them):

```
./configure --disable-everything --disable-programs --disable-doc \
  --enable-static --disable-shared \
  --enable-decoder=mp3,aac,flac,vorbis,opus,alac,pcm_s16le,pcm_s24le,wavpack \
  --enable-demuxer=mp3,aac,flac,ogg,mov,wav,matroska,aiff,asf \
  --enable-parser=mpegaudio,aac,flac,vorbis,opus \
  --enable-protocol=file,http,https,tcp,tls,hls,crypto    # https/hls for streaming
```
Qt's multimedia ffmpeg backend must still resolve every libav* symbol it imports, so the
*decoder/demuxer set* shrinks but the *libraries* (avcodec/avformat/avutil/swresample,
possibly avfilter/swscale) must remain present with matching version.

### 2. Build static Qt

Only the submodules we use (no qtdeclarative — there's no QML):

```
configure -static -static-runtime -release -ltcg \
  -submodules qtbase,qtmultimedia,qtsvg,qtimageformats \
  -nomake examples -nomake tests \
  -- -DFFMPEG_DIR=<static-ffmpeg-prefix> -DCMAKE_INSTALL_PREFIX=C:\Qt-static
cmake --build . --parallel && cmake --install .
```
`-ltcg` = link-time codegen (smaller/faster). ~1-2 hr one-time build; must rebuild Qt to
update it. Point our build at it with `-DCMAKE_PREFIX_PATH=C:\Qt-static`.

### 3. Static-plugin imports (the one code change static requires)

With dynamic Qt, plugins are DLLs loaded at runtime; static Qt must **link them in** via
`Q_IMPORT_PLUGIN`. `qt_add_executable` auto-imports the default platform plugin, but the
rest must be requested or features silently break. Add to `CMakeLists.txt`, guarded so
it's a no-op for dynamic builds:

```cmake
if(WIN32 AND QT6_IS_STATIC)   # or check Qt6::Core type STATIC_LIBRARY
    qt_import_plugins(mp3player
        INCLUDE Qt6::QWindowsIntegrationPlugin   # required or the app won't start
                Qt6::QSchannelBackendPlugin       # CRITICAL: https for yt-dlp streams + covers
                Qt6::QJpegPlugin Qt6::QSvgPlugin)  # covers + our themed SVG icons
endif()
```
The **schannel TLS backend** is the easy one to forget — without it every `https://`
fetch (stream resolution, cover downloads) fails at runtime with a working-looking build.
The FFmpeg media plugin is pulled in as a static lib via `FFMPEG_DIR` above.

### Size / cost

~20-40 MB single exe with `-ltcg` + minimal FFmpeg — comparable to, often smaller than,
the dynamic DLL pile. Trade-offs: long one-time Qt build, large disk, must rebuild Qt for
updates, the plugin-import boilerplate, and harder debugging into Qt internals.

### TODO when implementing

- Add a `windows-static` CMake preset (toolchain = `x64-windows-static` vcpkg triplet,
  `CMAKE_PREFIX_PATH` = static Qt) to `CMakePresets.json`.
- Detect static Qt in CMake and emit the `qt_import_plugins` block.
- Verify Qt's ffmpeg plugin's actual libav* link set before dropping avfilter/swscale.
- Smoke-test **https streaming specifically** (the schannel-plugin failure mode).

## Smoke test checklist

Cold scan, local playback, remote import + stream (yt-dlp), playlists, settings
round-trip, **media keys**, **tray notifications**, window/header state persistence.
