#pragma once

#include "MediaSession.h"

#include <QObject>
#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QVariantMap>
#include <QStringList>

class PlayerController;
class PlaylistModel;
class QWidget;
class MprisController;

// org.mpris.MediaPlayer2 — the "root" interface (identity, raise/quit).
class MprisRootAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)
public:
    explicit MprisRootAdaptor(MprisController *parent);

    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("Pocket Player"); }
    QStringList supportedUriSchemes() const { return {QStringLiteral("file")}; }
    QStringList supportedMimeTypes() const;

public slots:
    void Raise();
    void Quit();

private:
    MprisController *m_c;
};

// org.mpris.MediaPlayer2.Player — transport, status, metadata, volume.
class MprisPlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)
public:
    explicit MprisPlayerAdaptor(MprisController *parent);

    QString playbackStatus() const;
    QString loopStatus() const;
    void setLoopStatus(const QString &value);
    double rate() const { return 1.0; }
    void setRate(double) {}
    bool shuffle() const;
    void setShuffle(bool on);
    QVariantMap metadata() const;
    double volume() const;
    void setVolume(double v);
    qlonglong position() const;
    double minimumRate() const { return 1.0; }
    double maximumRate() const { return 1.0; }
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canPlay() const;
    bool canPause() const { return true; }
    bool canSeek() const { return true; }
    bool canControl() const { return true; }

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offsetUs);
    void SetPosition(const QDBusObjectPath &trackId, qlonglong positionUs);
    void OpenUri(const QString &uri);

signals:
    void Seeked(qlonglong positionUs);

private:
    MprisController *m_c;
};

// Bridges PlayerController to the MPRIS D-Bus interfaces. Owns the adaptors,
// registers the service, and pushes PropertiesChanged on state change. Reads the
// playing track from the player's queue (not the view model).
class MprisController : public MediaSession
{
    Q_OBJECT
public:
    MprisController(PlayerController *player, QWidget *window, QObject *parent = nullptr);

    bool isRegistered() const { return m_registered; }

    // Used by the adaptors:
    PlayerController *player() const { return m_player; }
    QWidget *window() const { return m_window; }
    QVariantMap metadata() const;
    QString playbackStatus() const;
    bool canGoNext() const;
    void emitSeeked(qlonglong us);
    void refreshMetadata() override;   // re-push Metadata (e.g. once art resolves)

private slots:
    void onPlaybackStateChanged();
    void onCurrentTrackChanged();
    void onDurationChanged();
    void onVolumeChanged();
    void onShuffleChanged();
    void onRepeatModeChanged();

private:
    void pushPlayerProps(const QVariantMap &changed);

    PlayerController *m_player;
    QWidget *m_window;
    MprisPlayerAdaptor *m_playerAdaptor = nullptr;
    bool m_registered = false;
};
