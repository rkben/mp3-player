#include "PlayerController.h"
#include "MediaEngine.h"

#include <QThread>
#include <QRandomGenerator>

PlayerController::PlayerController(QObject *parent)
    : QObject(parent)
    , m_engineThread(new QThread(this))
    , m_engine(new MediaEngine)   // no parent: moved to the worker thread
{
    // These cross the thread boundary via queued connections, so they must be
    // registered as metatypes (the QMediaPlayer enums and metadata value type).
    qRegisterMetaType<QMediaPlayer::MediaStatus>();
    qRegisterMetaType<QMediaPlayer::PlaybackState>();
    qRegisterMetaType<QMediaPlayer::Error>();
    qRegisterMetaType<QMediaMetaData>();

    m_engine->moveToThread(m_engineThread);
    connect(m_engineThread, &QThread::started, m_engine, &MediaEngine::init);
    connect(m_engineThread, &QThread::finished, m_engine, &QObject::deleteLater);

    // Controller -> engine (queued: runs on the worker thread that owns the player).
    connect(this, &PlayerController::engineLoad, m_engine, &MediaEngine::load);
    connect(this, &PlayerController::enginePlay, m_engine, &MediaEngine::play);
    connect(this, &PlayerController::enginePause, m_engine, &MediaEngine::pause);
    connect(this, &PlayerController::engineStop, m_engine, &MediaEngine::stop);
    connect(this, &PlayerController::engineSetPosition, m_engine, &MediaEngine::setPosition);
    connect(this, &PlayerController::engineSetVolume, m_engine, &MediaEngine::setVolume);

    // Engine -> controller (queued: marshalled back to the GUI thread). Position
    // and duration are cached for the synchronous getters and re-emitted.
    connect(m_engine, &MediaEngine::positionChanged, this, [this](qint64 ms) {
        m_position = ms;
        emit positionChanged(ms);
    });
    connect(m_engine, &MediaEngine::durationChanged, this, [this](qint64 ms) {
        m_duration = ms;
        emit durationChanged(ms);
    });
    connect(m_engine, &MediaEngine::mediaStatusChanged,
            this, &PlayerController::onMediaStatusChanged);
    connect(m_engine, &MediaEngine::metaDataChanged,
            this, &PlayerController::onMetaDataChanged);
    connect(m_engine, &MediaEngine::errorOccurred,
            this, &PlayerController::onErrorOccurred);
    connect(m_engine, &MediaEngine::playbackStateChanged,
            this, &PlayerController::onPlaybackStateChanged);

    m_engineThread->start();
}

PlayerController::~PlayerController()
{
    m_engineThread->quit();
    m_engineThread->wait();
}

void PlayerController::onPlaybackStateChanged(QMediaPlayer::PlaybackState s)
{
    m_playbackState = s;
    if (s == QMediaPlayer::PlayingState) {
        m_consecutiveErrors = 0;   // a track played -> chain is healthy
        m_lastErrorIndex = -1;
    }
    emit playbackStateChanged(s == QMediaPlayer::PlayingState);
}

void PlayerController::playInternal(int qindex)
{
    m_index = qindex;
    m_metaResolvedIndex = -1;   // a new track may re-use this slot; resolve afresh
    m_duration = 0;             // reset cached duration until the new source reports
    m_position = 0;
    emit engineLoad(m_queue.at(qindex).url, /*autoplay=*/true);
    emit currentTrackChanged(m_queue.at(qindex));
}

void PlayerController::recordHistory(int qindex)
{
    // Starting a new track drops any "forward" history we'd navigated back past.
    while (m_history.size() > m_historyPos + 1)
        m_history.removeLast();
    if (!m_history.isEmpty() && m_history.last() == qindex)
        return;   // repeat-one / replaying same track: no dup entry
    m_history.append(qindex);
    // Retain a small window so "previous" can walk back through recent plays.
    constexpr int kMaxHistory = 6;   // current + up to 5 prior
    while (m_history.size() > kMaxHistory)
        m_history.removeFirst();
    m_historyPos = m_history.size() - 1;
}

