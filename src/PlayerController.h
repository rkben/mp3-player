#pragma once

#include <QObject>
#include <QMediaPlayer>
#include <QMediaMetaData>
#include <QList>
#include <QUrl>

#include "PlaylistModel.h"   // Track

class MediaEngine;
class RemoteResolver;
class QThread;

// Owns an independent **play queue** — a snapshot of Tracks captured when
// playback starts. The queue is decoupled from whatever the Tracks view is
// currently showing, so searching/sorting/reloading the library doesn't disturb
// playback, navigation, or shuffle history. The UI identifies the playing track
// by URL (stable), not by a view row.
//
// The actual QMediaPlayer lives on a worker thread inside MediaEngine, so a slow
// source open (e.g. over NFS) never blocks the GUI. This controller stays on the
// GUI thread: it drives the engine via queued signals and caches the latest
// position/duration/state/volume so the synchronous getters (used by MPRIS) keep
// working without reaching across the thread boundary.
class PlayerController : public QObject
{
    Q_OBJECT
public:
    enum class RepeatMode { None, All, One };

    explicit PlayerController(QObject *parent = nullptr);
    ~PlayerController() override;

    // The current track is tracked independently of the queue: it may be playing
    // while detached (m_index == -1) after the queue was cleared. m_current is the
    // source of truth for "what's playing"; m_index only locates it in the queue.
    bool hasTrack() const { return !m_current.url.isEmpty(); }
    Track currentTrack() const { return m_current; }
    int currentIndex() const { return m_index; }     // queue index, or -1 if detached
    int queueSize() const { return m_queue.size(); }
    const QList<Track> &queue() const { return m_queue; }
    bool isPlaying() const { return m_playbackState == QMediaPlayer::PlayingState; }
    QMediaPlayer::PlaybackState playbackState() const { return m_playbackState; }
    qint64 duration() const { return m_duration; }
    qint64 position() const { return m_position; }
    float volume() const { return m_volume; }

    RepeatMode repeatMode() const { return m_repeat; }
    bool shuffle() const { return m_shuffle; }

public slots:
    // Replace the queue with `tracks` and start playing index `start`.
    void playQueue(const QList<Track> &tracks, int start);
    // Append tracks to the queue without disturbing playback. Emits queueChanged.
    void enqueue(const QList<Track> &tracks);
    // Play the queue item at `index` (e.g. double-clicking a queue row).
    void jumpTo(int index);
    // Empty the queue. If a track is playing it is kept (as the sole entry) and
    // keeps playing; when it ends the queue is exhausted and playback stops.
    void clearQueue();
    // Collapse duplicates in the active queue, in place. Always drops exact-URI
    // dups (keeps the first); when Prefer-HQ is on it additionally collapses same
    // artist+title copies to the best one. The current track is preserved. Emits
    // queueChanged if anything was removed. Returns the number of tracks removed.
    int dedupeQueue();
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
    // Route audio to a specific output device (QAudioDevice::id()); empty = default.
    void setAudioDevice(const QByteArray &id);
    void setRepeatMode(RepeatMode mode);
    void setShuffle(bool on);
    // Prefer-HQ dedup mode applied when a set is enqueued: 0=off, 1=naive (format),
    // 2=naive + bitrate tiebreak. Cached; affects future enqueues only.
    void setPreferHq(int mode) { m_preferHq = mode; }
    // Update the current track's cover art (resolved asynchronously elsewhere).
    void setCurrentArt(const QUrl &url, const QString &artUrl);
    // Enable/disable the audio-buffer capture that feeds the visualizer. Off
    // unless the visualizer is shown, so the album-art path stays zero-overhead.
    void setVisualizerActive(bool on) { emit engineSetVisualizerActive(on); }

signals:
    void currentTrackChanged(const Track &track);   // empty Track == nothing playing
    void queueChanged(const QList<Track> &queue);    // queue contents changed
    void trackError(const QString &trackName, const QString &message);
    void playbackStateChanged(bool playing);
    // True while yt-dlp is resolving a remote track's stream URL (which can take a
    // second); false once it resolves/fails or a local track is loaded. Drives a
    // "Fetching remote track…" status hint.
    void remoteResolving(bool active);
    // True while the *next* remote track's stream URL is being resolved ahead of time
    // (a few seconds before the current one ends); drives a "Prefetching…" status hint.
    void remotePrefetching(bool active);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void volumeChanged(float linear);
    void shuffleChanged(bool on);
    void repeatModeChanged(RepeatMode mode);
    // Smoothed [0..1] loudness for the visualizer (relayed from the engine).
    void amplitudeChanged(float amplitude);
    // 64 smoothed [0..1] frequency bands for the visualizer (relayed from the engine).
    void spectrumChanged(const QList<float> &bands);
    // Player-resolved tags for the track at `url` (e.g. formats TagLib can't read).
    void metadataResolved(const QUrl &url, const QString &title, const QString &artist,
                          const QString &album, int trackNo, qint64 durationMs);

    // Controller -> MediaEngine (queued; the engine runs on the worker thread).
    void engineLoad(const QUrl &url, bool autoplay);
    void enginePlay();
    void enginePause();
    void engineStop();
    void engineSetPosition(qint64 ms);
    void engineSetVolume(float linear);
    void engineSetAudioDevice(const QByteArray &id);
    void engineSetVisualizerActive(bool on);

private slots:
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void onMetaDataChanged(const QMediaMetaData &md);
    void onErrorOccurred(QMediaPlayer::Error error, const QString &errorString);
    void onPlaybackStateChanged(QMediaPlayer::PlaybackState state);

private:
    int pickNext(bool userInitiated) const;
    void skipBadTrack(const QString &message);
    void playInternal(int qindex);   // load + play, no history side effects
    void recordHistory(int qindex);  // push onto the back/forward history
    void decideAutoNext();           // cache the index end-of-track will advance to
    void maybePrefetch();            // resolve the next remote stream URL ahead of time

    QThread *m_engineThread;
    MediaEngine *m_engine;
    RemoteResolver *m_resolver;   // resolves remote page URLs to stream URLs on play
    QMediaPlayer::PlaybackState m_playbackState = QMediaPlayer::StoppedState;
    qint64 m_position = 0;    // cached from the engine for synchronous getters
    qint64 m_duration = 0;
    float m_volume = 0.8f;
    QList<Track> m_queue;
    QList<Track> m_readyQueue;     // silent cold-start fallback (e.g. media keys)
    Track m_current;               // the playing track; survives a queue clear
    int m_index = -1;              // m_current's index into m_queue, or -1 if detached
    QList<int> m_history;          // recently played queue indices (back/forward)
    int m_historyPos = -1;
    int m_autoNext = -1;           // index end-of-track advances to (decided once/track)
    QUrl m_prefetchUrl;            // page URL being/already prefetched (empty = none)
    QUrl m_prefetchStream;         // its resolved stream URL (non-empty == ready)
    bool m_prefetchTriggered = false;   // one prefetch attempt per track
    RepeatMode m_repeat = RepeatMode::None;
    bool m_shuffle = false;
    int m_consecutiveErrors = 0;   // guards against an all-bad queue looping
    int m_preferHq = 0;            // Prefer-HQ dedup mode (0=off/1=naive/2=bitrate)
    QUrl m_lastErrorUrl;           // dedupe: error + InvalidMedia fire together (by track)
    QUrl m_metaResolvedUrl;        // dedupe: metaDataChanged fires repeatedly (by track)
};
