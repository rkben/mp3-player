#pragma once

#include <QObject>
#include <QMediaPlayer>
#include <QList>
#include <QUrl>

#include "PlaylistModel.h"   // Track

class QAudioOutput;

// Owns the QMediaPlayer + QAudioOutput and an independent **play queue** — a
// snapshot of Tracks captured when playback starts. The queue is decoupled from
// whatever the Tracks view is currently showing, so searching/sorting/reloading
// the library doesn't disturb playback, navigation, or shuffle history. The UI
// identifies the playing track by URL (stable), not by a view row.
class PlayerController : public QObject
{
    Q_OBJECT
public:
    enum class RepeatMode { None, All, One };

    explicit PlayerController(QObject *parent = nullptr);

    bool hasTrack() const { return m_index >= 0 && m_index < m_queue.size(); }
    Track currentTrack() const { return hasTrack() ? m_queue.at(m_index) : Track{}; }
    int currentIndex() const { return m_index; }     // queue index (e.g. MPRIS id)
    int queueSize() const { return m_queue.size(); }
    const QList<Track> &queue() const { return m_queue; }
    bool isPlaying() const;
    QMediaPlayer::PlaybackState playbackState() const { return m_player->playbackState(); }
    qint64 duration() const;
    qint64 position() const;
    float volume() const;

    RepeatMode repeatMode() const { return m_repeat; }
    bool shuffle() const { return m_shuffle; }

public slots:
    // Replace the queue with `tracks` and start playing index `start`.
    void playQueue(const QList<Track> &tracks, int start);
    // Append tracks to the queue without disturbing playback. Emits queueChanged.
    void enqueue(const QList<Track> &tracks);
    // Play the queue item at `index` (e.g. double-clicking a queue row).
    void jumpTo(int index);
    // Load a queue but stay stopped (still "nothing playing"), so a later play()
    // — e.g. a media key / MPRIS Play before anything was clicked — can start it.
    // No-op while something is already playing/loaded.
    void setReadyQueue(const QList<Track> &tracks);
    void play();
    void pause();
    void stop();
    void togglePlayPause();
    void next();
    void previous();
    void setPosition(qint64 ms);
    void setVolume(float linear);          // 0.0 .. 1.0
    void setRepeatMode(RepeatMode mode);
    void setShuffle(bool on);
    // Update the current track's cover art (resolved asynchronously elsewhere).
    void setCurrentArt(const QUrl &url, const QString &artUrl);

signals:
    void currentTrackChanged(const Track &track);   // empty Track == nothing playing
    void queueChanged(const QList<Track> &queue);    // queue contents changed
    void trackError(const QString &trackName, const QString &message);
    void playbackStateChanged(bool playing);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void volumeChanged(float linear);
    void shuffleChanged(bool on);
    void repeatModeChanged(RepeatMode mode);
    // Player-resolved tags for the track at `url` (e.g. formats TagLib can't read).
    void metadataResolved(const QUrl &url, const QString &title, const QString &artist,
                          const QString &album, int trackNo, qint64 durationMs);

private slots:
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void onMetaDataChanged();
    void onErrorOccurred(QMediaPlayer::Error error, const QString &errorString);

private:
    int pickNext(bool userInitiated) const;
    void skipBadTrack(const QString &message);
    void playInternal(int qindex);   // load + play, no history side effects
    void recordHistory(int qindex);  // push onto the back/forward history

    QMediaPlayer *m_player;
    QAudioOutput *m_audio;
    QList<Track> m_queue;
    QList<Track> m_readyQueue;     // silent cold-start fallback (e.g. media keys)
    int m_index = -1;              // index into m_queue
    QList<int> m_history;          // recently played queue indices (back/forward)
    int m_historyPos = -1;
    RepeatMode m_repeat = RepeatMode::None;
    bool m_shuffle = false;
    int m_consecutiveErrors = 0;   // guards against an all-bad queue looping
    int m_lastErrorIndex = -1;     // dedupe: error + InvalidMedia fire together
    int m_metaResolvedIndex = -1;  // dedupe: metaDataChanged fires repeatedly
};
