# Cross-Platform Plan — Windows + macOS

## Context

Pocket Player is a Qt 6 Widgets app developed on Linux. Qt already abstracts the
GUI, filesystem, networking, and (since Qt 6.5) the FFmpeg multimedia backend, so
the codebase is *mostly* portable. The real platform-specific surfaces are:

- **Media-session integration** — `MprisController` + `Qt6::DBus` (media keys,
  now-playing, lock screen). Linux-only.
- **OS notifications** — `Notifier.cpp` posts via freedesktop D-Bus, gated on
  `HAVE_MPRIS`; falls back to the in-app `ToastArea` everywhere else.
- **Freedesktop icon themes** — `MainWindow::updateVolumeIcon()` uses
  `QIcon::fromTheme("audio-volume-*")`, which returns null on Win/macOS.
- **yt-dlp subprocess** — `RemoteResolver` / `Importer` / `ImportDialog` spawn
  `yt-dlp` via `QProcess`.
- **Packaging & CI** — currently Linux-only; the `Makefile` hardcodes a local path.

Guiding principle: **one codebase**. Isolate the few native pieces behind a small
interface (or `#ifdef`), let Qt carry the rest. Most effort is media-session code,
packaging, and CI — not app logic.

---

## Workstream 1 — Build system & dependencies

- **CMake is already portable.** Add a `CMakePresets.json` with `linux`, `windows`
  (MSVC), and `macos` (universal) presets so the build is one command per platform.
- **Retire the `Makefile` as dev-only** (it pins `/home/rkb/Projects/...` for the
  two-inode AUTOMOC quirk). Document CMake/presets as the canonical build.
- **Dependency acquisition:**
  - Linux: distro packages (current).
  - Windows: **vcpkg** for `taglib` (+ FFmpeg if not bundled with Qt); Qt via the
    official installer or `aqtinstall`.
  - macOS: **vcpkg** for `taglib`/`ffmpeg`; Qt via installer/aqt. (Homebrew is fine
    for a *native-arch* build but **cannot** produce the universal binary below — it
    installs host-arch libraries only.)
  - `find_package(taglib)` and `find_package(Qt6 …)` work on all three.
  - **Universal (arm64+x86_64) caveat:** the `macos` preset sets
    `CMAKE_OSX_ARCHITECTURES=arm64;x86_64`. Qt's official macOS binaries are universal
    (6.2+), but third-party deps must be too — use vcpkg universal triplets (or build
    per-arch from source + `lipo`). If that's not worth it, **drop universal** and ship
    a native-arch build.
- **Multimedia backend:** Qt 6.5+ defaults to the FFmpeg backend on every platform
  (we rely on it for HTTP streaming + the worker-thread open). Verify at runtime;
  if a platform falls back to WMF (Windows) / AVFoundation (macOS), force
  `QT_MEDIA_BACKEND=ffmpeg`. Packaging must ship the FFmpeg libs.
- **`ENABLE_MPRIS`** already defaults OFF on non-Linux (`if(UNIX AND NOT APPLE)`),
  so `Qt6::DBus` is never pulled in on Win/macOS. Keep that.

**Exit:** clean build on all three platforms (FFmpeg backend confirmed, D-Bus
compiled out off-Linux), no native session yet. *(No automated suite remains — the
old `year_test` was removed; gate on build success + the Workstream 10 smoke list.)*

---

## Workstream 2 — Media-session integration (largest gap)

Extract a small interface so `PlayerController` doesn't know the platform:

```
class MediaSession {            // abstract
  setMetadata(title, artist, album, artUrl, lengthUs)
  setPlaybackState(Playing/Paused/Stopped)
  setPosition(us); setCanGoNext/Prev(...)
  signals: play, pause, next, previous, seek, stop, setPositionRequested
};
```

- **Linux:** wrap the existing `MprisController` as `MprisSession` (no behaviour
  change).
- **Windows:** `SystemMediaTransportControls` (SMTC) via **C++/WinRT**
  (`Windows.Media.*`) — gives media-key handling, the SoundBar/lock-screen
  now-playing, and transport callbacks. Link `windowsapp`; compile the `.cpp` only
  on Windows. Needs a window handle (`ISystemMediaTransportControlsInterop`).
- **macOS ✅ done (`NowPlayingSession.mm`):** `MPNowPlayingInfoCenter` +
  `MPRemoteCommandCenter` (`MediaPlayer.framework`) in **Objective-C++** — now-playing
  info, cover art, hardware media keys, Control Center transport. Links
  `MediaPlayer`/`AppKit`; compile the `.mm` with **`-fobjc-arc`** (it allocates
  `NSImage`/`MPMediaItemArtwork` per update). **Runtime gotcha:** the system only
  registers a **code-signed `.app` launched via LaunchServices** (`open Foo.app`),
  not a bare/unsigned binary run from the shell — ad-hoc signing is enough. Trace with
  `QT_LOGGING_RULES="pocketplayer.nowplaying=true"`.
