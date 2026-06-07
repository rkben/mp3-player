# Pocket Player
Written by Claude Code, Opus 4.8 - low effort. `/usage` estimates like 250 USD in tokens. I haven't read anything in here.


A media player.


## Features

- Open folders and recursively load audio (`.mp3 .flac .ogg .m4a .wav`)
- Play / pause / next / previous, seek, volume
- Shuffle and repeat (off / all / one)
- Tag metadata via TagLib, cached in SQLite (path+mtime); parallel cold scan
- m3u8 playlists — create, import, save, append
- Remote streaming: import tracks/playlists from any `yt-dlp`-supported URL;
  the stream is resolved on demand at play time
- MPRIS D-Bus control on Linux
- MediaSession on MacOS

## Build

Requires Qt 6 (Core, Gui, Widgets, Multimedia, Sql, Concurrent, Network, SVG,
and DBus on Linux), TagLib, FFmpeg (the Qt Multimedia backend), and a C++17
compiler. `yt-dlp` is an optional runtime dependency for remote streaming.

### Arch Linux

```sh
sudo pacman -S --needed base-devel cmake qt6-base qt6-multimedia qt6-svg taglib ffmpeg
```

Optional: `yt-dlp` for remote streaming/import; `plasma-integration` for native
KDE theming (Breeze + color scheme).

### Compile

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mp3player
```

### Build options

| Option | Default | Effect |
|--------|---------|--------|
| `ENABLE_MPRIS` | `ON` (Linux), `OFF` elsewhere | MPRIS D-Bus media control. Pulls in `Qt6::DBus`; off = no D-Bus dependency. |
| `ENABLE_DISCORD_RPC` | `ON` | Discord Rich Presence (now-playing status). Cross-platform, no extra dependency (talks Discord's local IPC socket via `QLocalSocket`). |
| `DISCORD_APP_ID` | Pocket Player's app | Discord application/client ID baked into the build. Override to use your own app; can also be overridden at runtime in Settings → Discord. |
| `BUILD_SHADER_DEMO` | `OFF` | Build the standalone `QRhiWidget` shader/visualizer demo. |

e.g. `cmake -B build -S . -DENABLE_MPRIS=OFF -DENABLE_DISCORD_RPC=OFF`

## Layout

| File | Role |
|------|------|
| `src/main.cpp` | Entry point |
| `src/MainWindow.*` | UI: layout, transport, styling |
| `src/PlayerController.*` | Wraps `QMediaPlayer`/`QAudioOutput`, playlist navigation |
| `src/PlaylistModel.*` | `QAbstractListModel` of tracks driving the list view |
| `src/MusicLibrary.*` | Threaded SQLite tag cache + parallel TagLib scan |
| `src/MprisController.*` | MPRIS D-Bus adaptors (optional, `ENABLE_MPRIS`) |

## License

Released into the public domain (or under the BSD Zero Clause License) — final
license to be decided.

## Attribution

Built with:

- **Qt 6** — LGPLv3 — <https://www.qt.io/>
- **TagLib** — LGPLv2.1 / MPL 1.1 — <https://taglib.org/>
- **FFmpeg** — LGPLv2.1+ (Qt Multimedia backend) — <https://ffmpeg.org/>

`album_icon.svg` by Pymouss, own work, Public Domain —
[commons.wikimedia.org](https://commons.wikimedia.org/w/index.php?curid=5793388)
