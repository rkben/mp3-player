# CLAUDE.md

Guidance for working in this repository.

## What this is

Pocket Player — a Qt 6 **Widgets** music player (no QML/Qt Quick, no WebEngine).
Local library scanning + playback, m3u8 playlists, and remote streaming of
`yt-dlp`-supported URLs. Linux-first, with cross-platform work in progress
(see `notes/cross-platform.md`, `notes/macos.md`, `notes/windows.md`).

## Building

```sh
cmake --preset macos      # or: linux / windows
cmake --build --preset macos
./build/mp3player
```

Or plain: `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`.

On Linux the `Makefile` offers shortcuts (`make`, `make run`, `make rebuild`,
`make demo-run`).

### Build options
- `ENABLE_MPRIS` — `ON` on Linux, `OFF` elsewhere. Pulls in `Qt6::DBus`.
- `BUILD_SHADER_DEMO` — `OFF`. Standalone `QRhiWidget` shader/visualizer demo.

### Verifying changes
There are no unit tests. Verify with a headless smoke run:
```sh
timeout 6 env QT_QPA_PLATFORM=offscreen ./build/mp3player
```
GUI/audio behaviour needs a real session — do not screenshot via Xvfb/`import`,
it leaks onto the live Wayland display (Qt picks the Wayland plugin from
`WAYLAND_DISPLAY`). Ask the user to verify visuals.

**macOS:** native arm64 with **official Qt** (`~/Qt/6.11.1/macos`, installed via
aqtinstall — *not* Homebrew Qt, which breaks `macdeployqt`). Configure into a
separate `build-mac/` with that Qt's `qt-cmake`; official Qt bundles its own FFmpeg
backend, so only TagLib (Homebrew) is external. The media session (Now Playing /
media keys) can't be smoke-tested headless and only works from a **code-signed
`.app` launched via `open`**. `scripts/package-macos.sh` does configure → build →
deploy → sign → self-contained `.dmg`. Full setup in `notes/macos.md`.

## Architecture

| File | Role |
|------|------|
| `MainWindow.*` | All UI: two-tab left panel (Library tree + Remote node / Playlists), queue table, transport bar, track-info panel, context menus, settings glue |
| `PlayerController.*` | Owns the **play queue** (snapshot decoupled from the view), shuffle/repeat, history; drives `MediaEngine` and `RemoteResolver` |
| `MediaEngine.*` | `QMediaPlayer`+`QAudioOutput` on a **worker thread** so slow source opens (NFS/network) never freeze the GUI |
| `RemoteResolver.*` | Resolves a remote page URL → playable stream URL on demand via `yt-dlp -g` (async `QProcess`) |
| `MusicLibrary.*` | Worker-thread SQLite tag cache (path+mtime), parallel TagLib cold scan, FTS5 search, lazy art; `importTracks()` for remote rows |
| `PlaylistModel.*` | Sortable `QAbstractTableModel` over a flat `QList<Track>`, O(1) key→row index |
| `PlaylistStore.*` | m3u8 playlists + resumable queue cache under AppData |
| `Importer.*` / `ImportDialog.*` | yt-dlp `--dump-json` import: maps JSON → remote `Track`s, downloads covers, creates/appends playlists |
| `MediaSession.*` | Abstract OS media-session bridge; `MprisController.*` (Linux) and `NowPlayingSession.mm` (macOS, MPNowPlayingInfoCenter + MPRemoteCommandCenter) are the impls. `create()` is the platform seam (Windows SMTC slots in here) |
| `Toast.*` / `Notifier.*` | In-app toast overlay; OS notifications (D-Bus → tray → toast fallback) |
| `Theme.*`, `CoverLabel.h`, `TrackUri.h`, `ProcUtil.h` | Theming, cover image widget, URL helpers, Windows console-suppression |

## Key concepts (read before changing)

- **Track identity is a URI string**, not a local path. `Track::key()` =
  `url.toString(QUrl::FullyEncoded)` — `file://…` for local, `https://…` for
  remote. The DB primary key is `uri`; `PlaylistModel`/`MainWindow` index by key.
  Never reintroduce `toLocalFile()` as an identity. `TrackUri.h` has
  `urlFromStored()` / `storedForm()` for playlist-file round-tripping (local
  tracks stored as plain paths, remote as URLs).
- **Local vs remote rows.** `tracks.remote` flags it; remote rows have NULL
  `path`, sentinel `mtime=0`, a pre-downloaded cover, and are **never** touched by
  the folder scan/prune (scoped `WHERE remote=0`).
- **Streaming model:** store only the page URL; resolve the stream fresh per play
  (signed URLs expire). `PlayerController::playInternal` routes remote tracks
  through `RemoteResolver`, guarding the result against a mid-resolve skip.
- **Threads:** `MusicLibrary` and `MediaEngine` live on worker threads; all
  cross-thread comms are queued signals. `PlayerController` stays on the GUI
  thread and caches position/duration/state for synchronous getters (MPRIS).

## Conventions

- Match the surrounding comment density (this codebase comments the *why*).
- `QStringLiteral` for literals; arg-list `QProcess::start` (never shell strings).
- Wrap spawned processes with `suppressConsoleWindow()` (`ProcUtil.h`).
- Keep platform code behind `MediaSession` / `#ifdef`, not scattered in MainWindow.