- Wire `PlayerController` ↔ session via the interface; build the right impl per OS
  in CMake (`target_sources` guarded by `WIN32` / `APPLE`).

**Files:** new `MediaSession.h`, `MprisSession.*` (from `MprisController`),
`SmtcSession.cpp` (Win), `NowPlayingSession.mm` (mac); `PlayerController` gains a
`MediaSession*`.

---

## Workstream 3 — OS notifications  *(done)*

`Notifier::notify` already implements the full chain: D-Bus (Linux, under
`HAVE_MPRIS`) → **`QSystemTrayIcon::showMessage`** (portable native path, zero extra
deps, works unpackaged) → in-app `ToastArea` as the last-resort fallback. Nothing is
needed here for a functional Windows/macOS build.

- Optional later polish: native WinRT toasts (needs an AppUserModelID / packaged
  identity) and `UNUserNotificationCenter` on macOS (needs a signed bundle +
  entitlements). Not worth it for v1.

---

## Workstream 4 — Icons & theming

- `updateVolumeIcon()` relies on `QIcon::fromTheme` (freedesktop only). It already
  falls back to `style()->standardIcon(...)`, but those differ per platform. **Ship
  our own monochrome SVG volume icons** in `resources/` (we already bundle
  shuffle/repeat/settings) and tint them with the existing `themedIcon()` path, so
  the look is identical everywhere.
- "System" theme = native style (`windowsvista`/`macos`). Audit the QSS overrides
  in `Theme.cpp` against both native styles; the palette-based icon recolour is
  fine.
- Guard the Linux-only `plasma-integration` logging in `Theme::logPlatformTheme`.

---

## Workstream 5 — yt-dlp integration

- `QStandardPaths::findExecutable("yt-dlp")` already handles `.exe` on Windows.
  The Settings yt-dlp-path field covers non-PATH installs.
- **Windows: suppress the console flash.** `QProcess` spawning `yt-dlp.exe` pops a
  console window; set `CREATE_NO_WINDOW` via
  `QProcess::setCreateProcessArgumentsModifier` (Windows-only) in `RemoteResolver`,
  `Importer`, and `ImportDialog`.
- Consider an optional **"download/update yt-dlp" helper** (it ships as a single
  self-contained binary on all platforms) so users aren't stuck.

---

## Workstream 6 — Filesystem & identity portability

- **Case-insensitive filesystems (Windows, default macOS):** track identity is
  `Track::key()` = `url.toString()`, a *case-sensitive* string. The same file in
  two casings → two DB rows + scan churn. Mitigation: canonicalise local paths
  (`QFileInfo::canonicalFilePath`) before forming the key, or lowercase-compare on
  those platforms. Decide deliberately; document the trade-off.
- **Path separators in playlists:** `storedForm()` writes native paths
  (backslashes on Windows). `urlFromStored()` already round-trips them, but for
  portable `.m3u8` consider normalising to forward slashes on write.
- Drive letters / UNC / long (>260) paths: `QUrl::fromLocalFile` + Qt handle these;
  smoke-test.

---

## Workstream 7 — Threading / media correctness

- `MediaEngine` runs `QMediaPlayer` on a **worker thread** (for slow opens). Verify
  this holds on the Windows and macOS FFmpeg backends (no main-thread-only
  assumptions; avoid the WMF/COM-apartment path by forcing FFmpeg). Re-test the
  "slow open doesn't freeze UI" property against an SMB share.

---

## Workstream 8 — Packaging & distribution

- **Windows:** **prefer a static build** — single self-contained `.exe` (static Qt +
  static CRT + minimal static FFmpeg/TagLib via the `x64-windows-static` vcpkg
  triplet), no `windeployqt` / DLL-gathering / VC-redist. Full recipe + plugin-import
  requirements in `notes/windows.md`. Dynamic `windeployqt` (bundle FFmpeg + TagLib
  DLLs + MSVC runtime) is the fallback. Ship a portable zip and/or **WiX/NSIS
  installer**; Authenticode signing optional (reduces SmartScreen friction).
