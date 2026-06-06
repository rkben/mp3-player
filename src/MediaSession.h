#pragma once

#include <QObject>

class PlayerController;
class QWidget;

// Platform-agnostic OS media-session bridge: now-playing metadata, transport
// state, and hardware media-key handling. Concrete implementations:
//   - MprisController     — Linux (org.mpris.MediaPlayer2 over D-Bus)
//   - (future) SMTC       — Windows (SystemMediaTransportControls)
//   - (future) NowPlaying — macOS (MPNowPlayingInfoCenter)
//
// create() returns the right implementation for the platform/build, or nullptr
// when none is available — callers null-check rather than #ifdef.
class MediaSession : public QObject
{
    Q_OBJECT
public:
    explicit MediaSession(QObject *parent = nullptr) : QObject(parent) {}
    ~MediaSession() override = default;

    // Re-push the current now-playing metadata to the OS (e.g. once cover art
    // resolves asynchronously).
    virtual void refreshMetadata() = 0;

    static MediaSession *create(PlayerController *player, QWidget *window,
                                QObject *parent = nullptr);
};
