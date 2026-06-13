# macOS build & port notes

Status: **builds, runs, and packages** on macOS with official Qt 6.11.1 (aqtinstall),
**arm64 and universal (arm64+x86_64)** — universal verified: both slices launch
(arm64 native, x86_64 via Rosetta) and all 35 bundle Mach-Os are fat. The native
media session (`NowPlayingSession.mm`) is **implemented and verified**.
`scripts/package-macos.sh` produces a self-contained ad-hoc-signed `.dmg`: ~48 MB
universal, or **~21 MB** arm64 (auto-thinned, full FFmpeg). Remaining: notarization
(Developer ID). See `notes/cross-platform.md` for the big picture.

## Toolchain & dependencies

Setup is **native arm64 + official Qt** (decided 2026-06). Official Qt — *not*
Homebrew Qt — because only the official binaries pair cleanly with `macdeployqt`
(Homebrew Qt clashes: duplicate frameworks + unreliable deploy) and they **bundle
Qt's own FFmpeg multimedia backend**, so no external FFmpeg is needed.

- **Xcode command-line tools** (clang) + **Ninja** (`brew install ninja`).
- **Qt 6.11.1** via **aqtinstall** (one-time):
  ```sh
  python3 -m venv ~/aqt-venv && ~/aqt-venv/bin/pip install aqtinstall
  ~/aqt-venv/bin/aqt install-qt mac desktop 6.11.1 clang_64 \
      -m qtmultimedia qtimageformats --outputdir ~/Qt
  ```
  `qtsvg` ships in base (needed for the SVG icons); `qtmultimedia` pulls the
  bundled FFmpeg libs + `libffmpegmediaplugin`. Lands at `~/Qt/6.11.1/macos`.
  - **Version pin matters on macOS 26:** Qt **6.8.x LTS links the removed
    `AGL.framework`** and fails to link against the macOS 26 SDK — use 6.10+/6.11.
- **TagLib** — build a **static** TagLib from source with
  `scripts/build-taglib-universal.sh` → `.deps/taglib-universal` (in-repo, gitignored;
  universal, contains arm64; links straight into the app, nothing to bundle).
  Source/build artifacts also stay under `.deps/` — never in `$HOME` — so a checkout is
  self-contained for CI. Override with `DEPS_DIR=` / `TAGLIB_PREFIX=`. `package-macos.sh` uses it
  for **both** arm64 and universal. Homebrew TagLib is only a *fallback* for throwaway
  arm64 builds — it's compiled for the host's *current* macOS, so a bundled Homebrew
  `libtag` refuses to load on the older macOS we claim to support (the script warns).
  Static TagLib requires `find_package(ZLIB)` in CMake (it exports `ZLIB::ZLIB` in its
  link interface; already wired).
  - **Deployment target = 13.0** (`LSMinimumSystemVersion`): official Qt 6.11
    frameworks require macOS 13+, so all components we build (app, TagLib) use
    `-mmacosx-version-min=13.0` to keep that floor honest.
- **yt-dlp** — `brew install yt-dlp`. The Settings field covers non-PATH installs.

## Configure & build

Configure with the official Qt's `qt-cmake` (sets the toolchain/prefix for you):

```sh
~/Qt/6.11.1/macos/bin/qt-cmake -B build-mac -S . -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0   # official Qt 6.11 frameworks require macOS 13+
cmake --build build-mac -j
```

`build-mac/` (gitignored) is the official-Qt build dir, kept separate from any
Homebrew `build/`. The `macos` CMakePresets entry still targets universal
(`arm64;x86_64`) for a *future* universal build, which would additionally need a
universal TagLib (vcpkg universal, or per-arch + `lipo`) — out of scope for now.

## What needs doing for a good macOS citizen