- **macOS:** `macdeployqt` → `.app`; `Info.plist`
  (`LSMinimumSystemVersion`, bundle id); **universal binary** (arm64 + x86_64);
  ship a `.dmg`.
  - **Signing/notarization — two tiers (notarization is skippable, signing is not):**
    - **v1 (no Apple account):** **ad-hoc sign** the bundle
      (`codesign --force --deep -s - PocketPlayer.app`). On **Apple Silicon the
      kernel refuses to run arm64 code with *no* signature at all** (SIGKILL,
      "damaged and can't be opened") — quarantine removal does **not** help, so an
      ad-hoc signature is mandatory (free, no Developer ID). `macdeployqt`/Xcode
      usually apply it automatically. Then users bypass the **notarization** gate
      by stripping the quarantine xattr that the browser sets:
      `xattr -dr com.apple.quarantine /Applications/PocketPlayer.app` (or
      `xattr -cr`), or simply **right-click → Open** on first launch. Ship this as a
      "first launch on macOS" instruction. (Intel can run unsigned once quarantine
      is gone; universal builds still need arm64 ad-hoc signing, so always sign.)
    - **v2 (frictionless):** **Developer ID sign + notarize + staple** ($99/yr
      Apple Developer) so the download "just works" with no user step.
- **Linux:** keep distro build; add **AppImage** (and/or Flatpak) for parity.

---

## Workstream 9 — CI/CD

- **GitHub Actions matrix:** `ubuntu-latest`, `windows-latest`,
  `macos-14` (arm) + `macos-13` (intel).
- Qt via `jurplel/install-qt-action`; deps via vcpkg (Win) / brew (mac) / apt
  (Linux).
- Steps: configure (preset) → build → deploy (windeployqt/macdeployqt/appimage) →
  upload artifacts. Add signing/notarization as gated secrets later. *(No `ctest`
  step — no automated suite remains; CI gates on build + artifact success.)*

---

## Workstream 10 — Testing

- **No automated suite** — the old `year_test`/`YearParser` was removed (year parsing
  is now inline in `MusicLibrary::parseTags`). Consider restoring a small portable
  `ctest` as a CI gate; until then CI relies on build success + manual smoke.
- Per-platform manual smoke: cold scan, local playback, **remote import + stream
  (yt-dlp resolve)**, playlists save/load, settings round-trip, **media keys**,
  **notifications**, window/header state persistence.

---

## Sequencing / milestones

1. **M1 — Builds everywhere.** CMakePresets, deps, FFmpeg backend confirmed; D-Bus
   compiled out. No native session. *(small–medium)*
2. **M2 — Portable notifications** via `QSystemTrayIcon`. ✅ *Done — the `Notifier`
   fallback chain is already implemented (Workstream 3).*
3. **M3 — Native media sessions:** `MediaSession` interface, Windows SMTC, macOS
   MPNowPlaying. *(medium–large — native code on two OSes)*
   - macOS ✅ **done & verified** (Now Playing tile, cover art, hardware media keys,
     Control Center transport — `NowPlayingSession.mm`). Windows SMTC still TODO.
4. **M4 — Packaging + CI matrix** (windeployqt / macdeployqt / AppImage; GH Actions).
   *(medium)* ← **current focus.**
   - macOS ✅ **self-contained `.dmg` working, arm64 (~21 MB, thinned) + universal
     (~48 MB)**, via `scripts/package-macos.sh` (**official Qt 6.11.1 via aqtinstall**
     — switching off Homebrew Qt fixed the `macdeployqt` duplicate-frameworks clash).
     Universal needs a universal static TagLib (`scripts/build-taglib-universal.sh`);
     Qt+FFmpeg are already fat. Size: thinning is the safe win; a **minimal-FFmpeg
     swap was dropped as too fragile** — we ship Qt's bundled FFmpeg (see
     `notes/macos.md`). yt-dlp PATH fallback added for GUI launches (launchd PATH
     lacks Homebrew). Remaining:
     notarization, GH Actions.
   - Windows: static single-exe (see Workstream 8 / `notes/windows.md`) — TODO.
   - Linux: AppImage — TODO.
5. **M5 — Polish:** bundled icons, yt-dlp console suppression, case-insensitive
   identity, signing + notarization. *(medium)*

## Key risks

- macOS **codesign/notarization** friction (Apple Developer account, entitlements).
- **C++/WinRT (SMTC)** and **Obj-C++ `.mm` (MPNowPlaying)** build integration in
  CMake (per-platform `target_sources`, framework linking).
- FFmpeg backend **packaging size** (~tens of MB of libs per platform).
- `QMediaPlayer`-on-worker-thread behaviour across the Win/macOS backends — verify
  early (it underpins the streaming + no-freeze design).

## Net assessment

App logic is already portable; ~80% of the work is **(a)** three native
media-session shims behind one interface, and **(b)** packaging/signing/CI per
platform. M1+M2 give a *functional* (if unpolished) Windows/macOS build quickly;
M3 brings it up to native-citizen quality.
