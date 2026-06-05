# Pocket Player
Written by Claude Code, Opus 4.8 - low effort. `/usage` estimates like 130 USD in tokens. I haven't read anything in here.



A lightweight, mobile-styled music player built with **Qt 6 Widgets** and
**Qt Multimedia**. Targets the desktop but uses a phone-shaped, touch-friendly
UI. Deliberately avoids heavy dependencies (no WebEngine, no QML/Qt Quick scene
graph) to keep CPU and power usage low.

## Features

- Open a folder and recursively load audio (`.mp3 .flac .ogg .m4a .wav`)
- Play / pause / next / previous, seek, volume
- Shuffle and repeat (off / all / one)
- Tag metadata via TagLib, cached in SQLite (path+mtime); parallel cold scan
- MPRIS D-Bus control on Linux — media keys, desktop now-playing, `playerctl`

## Build

Requires Qt 6 (Core, Gui, Widgets, Multimedia, Sql, Concurrent, DBus, SVG),
TagLib, FFmpeg (the Qt Multimedia backend), and a C++17 compiler.

### Arch Linux

```sh
sudo pacman -S --needed base-devel cmake qt6-base qt6-multimedia qt6-svg taglib ffmpeg
```

Optional: `plasma-integration` for native KDE theming (Breeze + color scheme).

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

e.g. `cmake -B build -S . -DENABLE_MPRIS=OFF`

## Layout

| File | Role |
|------|------|
| `src/main.cpp` | Entry point |
| `src/MainWindow.*` | UI: layout, transport, styling |
| `src/PlayerController.*` | Wraps `QMediaPlayer`/`QAudioOutput`, playlist navigation |
| `src/PlaylistModel.*` | `QAbstractListModel` of tracks driving the list view |
| `src/MusicLibrary.*` | Threaded SQLite tag cache + parallel TagLib scan |
| `src/MprisController.*` | MPRIS D-Bus adaptors (optional, `ENABLE_MPRIS`) |
