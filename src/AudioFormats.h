#pragma once

#include <QStringList>

// The audio extensions the QMediaPlayer FFmpeg backend can decode and TagLib can
// (mostly) tag. QDir name filters are case-insensitive, so .MP3/.FLAC match too.
// Files TagLib can't read still play — they just fall back to a filename title.
//
// Single source of truth shared by the library scanner (MusicLibrary) and the
// browse tree / enqueue paths (MainWindow), so the two can never drift.
inline const QStringList &audioGlobs()
{
    static const QStringList globs{
        "*.mp3", "*.flac", "*.ogg", "*.oga", "*.opus", "*.m4a", "*.m4b", "*.aac",
        "*.wav", "*.aiff", "*.aif", "*.wma", "*.ape", "*.wv", "*.mpc", "*.mp2",
        "*.spx", "*.tta", "*.dsf", "*.ac3"};
    return globs;
}