void PlayerController::playQueue(const QList<Track> &tracks, int start)
{
    m_queue = tracks;
    m_history.clear();
    m_historyPos = -1;
    m_consecutiveErrors = 0;
    m_lastErrorIndex = -1;

    emit queueChanged(m_queue);

    if (start < 0 || start >= m_queue.size()) {
        m_index = -1;
        emit engineStop();
        emit currentTrackChanged(Track{});
        return;
    }
    playInternal(start);
    recordHistory(start);
}

void PlayerController::enqueue(const QList<Track> &tracks)
{
    if (tracks.isEmpty())
        return;
    m_queue.append(tracks);
    emit queueChanged(m_queue);
}

void PlayerController::jumpTo(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;
    m_consecutiveErrors = 0;
    m_lastErrorIndex = -1;
    playInternal(index);
    recordHistory(index);
}

void PlayerController::clearQueue()
{
    if (hasTrack()) {
        // Keep the playing track so audio (and now-playing) stay valid; drop the
        // rest. It plays out, then pickNext finds nothing and playback stops.
        m_queue = {m_queue.at(m_index)};
        m_index = 0;
        m_history = {0};
        m_historyPos = 0;
    } else {
        m_queue.clear();
        m_index = -1;
        m_history.clear();
        m_historyPos = -1;
    }
    m_consecutiveErrors = 0;
    m_lastErrorIndex = -1;
    m_metaResolvedIndex = -1;
    emit queueChanged(m_queue);
}

void PlayerController::setReadyQueue(const QList<Track> &tracks)
{
    // A silent fallback only: play() adopts it when the real queue is empty.
    // Never touches m_queue, so enqueue() builds on an empty queue, not this.
    m_readyQueue = tracks;
}

void PlayerController::play()
{
    if (m_playbackState == QMediaPlayer::PlayingState)
        return;
    if (hasTrack()) {
        emit enginePlay();   // resume a paused track
        return;
    }
    // Cold start: play the queue if the user built one, else fall back to the
    // ready queue (e.g. media-key Play before anything was enqueued).
    if (m_queue.isEmpty()) {
        if (m_readyQueue.isEmpty())
            return;
        m_queue = m_readyQueue;
        emit queueChanged(m_queue);
    }
    playInternal(0);
    recordHistory(0);
}

void PlayerController::pause() { emit enginePause(); }
void PlayerController::stop() { emit engineStop(); }

void PlayerController::togglePlayPause()
{
    if (m_playbackState == QMediaPlayer::PlayingState)
        emit enginePause();
    else
        play();
}

void PlayerController::next()
{
    // If we'd walked back into history, step forward through it before picking new.
    if (m_historyPos >= 0 && m_historyPos + 1 < m_history.size()) {
        ++m_historyPos;
        playInternal(m_history.at(m_historyPos));
        return;
    }
    const int n = pickNext(/*userInitiated=*/true);
    if (n >= 0) {
        playInternal(n);
        recordHistory(n);
    }
}

void PlayerController::previous()
{
    if (m_queue.isEmpty())
        return;
    // Restart current track if we're more than a few seconds in (common player
    // behaviour: "previous" first rewinds, then steps back).
    constexpr qint64 kRestartThresholdMs = 3000;
    if (m_position > kRestartThresholdMs) {
        m_position = 0;
        emit engineSetPosition(0);
        return;
    }
    // Walk back through actual play history — this is what makes "previous" work
    // sensibly in shuffle (return to the song you just heard, not a new random).
    if (m_historyPos > 0) {
        --m_historyPos;
        playInternal(m_history.at(m_historyPos));
        return;
    }
    if (m_shuffle)
        return;   // no history yet: nothing sensible to go back to in shuffle
    int prev = m_index - 1;
    if (prev < 0)
        prev = (m_repeat == RepeatMode::All) ? m_queue.size() - 1 : 0;
    playInternal(prev);
    recordHistory(prev);
}

void PlayerController::setPosition(qint64 ms)
{
    m_position = ms;   // optimistic; the engine confirms via positionChanged
    emit engineSetPosition(ms);
}

void PlayerController::setVolume(float linear)
{
    if (qFuzzyCompare(m_volume, linear))
        return;
    m_volume = linear;
    emit engineSetVolume(linear);
    emit volumeChanged(linear);
}

void PlayerController::setRepeatMode(RepeatMode mode)
{
    if (m_repeat == mode)
        return;
    m_repeat = mode;
    emit repeatModeChanged(mode);
}