1. **Media session (M3). ✅ Done & verified (macOS 26).** `NowPlayingSession.mm`
   (Objective-C++) drives `MPNowPlayingInfoCenter` (title/artist/album/art/
   duration/elapsed + playback state) and routes `MPRemoteCommandCenter`
   (play/pause/toggle/next/prev/stop + timeline scrub) back to `PlayerController`.
   Built only under `APPLE` (`enable_language(OBJCXX)`, the `.mm` compiled with
   `-fobjc-arc`, links `MediaPlayer`/`AppKit`), returned from
   `MediaSession::create()` under `Q_OS_MACOS`. Confirmed working: Now Playing tile
   + cover art, hardware media keys (F7/F8/F9), and Control Center transport.

   **Runtime gotcha (cost us a debugging session):** the system's MediaRemote daemon
   only registers a **code-signed `.app` launched through LaunchServices** — a bare
   or unsigned binary run straight from the shell never becomes the Now Playing app
   and never receives media keys. Ad-hoc signing suffices. Remote-command handlers
   must capture by **value**, not via a `[&]` C++ lambda: the heap-copied Obj-C block
   outlives the setup frame, so a reference capture dangles → segfault on first key.

   To exercise it (the GUI/Control-Center path can't be smoke-tested headless):
   ```sh
   codesign --force --deep --sign - build/mp3player.app
   open build/mp3player.app \
     --env QT_LOGGING_RULES=pocketplayer.nowplaying=true \
     --stdout /tmp/pp.log --stderr /tmp/pp.log
   # play a track, hit Control Center / media keys, then: grep nowplaying /tmp/pp.log
   ```
2. **Notifications.** `Notifier` falls back to `QSystemTrayIcon::showMessage`
   (menu-bar item). Native `UNUserNotificationCenter` needs a signed bundle +
   entitlements — defer.
3. **Icons.** `QIcon::fromTheme` returns null on macOS; we fall back to
   `style()->standardIcon`. Consider bundling our own SVGs for parity.
4. **Filesystem identity.** Default APFS is case-insensitive → the same file in
   two casings = duplicate rows + scan churn (`Track::key()` is case-sensitive).
   Canonicalise local paths before forming the key.

## Packaging

**`scripts/package-macos.sh`** does the whole thing: finds the official Qt
(`$QT_DIR` or newest `~/Qt/*/macos`), configures via `qt-cmake`, builds, runs
`macdeployqt` (gathers Qt frameworks + bundled FFmpeg + TagLib, rewriting all
`/opt/homebrew` refs to `@executable_path`), trims unused plugins, ad-hoc signs,
and produces `build-mac/mp3player.dmg`. The result is fully self-contained (~48 MB)
— verified to have no external `/opt/homebrew` / `/usr/local` deps.

- **Universal:** `ARCHS="arm64;x86_64" scripts/package-macos.sh` → `build-mac-uni/`.
  Needs the universal static TagLib first (`scripts/build-taglib-universal.sh`); the
  script errors with that hint if it's missing. Official Qt frameworks + FFmpeg are
  already universal, so the arm64-only dmg is the *same ~48 MB* (macdeployqt copies
  the fat frameworks either way) — to actually shrink an arm64-only build you'd
  `lipo -thin` the frameworks, a separate size task.

- The bundle/`Info.plist` (bundle id `dev.rkben.pocketplayer`,
  `LSMinimumSystemVersion`, icon) come from `resources/macos/Info.plist.in` wired in
  `CMakeLists.txt` (the `APPLE` block); app icon via `scripts/make-icns.sh` if a PNG
  is provided, else the default Qt icon.
- We **don't** use `macdeployqt -dmg`/`-codesign`: it signs *before* its own later
  framework surgery, leaving an invalid signature (Apple Silicon then SIGKILLs the
  app). The script signs + builds the dmg itself, after all edits settle.
- Stray `macdeployqt` `libpq`/`libiodbc` errors are harmless — it probes the
  ODBC/Postgres SQL drivers, which the script then deletes (we ship only SQLite).
### Size reduction (arm64 single-arch) → ~21 MB dmg

The dmg went **48 MB → ~21 MB** via two safe levers (universal stays fat for breadth):

1. **Thinning (automatic, single-arch builds).** Official Qt's frameworks + bundled
   FFmpeg are fat, and `macdeployqt` copies both slices even for an arm64-only build,
   so the bundle is ~half x86_64 dead weight. The script `lipo -thin`s every Mach-O
   to the target arch after deploy. (QtGui *hard-links* QtDBus on macOS, so QtDBus
   stays — thinned, not deleted, or the app won't load.) This is the big, robust win.
2. **ULFO dmg** (LZFSE, vs UDZO/zlib) — a couple MB for free.

#### Minimal FFmpeg — swap dropped on purpose (we ship Qt's bundled FFmpeg)

Qt's bundled `libavcodec` is ~13.5 MB thinned (mostly video codecs we never decode).
We had a working swap: a soname-matched audio-only FFmpeg build (`libavcodec` ~1.2 MB,
plus `@rpath` id fixups) dropped into the bundle, shaving a further ~23 MB off the dmg.
It **worked**, but it was too fragile to depend on — it hinged on hand-matching the
exact FFmpeg point release to the libav* sonames Qt happened to bundle, and on Qt's
indirect re-export of ~49 libavutil symbols staying put; any Qt update could silently
break playback ("Failed to load media"). Not worth that maintenance risk for a music
player, so the swap (and `build-ffmpeg-min.sh`) was removed. We use Qt's bundled
FFmpeg unmodified. If a smaller FFmpeg is ever truly needed, the robust route is
building Qt itself against a minimal FFmpeg (custom Qt) — out of scope.

Result: `scripts/package-macos.sh` → **~21 MB** self-contained dmg (thinned, Qt's
bundled FFmpeg, streaming works).

### Signing / notarization (notarization is skippable, signing is **not**)

- **v1 — no Apple account:** **ad-hoc sign** the bundle
  (`codesign --force --deep -s - mp3player.app`). On **Apple Silicon the kernel
  refuses to run arm64 code with *no* signature at all** — quarantine removal does
  not help, so ad-hoc signing is mandatory (free). `macdeployqt`/Xcode usually do
  it automatically. Users then bypass the notarization gate by stripping the
  quarantine xattr the browser set:
  `xattr -dr com.apple.quarantine /Applications/mp3player.app` (or `xattr -cr`),
  or **right-click → Open** on first launch. Ship this as a "first launch on macOS"
  instruction. (Intel can run unsigned once quarantine is gone; universal builds
  still need arm64 ad-hoc signing — always sign.)
- **v2 — frictionless:** **Developer ID sign + notarize + staple** ($99/yr Apple
  Developer) so the download "just works" with no user step.

## CI (forgejo-runner)

`.forgejo/workflows/macos.yml` builds the **universal** dmg on a **self-hosted mac
runner** (`runs-on: macos`) — macOS can't use containers, so the job runs on the
host. It `brew install`s `ninja` + `utf8cpp`, installs official Qt 6.11.1 via
aqtinstall into `~/Qt` (cached; not a marketplace action, so it doesn't depend on
the instance mirroring one), builds the universal static TagLib
(`scripts/build-taglib-universal.sh`, also cached), then runs
`ARCHS="arm64;x86_64" scripts/package-macos.sh` and uploads `mp3player.dmg`. Ad-hoc
signing needs no secrets; Developer ID **notarization is still TODO** (would add a
signed/stapled, frictionless download — needs secrets on the runner).

## Smoke test checklist

Cold scan, local playback, remote import + stream (yt-dlp), playlists, settings
round-trip, **media keys / Control Center**, **notifications**, window/header state
persistence — on both arm64 and x86_64.
