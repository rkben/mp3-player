#pragma once

#include "MediaSession.h"

class PlayerController;
class QWidget;

// macOS media-session bridge: mirrors the player into MPNowPlayingInfoCenter
// (Control Center / lock screen / now-playing widget) and routes hardware media
// keys + remote commands back through MPRemoteCommandCenter. The Linux
// counterpart is MprisController; both subclass MediaSession so PlayerController
// stays platform-agnostic.
//
// Implementation lives in NowPlayingSession.mm (Objective-C++); this header is
// pure C++ so MediaSession.cpp can include it without dragging in MediaPlayer.h.
class NowPlayingSession : public MediaSession
{
    Q_OBJECT
public:
    NowPlayingSession(PlayerController *player, QWidget *window,
                      QObject *parent = nullptr);
    ~NowPlayingSession() override;

    void refreshMetadata() override;   // re-push now-playing info (e.g. art resolved)

private slots:
    void onPlaybackStateChanged(bool playing);
    void onCurrentTrackChanged();
    void onDurationChanged(qint64 ms);

private:
    void updateNowPlayingInfo();   // rebuild the now-playing dictionary
    void updatePlaybackState();    // playing/paused/stopped + playback rate
    void setupRemoteCommands();    // wire MPRemoteCommandCenter -> player slots

    PlayerController *m_player;
    QWidget *m_window;
};