void PlayerController::setShuffle(bool on)
{
    if (m_shuffle == on)
        return;
    m_shuffle = on;
    emit shuffleChanged(on);
}

void PlayerController::setCurrentArt(const QUrl &url, const QString &artUrl)
{
    if (!hasTrack() || m_queue.at(m_index).url != url)
        return;
    if (m_queue.at(m_index).artUrl == artUrl)
        return;   // unchanged — avoid a redundant currentTrackChanged (re-request loop)
    m_queue[m_index].artUrl = artUrl;
    emit currentTrackChanged(m_queue.at(m_index));   // let MPRIS pick up the art
}

int PlayerController::pickNext(bool userInitiated) const
{
    const int n = m_queue.size();
    if (n == 0)
        return -1;

    if (m_repeat == RepeatMode::One && !userInitiated)
        return m_index;

    if (m_shuffle) {
        if (n == 1)
            return 0;
        int r = m_index;
        while (r == m_index)
            r = QRandomGenerator::global()->bounded(n);
        return r;
    }

    int next = m_index + 1;
    if (next >= n)
        return (m_repeat == RepeatMode::All) ? 0 : -1;
    return next;
}

void PlayerController::onMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    if (status == QMediaPlayer::EndOfMedia) {
        m_consecutiveErrors = 0;   // reached the end cleanly
        const int n = pickNext(/*userInitiated=*/false);
        if (n >= 0) {
            playInternal(n);
            recordHistory(n);
        }
    } else if (status == QMediaPlayer::InvalidMedia) {
        // Unsupported codec / corrupt file: don't let it stall the queue.
        skipBadTrack(tr("Unsupported or corrupt file"));
    }
}

void PlayerController::onErrorOccurred(QMediaPlayer::Error error, const QString &errorString)
{
    if (error != QMediaPlayer::NoError)
        skipBadTrack(errorString);
}

void PlayerController::skipBadTrack(const QString &message)
{
    // errorOccurred and InvalidMedia both fire for one bad file: handle once.
    if (!hasTrack() || m_index == m_lastErrorIndex)
        return;
    m_lastErrorIndex = m_index;

    emit trackError(m_queue.at(m_index).title, message);

    // Bail out if everything is failing, so we don't spin through a dead queue.
    if (++m_consecutiveErrors >= m_queue.size()) {
        emit engineStop();
        return;
    }
    const int n = pickNext(/*userInitiated=*/true);   // always move forward
    if (n >= 0) {
        playInternal(n);
        recordHistory(n);
    }
}

void PlayerController::onMetaDataChanged(const QMediaMetaData &md)
{
    if (!hasTrack())
        return;
    // The player uses the same FFmpeg backend as the scanner, so this fills in
    // metadata for formats TagLib couldn't tag (e.g. .tta). Persisted by the UI.
    const QString title = md.stringValue(QMediaMetaData::Title);
    QString artist = md.stringValue(QMediaMetaData::ContributingArtist);
    if (artist.isEmpty())
        artist = md.stringValue(QMediaMetaData::AlbumArtist);
    const QString album = md.stringValue(QMediaMetaData::AlbumTitle);
    const int trackNo = md.value(QMediaMetaData::TrackNumber).toInt();
    const qint64 durationMs = md.value(QMediaMetaData::Duration).toLongLong();

    if (title.isEmpty() && artist.isEmpty() && album.isEmpty())
        return;   // nothing useful to surface

    // Fill the queue copy so the now-playing display and MPRIS reflect it too.
    // (Don't fold duration/trackNo into the queue Track — they're surfaced via
    // metadataResolved/the DB, not the now-playing labels.)
    Track &t = m_queue[m_index];
    const bool changed = t.mergeFrom(title, artist, album, /*dur=*/0, /*tno=*/0);
    if (changed)
        emit currentTrackChanged(t);

    // metaDataChanged can fire several times per track; only persist when the
    // track changed or its tags actually moved, so we don't spam the DB writer.
    if (!changed && m_metaResolvedIndex == m_index)
        return;
    m_metaResolvedIndex = m_index;
    emit metadataResolved(t.url, title, artist, album, trackNo, durationMs);
}
