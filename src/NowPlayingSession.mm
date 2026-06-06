#include "NowPlayingSession.h"
#include "PlayerController.h"
#include "PlaylistModel.h"

#include <QWidget>
#include <QUrl>
#include <QString>
#include <QLoggingCategory>

#import <MediaPlayer/MediaPlayer.h>
#import <AppKit/AppKit.h>

// Diagnostic channel for the macOS media session — enable with
//   QT_LOGGING_RULES="pocketplayer.nowplaying=true"
// to trace registration + incoming remote commands.
Q_LOGGING_CATEGORY(lcNowPlaying, "pocketplayer.nowplaying")

NowPlayingSession::NowPlayingSession(PlayerController *player, QWidget *window,
                                     QObject *parent)
    : MediaSession(parent), m_player(player), m_window(window)
{
    connect(m_player, &PlayerController::playbackStateChanged,
            this, &NowPlayingSession::onPlaybackStateChanged);
    connect(m_player, &PlayerController::currentTrackChanged,
            this, &NowPlayingSession::onCurrentTrackChanged);
    connect(m_player, &PlayerController::durationChanged,
            this, &NowPlayingSession::onDurationChanged);

    setupRemoteCommands();
    updateNowPlayingInfo();
    updatePlaybackState();
    qCInfo(lcNowPlaying) << "session active (bundle id"
                         << NSBundle.mainBundle.bundleIdentifier.UTF8String << ")";
}

NowPlayingSession::~NowPlayingSession()
{
    // Clear our now-playing entry so a stale track doesn't linger in Control
    // Center after the app quits, and drop the command handlers.
    MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = nil;
    MPNowPlayingInfoCenter.defaultCenter.playbackState = MPNowPlayingPlaybackStateStopped;

    MPRemoteCommandCenter *cc = MPRemoteCommandCenter.sharedCommandCenter;
    for (MPRemoteCommand *cmd : @[ cc.playCommand, cc.pauseCommand,
                                   cc.togglePlayPauseCommand, cc.nextTrackCommand,
                                   cc.previousTrackCommand, cc.stopCommand,
                                   cc.changePlaybackPositionCommand ])
        [cmd removeTarget:nil];
}

void NowPlayingSession::setupRemoteCommands()
{
    MPRemoteCommandCenter *cc = MPRemoteCommandCenter.sharedCommandCenter;
    PlayerController *player = m_player;

    // Register a transport command: log it (for diagnosing media-key routing) and
    // marshal the action onto the GUI thread. MPRemoteCommandCenter handlers fire
    // on the main thread (== Qt GUI thread here), but the queued invocation keeps
    // us correct regardless and matches how the rest of the app drives the player.
    //
    // `action` is a capture-free function pointer and `p` is a fresh by-value
    // local, so the heap-copied block holds only plain values — never a reference
    // into this (returning) stack frame, which would dangle once setup returns.
    using Action = void (*)(PlayerController *);
    auto bind = [](MPRemoteCommand *cmd, const char *name,
                   PlayerController *p, Action action) {
        [cmd addTargetWithHandler:^(MPRemoteCommandEvent *) {
            qCInfo(lcNowPlaying) << "remote command:" << name;
            QMetaObject::invokeMethod(p, [p, action] { action(p); },
                                      Qt::QueuedConnection);
            return MPRemoteCommandHandlerStatusSuccess;
        }];
    };

    bind(cc.playCommand,            "play",     player, [](PlayerController *p) { p->play(); });
    bind(cc.pauseCommand,           "pause",    player, [](PlayerController *p) { p->pause(); });
    bind(cc.togglePlayPauseCommand, "toggle",   player, [](PlayerController *p) { p->togglePlayPause(); });
    bind(cc.stopCommand,            "stop",     player, [](PlayerController *p) { p->stop(); });
    bind(cc.nextTrackCommand,       "next",     player, [](PlayerController *p) { p->next(); });
    bind(cc.previousTrackCommand,   "previous", player, [](PlayerController *p) { p->previous(); });

    // Scrubbing from the Control Center timeline (gated with the rest below).
    [cc.changePlaybackPositionCommand addTargetWithHandler:^(MPRemoteCommandEvent *e) {
        auto *pe = (MPChangePlaybackPositionCommandEvent *)e;
        const qint64 ms = (qint64)(pe.positionTime * 1000.0);   // seconds -> ms
        qCInfo(lcNowPlaying) << "remote command: seek" << ms << "ms";
        QMetaObject::invokeMethod(player, [player, ms] { player->setPosition(ms); },
                                  Qt::QueuedConnection);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
}

void NowPlayingSession::updateNowPlayingInfo()
{
    if (!m_player->hasTrack()) {
        MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = nil;
        return;
    }

    const Track t = m_player->currentTrack();
    NSMutableDictionary *info = [NSMutableDictionary dictionary];

    auto setStr = [&](NSString *key, const QString &v) {
        if (!v.isEmpty())
            info[key] = v.toNSString();
    };
    setStr(MPMediaItemPropertyTitle, t.title);
    setStr(MPMediaItemPropertyArtist, t.artist);
    setStr(MPMediaItemPropertyAlbumTitle, t.album);
    if (t.trackNo > 0)
        info[MPMediaItemPropertyAlbumTrackNumber] = @(t.trackNo);

    const qint64 durMs = m_player->duration();
    if (durMs > 0)
        info[MPMediaItemPropertyPlaybackDuration] = @(durMs / 1000.0);
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(m_player->position() / 1000.0);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(m_player->isPlaying() ? 1.0 : 0.0);

    // Cover art: artUrl is a file:// URL to a local image (covers for remote
    // tracks are pre-downloaded). Load it into an MPMediaItemArtwork; skip if it
    // isn't a readable local file.
    if (!t.artUrl.isEmpty()) {
        const QUrl artUrl(t.artUrl);
        const QString localPath = artUrl.isLocalFile() ? artUrl.toLocalFile() : QString();
        if (!localPath.isEmpty()) {
            NSImage *img = [[NSImage alloc] initWithContentsOfFile:localPath.toNSString()];
            if (img) {
                MPMediaItemArtwork *art = [[MPMediaItemArtwork alloc]
                    initWithBoundsSize:img.size
                        requestHandler:^NSImage *(CGSize) { return img; }];
                info[MPMediaItemPropertyArtwork] = art;
            }
        }
    }

    MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = info;
    qCInfo(lcNowPlaying) << "now-playing set:" << t.title << "/" << t.artist
                         << "dur(ms)" << durMs << "art" << (info[MPMediaItemPropertyArtwork] != nil);
}

void NowPlayingSession::updatePlaybackState()
{
    MPNowPlayingInfoCenter *center = MPNowPlayingInfoCenter.defaultCenter;
    switch (m_player->playbackState()) {
    case QMediaPlayer::PlayingState:
        center.playbackState = MPNowPlayingPlaybackStatePlaying; break;
    case QMediaPlayer::PausedState:
        center.playbackState = MPNowPlayingPlaybackStatePaused; break;
    default:
        center.playbackState = MPNowPlayingPlaybackStateStopped; break;
    }

    // Commands are only meaningful once there's a queue.
    const BOOL hasQueue = m_player->queueSize() > 0;
    MPRemoteCommandCenter *cc = MPRemoteCommandCenter.sharedCommandCenter;
    cc.playCommand.enabled = hasQueue;
    cc.pauseCommand.enabled = hasQueue;
    cc.togglePlayPauseCommand.enabled = hasQueue;
    cc.nextTrackCommand.enabled = hasQueue;
    cc.previousTrackCommand.enabled = hasQueue;
    cc.stopCommand.enabled = hasQueue;
    cc.changePlaybackPositionCommand.enabled = hasQueue;
}

void NowPlayingSession::refreshMetadata()
{
    updateNowPlayingInfo();
}

void NowPlayingSession::onPlaybackStateChanged(bool)
{
    // Re-stamp elapsed time + rate so the system's extrapolated clock stays in
    // sync across play/pause transitions, then flip the published state.
    updateNowPlayingInfo();
    updatePlaybackState();
}

void NowPlayingSession::onCurrentTrackChanged()
{
    updateNowPlayingInfo();
    updatePlaybackState();   // refresh command enablement as the queue changes
}

void NowPlayingSession::onDurationChanged(qint64)
{
    updateNowPlayingInfo();   // PlaybackDuration just resolved
}
